// Read-only artifact/receipt inspection. No process execution, native loading,
// graphics, compiler, or network. File-format inspection is not an ABI shim.
import { existsSync, lstatSync, readFileSync, readdirSync, realpathSync } from 'node:fs';
import path from 'node:path';
import { inflateRawSync } from 'node:zlib';
import { artifactDigest, sha256 } from './artifact.mjs';
import { sourceState } from './source-state.mjs';

const targets = {
  ios_sim_arm64: '//:valdi_runtime_ios_sim',
  ios_device_arm64: '//:valdi_runtime_ios_device',
  android_arm64: '//:valdi_runtime_android',
};
function requireValue(condition, message) {
  if (!condition) throw new Error(message);
}
function evidenceFile(input) {
  const file = realpathSync(input);
  requireValue(lstatSync(file).isFile(), `Evidence is not a file: ${file}`);
  const bytes = readFileSync(file);
  requireValue(bytes.length > 0, `Empty evidence: ${file}`);
  return { path: file, sha256: sha256(bytes) };
}
function within(file, directory) {
  return file.startsWith(realpathSync(directory) + path.sep);
}
function range(buffer, offset, length) {
  requireValue(Number.isSafeInteger(offset) && Number.isSafeInteger(length) &&
    offset >= 0 && length >= 0 && offset + length <= buffer.length, 'Truncated artifact metadata');
  return buffer.subarray(offset, offset + length);
}

// AARs use ordinary ZIP central directories. Stop on ZIP64/encryption/unknown
// compression instead of accepting an unchecked runtime. Only read ELF header.
function aarLibraries(file) {
  const zip = readFileSync(file);
  let end = -1;
  for (let i = zip.length - 22; i >= Math.max(0, zip.length - 65557); i--)
    if (zip.readUInt32LE(i) === 0x06054b50) { end = i; break; }
  requireValue(end >= 0, 'Runtime AAR has no ZIP end record');
  requireValue(zip.readUInt16LE(end + 4) === 0 && zip.readUInt16LE(end + 6) === 0, 'Split ZIP not supported');
  const count = zip.readUInt16LE(end + 10);
  requireValue(count < 65535, 'ZIP64 runtime AAR requires parent inspection support');
  let cursor = zip.readUInt32LE(end + 16);
  const entries = new Map();
  for (let i = 0; i < count; i++) {
    const header = range(zip, cursor, 46);
    requireValue(header.readUInt32LE(0) === 0x02014b50, 'Malformed ZIP central entry');
    const flags = header.readUInt16LE(8);
    const method = header.readUInt16LE(10);
    const packed = header.readUInt32LE(20);
    const unpacked = header.readUInt32LE(24);
    const nameLength = header.readUInt16LE(28);
    const name = range(zip, cursor + 46, nameLength).toString('utf8');
    requireValue(!entries.has(name), `Duplicate AAR entry ${name}`);
    entries.set(name, { flags, method, packed, unpacked, offset: header.readUInt32LE(42) });
    cursor += 46 + nameLength + header.readUInt16LE(30) + header.readUInt16LE(32);
  }
  requireValue(entries.has('classes.jar') && entries.has('AndroidManifest.xml'), 'Not a real Android runtime AAR');
  const libraries = [...entries.keys()].filter(name => /^jni\/[^/]+\/[^/]+\.so$/.test(name));
  requireValue(libraries.length > 0 && libraries.every(name => name.startsWith('jni/arm64-v8a/')),
    'AAR must contain only the requested Android arm64 native slice');
  const name = 'jni/arm64-v8a/libvaldi_runtime.so';
  const entry = entries.get(name);
  requireValue(entry && !(entry.flags & 1) && entry.unpacked > 64 && entry.unpacked < 1024 ** 3,
    'AAR lacks an ordinary unencrypted libvaldi_runtime.so');
  const local = range(zip, entry.offset, 30);
  requireValue(local.readUInt32LE(0) === 0x04034b50, 'Malformed local ZIP entry');
  const data = range(zip, entry.offset + 30 + local.readUInt16LE(26) + local.readUInt16LE(28), entry.packed);
  const elf = entry.method === 0 ? data : entry.method === 8
    ? inflateRawSync(data, { maxOutputLength: entry.unpacked }) : undefined;
  requireValue(elf && elf.length === entry.unpacked && elf.subarray(0, 4).equals(Buffer.from([127, 69, 76, 70])),
    'Runtime entry is not ELF');
  requireValue(elf[4] === 2 && elf[5] === 1 && elf.readUInt16LE(16) === 3 && elf.readUInt16LE(18) === 183,
    'Runtime must be an Android AArch64 ELF shared object');
  return { kind: 'aar', nativeLibrary: name, architecture: 'aarch64', libraries };
}

