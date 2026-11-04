// Parent-run source/resource/codegen/Release-Hermes compilation only.
// No runtime export, native build, host edit, device launch or backend selection.
import { createHash } from 'node:crypto';
import {
  cpSync, existsSync, mkdirSync, readFileSync, readdirSync, realpathSync, writeFileSync,
} from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
function option(name: string): string {
  const at = args.indexOf(name);
  if (at < 0 || !args[at + 1] || args[at + 1].startsWith('--')) throw new Error(`${name} required`);
  return args.splice(at, 2)[1];
}
const tools = realpathSync(option('--tools'));
const sourceRoot = realpathSync(option('--source-root'));
const sourceRevision = option('--source-revision');
const output = path.resolve(option('--out'));
if (args.length) throw new Error(`Unknown arguments: ${args.join(' ')}`);
if (!/^[a-f0-9]{40}$/.test(sourceRevision)) throw new Error('--source-revision requires a full experimental source HEAD');
if (realpathSync(path.join(sourceRoot, 'snap_drawing/experimental/tgfx/full/performance')) !== realpathSync(here))
  throw new Error('--source-root must be W containing this benchmark, not the installed tools main checkout');
if (existsSync(output)) throw new Error(`Select a fresh --out directory: ${output}`);
if (output === path.join(here, 'module') || output.startsWith(path.join(here, 'module') + path.sep))
  throw new Error('--out cannot be inside module/');
const main = path.resolve(tools, '../..');
const nodeModules = path.join(main, 'valdi/node_modules');
if (!existsSync(nodeModules) || !existsSync(path.join(tools, 'node_modules')))
  throw new Error('--tools must point to installed main tools/valdi-rolldown; no dependency installation is performed');

function files(directory: string, skip: string[] = []): string[] {
  return readdirSync(directory, { withFileTypes: true }).filter(entry => !skip.includes(entry.name))
    .sort((a, b) => a.name.localeCompare(b.name)).flatMap(entry => {
    const file = path.join(directory, entry.name);
    if (entry.isSymbolicLink()) throw new Error(`Unexpected input symlink: ${file}`);
    return entry.isDirectory() ? files(file, skip) : [file];
  });
}
const sha256 = (bytes: Uint8Array) => createHash('sha256').update(bytes).digest('hex');
const hashFiles = (root: string, inputs: string[]) => Object.fromEntries(
  inputs.map(file => [path.relative(root, file), sha256(readFileSync(file))]),
);
function writeJson(file: string, value: unknown): void {
  writeFileSync(file, JSON.stringify(value, null, 2) + '\n');
}

mkdirSync(output, { recursive: true });
const temporary = path.join(output, 'tmp');
mkdirSync(temporary);
process.env.TMPDIR = temporary;
process.env.TMP = temporary;
process.env.TEMP = temporary;
process.env.VALDI_SOURCE_ROOT = sourceRoot;
const tool = (name: string) => import(pathToFileURL(path.join(tools, name)).href);
const { compileNodeModule } = await tool('module-build.ts');
const { compileRuntimeModule } = await tool('runtime-module.ts');
const { validateRuntimeContract, hermesCompiler } = await tool('hermes.ts');
const { unpack } = await tool('archive.ts');
const hermesRelease = validateRuntimeContract();
const { bytecodeVersion } = hermesCompiler();

const fixtureFiles = {
  png: 'snap_drawing/experimental/tgfx/full/tests/media-data/pattern.png',
  svg: 'snap_drawing/experimental/tgfx/full/app/fixtures/image.svg',
  lottie: 'apps/integration_test/src/valdi/integration_test_app/res/animation.json',
  font: 'snap_drawing/experimental/tgfx/full/tests/media-data/Montserrat-Regular.ttf',
  fontLicense: 'snap_drawing/experimental/tgfx/full/tests/media-data/Montserrat-LICENSE.txt',
};
const fixtures = Object.fromEntries(Object.entries(fixtureFiles)
  .map(([name, relative]) => [name, readFileSync(path.join(sourceRoot, relative))]));
const drawingDirectory = path.join(sourceRoot, 'src/valdi_modules/src/valdi/drawing');
const drawingSources = files(path.join(drawingDirectory, 'src'))
  .filter(file => /\.(ts|tsx|js)$/.test(file) && !file.includes('.spec.'));
const moduleInputs = files(path.join(here, 'module'));
const toolInputs = files(tools, ['node_modules', '.git', '.local', '.gen', 'out']).filter(file => {
  const relative = path.relative(tools, file);
  return /\.(ts|json)$/.test(relative);
});
const provenance = {
  schema: 1, module: 'render_benchmark', iosModuleName: 'RenderBenchmark',
  compilation: 'release', runtimeBinding: 'unbound', nativeAcceptance: false,
  sourceRoot, sourceRevision, revisionOrigin: 'caller supplied; exact input hashes recorded',
  tools, hermesRelease, hermesBytecodeVersion: bytecodeVersion,
  compilerEntrySha256: sha256(readFileSync(fileURLToPath(import.meta.url))),
  moduleFiles: hashFiles(path.join(here, 'module'), moduleInputs),
  drawingFiles: hashFiles(drawingDirectory, drawingSources),
  toolFiles: hashFiles(tools, toolInputs),
  sourceContractFiles: hashFiles(sourceRoot, [
    'modules/_configs/base.tsconfig.json',
    'third-party/hermes_v1/hermes.BUILD',
    'valdi/src/valdi/runtime/JavaScript/JavaScriptUtils.cpp',
  ].map(relative => path.join(sourceRoot, relative))),
  fixtureFiles: Object.fromEntries(Object.entries(fixtureFiles)
    .map(([name, relative]) => [name, { path: relative, sha256: sha256(fixtures[name]), bytes: fixtures[name].length }])),
};
writeJson(path.join(output, 'compile-inputs.json'), provenance);
const moduleDirectory = path.join(output, 'module');
cpSync(path.join(here, 'module'), moduleDirectory, { recursive: true });
// SVG and Lottie are not supported by the current PNG/WebP catalog compiler.
// Keep their original bytes (and the PNG byte path) in a generated TS module.
writeFileSync(path.join(moduleDirectory, 'src/Fixtures.ts'),
  `export const FIXTURES = ${JSON.stringify({
    png: [...fixtures.png], svg: [...fixtures.svg], lottie: [...fixtures.lottie],
  })};\nexport const PROVENANCE = ${JSON.stringify(provenance)};\n`);
mkdirSync(path.join(moduleDirectory, 'res'), { recursive: true });
writeFileSync(path.join(moduleDirectory, 'res/pattern.png'), fixtures.png);
writeFileSync(path.join(moduleDirectory, 'res/Montserrat-Regular.ttf'), fixtures.font);
writeFileSync(path.join(moduleDirectory, 'res/Montserrat-LICENSE.txt'), fixtures.fontLicense);

const generated = path.join(output, 'generated');
await compileNodeModule({
  name: 'render_benchmark', dir: moduleDirectory, generatedDirs: [], nodeModules,
}, generated, 'RenderBenchmark', 'release',
[{ name: 'drawing', dir: drawingDirectory, generatedDirs: [] }], 'objc');

const packageModules = path.join(output, 'package/modules');
mkdirSync(packageModules, { recursive: true });
cpSync(path.join(generated, 'render_benchmark.valdimodule'), path.join(packageModules, 'render_benchmark.valdimodule'));
const drawingArchive = path.join(packageModules, 'drawing.valdimodule');
await compileRuntimeModule({
  name: 'drawing', moduleDirectory: drawingDirectory, sources: drawingSources,
  outputs: [{ relative: 'android/release/assets/drawing.valdimodule', path: drawingArchive, directory: false }],
  dependencies: ['valdi_core', 'valdi_tsx', 'coreutils', 'source_map', 'jasmine', 'drawing', 'client_sql']
    .map(module_name => ({ module_name, module_path: '', module_content: '' })),
  iosModuleName: 'SCCDrawing', iosClassPrefix: '__valdi_unset_ios_class_prefix__',
  androidClassPath: 'com.snap.modules.drawing', disableAnnotations: false,
  asyncStrictMode: false, compilationMode: 'js_bytecode',
});
cpSync(path.join(generated, 'resources'), path.join(output, 'resources'), { recursive: true });
const licenseDirectory = path.join(output, 'resources/licenses');
mkdirSync(licenseDirectory);
writeFileSync(path.join(licenseDirectory, 'Montserrat-LICENSE.txt'), fixtures.fontLicense);