function machoPlatform(file) {
  let bytes = readFileSync(file);
  requireValue(bytes.length >= 32, 'Truncated framework executable');
  const magic = bytes.readUInt32BE(0);
  if (magic === 0xcafebabe || magic === 0xcafebabf) {
    requireValue(bytes.readUInt32BE(4) === 1 && bytes.readUInt32BE(8) === 0x0100000c,
      'Framework fat binary must contain exactly arm64');
    const wide = magic === 0xcafebabf;
    const offset = wide ? Number(bytes.readBigUInt64BE(16)) : bytes.readUInt32BE(16);
    const size = wide ? Number(bytes.readBigUInt64BE(24)) : bytes.readUInt32BE(20);
    bytes = range(bytes, offset, size);
  }
  requireValue(bytes.length >= 32 && bytes.readUInt32LE(0) === 0xfeedfacf &&
    bytes.readUInt32LE(4) === 0x0100000c && bytes.readUInt32LE(12) === 6,
    'Framework must contain a real arm64 Mach-O dynamic library');
  let cursor = 32;
  const platforms = [];
  const commands = bytes.readUInt32LE(16);
  for (let i = 0; i < commands; i++) {
    const header = range(bytes, cursor, 8);
    const size = header.readUInt32LE(4);
    requireValue(size >= 8, 'Malformed Mach-O command size');
    const command = range(bytes, cursor, size);
    if (header.readUInt32LE(0) === 0x32) {
      requireValue(size >= 24, 'Malformed LC_BUILD_VERSION');
      platforms.push(command.readUInt32LE(8));
    }
    cursor += size;
  }
  requireValue(platforms.length === 1, 'Framework requires one explicit LC_BUILD_VERSION platform');
  return platforms[0];
}

export function inspectRuntime(runtime, profile, owned) {
  runtime = realpathSync(runtime);
  requireValue(within(runtime, path.join(owned, '.local')), 'Runtime must be an app-local experimental export');
  requireValue(!runtime.includes('/node-diagnostics-'), 'Compile-only outputs cannot be bound as a native runtime');
  if (profile === 'android_arm64') {
    requireValue(runtime.endsWith('.aar') && lstatSync(runtime).isFile(), 'Expected an exported Android AAR');
    return aarLibraries(runtime);
  }
  requireValue(profile === 'ios_sim_arm64' || profile === 'ios_device_arm64', 'Unsupported runtime profile');
  requireValue(runtime.endsWith('.xcframework') && lstatSync(runtime).isDirectory(), 'Expected an exported XCFramework directory');
  function checkLinks(directory) {
    for (const name of readdirSync(directory)) {
      const entry = path.join(directory, name);
      const stat = lstatSync(entry);
      if (stat.isSymbolicLink())
        requireValue(within(realpathSync(entry), runtime), 'Framework symlink escapes the experimental artifact');
      else if (stat.isDirectory()) checkLinks(entry);
    }
  }
  checkLinks(runtime);
  const xml = readFileSync(path.join(runtime, 'Info.plist'), 'utf8');
  requireValue(xml.startsWith('<?xml'), 'XCFramework Info.plist must be XML; normalize a parent-owned copy before hashing');
  const libraries = /<key>AvailableLibraries<\/key>\s*<array>([\s\S]*)<\/array>/.exec(xml)?.[1];
  const slices = libraries ? [...libraries.matchAll(/<dict>([\s\S]*?)<\/dict>/g)].map(match => match[1]) : [];
  requireValue(slices.length === 1, 'Restricted export requires exactly one XCFramework slice');
  const slice = slices[0];
  const value = key => new RegExp(`<key>${key}</key>\\s*<string>([^<]+)</string>`).exec(slice)?.[1];
  const architectures = /<key>SupportedArchitectures<\/key>\s*<array>([\s\S]*?)<\/array>/.exec(slice)?.[1];
  const arch = architectures ? [...architectures.matchAll(/<string>([^<]+)<\/string>/g)].map(match => match[1]) : [];
  const identifier = value('LibraryIdentifier');
  requireValue(identifier && /^[A-Za-z0-9_-]+$/.test(identifier) &&
    value('LibraryPath') === 'ValdiRuntime.framework' && value('SupportedPlatform') === 'ios' &&
    arch.length === 1 && arch[0] === 'arm64', 'Incorrect XCFramework library identity/architectures');
  const simulator = profile === 'ios_sim_arm64';
  requireValue(value('SupportedPlatformVariant') === (simulator ? 'simulator' : undefined),
    'XCFramework device/simulator variant mismatch');
  const binary = path.join(runtime, identifier, 'ValdiRuntime.framework/ValdiRuntime');
  requireValue(machoPlatform(binary) === (simulator ? 7 : 2), 'Actual Mach-O platform differs from the selected SDK');
  const headers = path.join(runtime, identifier, 'ValdiRuntime.framework/Headers');
  for (const name of ['SCValdiRuntimeManager.h', 'SCValdiComponentContainerView.h',
    'SCValdiCustomModuleProvider.h', 'SCValdiMarshallableObject.h'])
    requireValue(existsSync(path.join(headers, name)), `Missing real runtime header ${name}`);
  return { kind: 'xcframework', identifier, architecture: 'arm64', platform: simulator ? 'ios-simulator' : 'ios' };
}