const archives = {
  render_benchmark: path.join(packageModules, 'render_benchmark.valdimodule'),
  drawing: drawingArchive,
};
const archiveEntries: { [name: string]: { [entry: string]: string } } = {};
for (const [name, file] of Object.entries(archives)) {
  const entries = unpack(readFileSync(file)) as Map<string, Buffer>;
  const required = name === 'drawing'
    ? ['src/ManagedContextFactory.js', 'src/BitmapFactory.js']
    : ['src/Benchmark.js', 'src/Scenes.js', 'src/Results.js', 'src/Fixtures.js', 'res.js', 'res.assetcatalog', 'res/montserrat_regular.ttf'];
  for (const entry of required) if (!entries.has(entry)) throw new Error(`Missing ${name}/${entry}`);
  let bytecodeCount = 0;
  for (const [entry, bytes] of entries) {
    if (!entry.endsWith('.js')) continue;
    if (bytes.length < 16 || bytes.readUInt32LE(0) !== 0x0100c634
        || bytes.readBigUInt64LE(4) !== 0x1f1903c103bc1fc6n || bytes.readUInt32LE(12) !== bytecodeVersion)
      throw new Error(`Not pinned Valdi-wrapped Release Hermes bytecode: ${name}/${entry}`);
    bytecodeCount++;
  }
  if (!bytecodeCount) throw new Error(`No Hermes entries in ${name}`);
  archiveEntries[name] = Object.fromEntries([...entries].map(([entry, bytes]) => [entry, sha256(bytes)]));
}
const kotlinFile = path.join(generated, 'android/render_benchmark.kt');
const objcFile = path.join(generated, 'ios/RenderBenchmarkTypes/RenderBenchmarkTypes.h');
const kotlin = readFileSync(kotlinFile, 'utf8');
const objc = readFileSync(objcFile, 'utf8');
if (!kotlin.includes('package com.snap.modules.render_benchmark') || !kotlin.includes('class BenchmarkContext')
    || !kotlin.includes('nowMs') || !kotlin.includes('report')
    || !objc.includes('RenderBenchmarkBenchmarkContext') || !objc.includes('initWithReport:')
    || !objc.includes('nowMs:'))
  throw new Error('Generated BenchmarkContext contract differs; inspect emitted Kotlin/ObjC before wiring hosts');
// Detect source drift during compilation. This is input provenance, never backend proof.
const checkUnchanged = (root: string, hashes: { [file: string]: string }) => {
  for (const [relative, expected] of Object.entries(hashes))
    if (sha256(readFileSync(path.join(root, relative))) !== expected) throw new Error(`Input changed during compilation: ${relative}`);
};
checkUnchanged(path.join(here, 'module'), provenance.moduleFiles);
checkUnchanged(drawingDirectory, provenance.drawingFiles);
checkUnchanged(tools, provenance.toolFiles);
checkUnchanged(sourceRoot, provenance.sourceContractFiles);
for (const [name, relative] of Object.entries(fixtureFiles))
  if (sha256(readFileSync(path.join(sourceRoot, relative))) !== provenance.fixtureFiles[name].sha256)
    throw new Error(`Fixture changed during compilation: ${relative}`);
writeJson(path.join(output, 'compile-complete.json'), {
  ...provenance, compileCompleted: true, backend: null,
  backendAuthority: 'separate host/native evidence; identical package is shared by both backends and platforms',
  archiveHashes: Object.fromEntries(Object.entries(archives).map(([name, file]) => [name, sha256(readFileSync(file))])),
  archiveEntries,
  generatedFiles: hashFiles(generated, files(generated)),
  resourceFiles: hashFiles(path.join(output, 'resources'), files(path.join(output, 'resources'))),
});
console.log(`Compile complete: ${output}. Reuse exact package/modules and resources for both hosts/backends; runtime remains unbound.`);