export function bindingEvidence({ sourceRoot, sourceRevision, sourceStateSha256, profile, safetyFile, buildLog, graphFile }) {
  const safetyEvidence = evidenceFile(safetyFile);
  const safety = JSON.parse(readFileSync(safetyEvidence.path, 'utf8'));
  requireValue(safety.schema === 1 && safety.kind === 'tgfx-native-safety' && safety.status === 'cleared' &&
    realpathSync(safety.sourceRoot) === sourceRoot && safety.sourceRevision === sourceRevision &&
    safety.sourceStateSha256 === sourceStateSha256 && safety.profiles?.includes(profile) &&
    Array.isArray(safety.blockers) && safety.blockers.length === 0 &&
    Array.isArray(safety.evidence) && safety.evidence.length > 0,
    'Native safety review has not cleared async-writer/cancellation blockers for these exact source inputs');
  requireValue(safety.scope === 'async-writers-and-cancellation', 'Safety review scope mismatch');
  const reviewFiles = safety.evidence.map(evidenceFile);
  const logEvidence = evidenceFile(buildLog);
  const graphEvidence = evidenceFile(graphFile);
  requireValue(/Build completed successfully/.test(readFileSync(logEvidence.path, 'utf8')),
    'Export build log has no successful Bazel build result');
  const graph = readFileSync(graphEvidence.path, 'utf8');
  const platformTarget = profile === 'android_arm64' ? '//valdi:valdi_android' : '//valdi:valdi_ios';
  requireValue(graph.includes(targets[profile]) && graph.includes(platformTarget) &&
    graph.includes('//snap_drawing:snap_drawing_cc') && graph.includes('valdi_tgfx_deps'),
    'Configured graph does not show the actual platform Valdi/TGFX export closure');
  for (const line of graph.split('\n')) {
    const label = /(@@?[^/\s]+)\/\//.exec(line)?.[1]?.replace(/^@@?/, '');
    requireValue(!line.includes('//snap_drawing:skia_backend') &&
      !(label && label.split(/[+~]/).includes('skia')), `Skia target in runtime export graph: ${line}`);
  }
  return { safety: safetyEvidence, reviewFiles, buildLog: logEvidence, configuredGraph: graphEvidence };
}

export function validateBoundReceipt(receipt, sourceRoot, owned) {
  requireValue(receipt.schema === 2 && receipt.kind === 'tgfx-valdi-runtime-export' &&
    receipt.runtimeBinding === 'exported' && receipt.nativeAcceptance === false &&
    receipt.backend === 'tgfx' && receipt.engine === 'hermes' &&
    realpathSync(receipt.sourceRoot) === sourceRoot && targets[receipt.profile],
    'A reviewed native runtime export receipt is required; compile-only provenance is not a receipt');
  requireValue(sourceState(sourceRoot, receipt.profile) === receipt.sourceStateSha256, 'Native source state changed');
  requireValue(artifactDigest(receipt.runtime) === receipt.runtimeSha256, 'Runtime artifact changed');
  const inspection = inspectRuntime(receipt.runtime, receipt.profile, owned);
  requireValue(JSON.stringify(inspection) === JSON.stringify(receipt.inspection), 'Runtime platform inspection changed');
  const evidence = bindingEvidence({
    ...receipt, safetyFile: receipt.evidence.safety.path,
    buildLog: receipt.evidence.buildLog.path, graphFile: receipt.evidence.configuredGraph.path,
  });
  requireValue(JSON.stringify(evidence) === JSON.stringify(receipt.evidence), 'Runtime review/build evidence changed');
}
