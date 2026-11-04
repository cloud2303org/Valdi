// Run with the app main checkout's installed tsx, ONLY after parent source review.
// Imports the actual APIs. No copied compiler, generated ABI, Bazel, or native build.
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, realpathSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { artifactDigest, sha256 } from './artifact.mjs';
import { sourceState } from './source-state.mjs';
import { validateBoundReceipt } from './runtime-contract.mjs';

const app = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
function option(name: string): string {
  const at = args.indexOf(name);
  if (at < 0 || !args[at + 1] || args[at + 1].startsWith('--')) throw new Error(`${name} required`);
  return args.splice(at, 2)[1];
}
const tools = realpathSync(option('--tools'));
const sourceRoot = realpathSync(option('--source-root'));
const receiptFile = realpathSync(option('--runtime-receipt'));
const receipt = JSON.parse(readFileSync(receiptFile, 'utf8'));
if (args.length) throw new Error(`Unknown arguments: ${args.join(' ')}`);
if (path.join(sourceRoot, 'snap_drawing/experimental/tgfx/full/app') !== realpathSync(app))
  throw new Error('This bridge must belong to the explicitly supplied experimental source worktree');
validateBoundReceipt(receipt, sourceRoot, app);
const profiles = ['ios_sim_arm64', 'ios_device_arm64', 'android_arm64'];
if (!profiles.includes(receipt.profile)) throw new Error('Unsupported runtime profile');
if (sourceState(sourceRoot, receipt.profile) !== receipt.sourceStateSha256)
  throw new Error('Runtime receipt no longer matches the current experimental source inputs');
const main = path.resolve(tools, '../..');
const runtime = realpathSync(receipt.runtime);
if (runtime.startsWith(main + path.sep)) throw new Error('Runtime input must be outside the app main checkout');
if (artifactDigest(runtime) !== receipt.runtimeSha256) throw new Error('Exported runtime changed after attestation');
if (!existsSync(path.join(main, 'valdi/node_modules'))) throw new Error('App main installed dependencies missing');

// A fresh stage is mandatory: do not merge outputs from two runtime receipts.
const stage = path.join(app, '.local', receipt.profile);
if (existsSync(stage)) throw new Error(`Stage already exists; parent must select/remove it explicitly: ${stage}`);
mkdirSync(stage, { recursive: true });
// The real compiler uses os.tmpdir() internally. Keep even its temporary copies
// inside this owned stage. Native tools are not invoked by this assignment.
const temporary = path.join(stage, 'tmp');
mkdirSync(temporary);
process.env.TMPDIR = temporary;
process.env.TMP = temporary;
process.env.TEMP = temporary;
process.env.VALDI_SOURCE_ROOT = sourceRoot;
const importTool = (name: string) => import(pathToFileURL(path.join(tools, name)).href);
const { compileNodeModule } = await importTool('module-build.ts');
const { compileRuntimeModule } = await importTool('runtime-module.ts');
const { unpack } = await importTool('archive.ts');
const { validateRuntimeContract } = await importTool('hermes.ts');
const { appModule } = await importTool('pipeline.ts');
const moduleName = 'tgfx_acceptance';
if (existsSync(appModule(moduleName).dir)) throw new Error('Experimental module name collides with app main');
const moduleDirectory = path.join(stage, 'module');
cpSync(path.join(app, 'module'), moduleDirectory, { recursive: true });
const fixturePaths = {
  png: 'snap_drawing/experimental/tgfx/full/tests/media-data/pattern.png',
  svg: 'snap_drawing/experimental/tgfx/full/app/fixtures/image.svg',
  lottie: 'apps/integration_test/src/valdi/integration_test_app/res/animation.json',
};
const fixtures = Object.fromEntries(Object.entries(fixturePaths)
  .map(([name, relative]) => [name, readFileSync(path.join(sourceRoot, relative))]));
const toolsHashes = Object.fromEntries(['module-build.ts', 'runtime-module.ts', 'pipeline.ts', 'valdiTs.ts', 'hermes.ts', 'resources.ts']
  .map(name => [name, sha256(readFileSync(path.join(tools, name)))]));
const provenance = {
  ...receipt, tools, toolsHashes, hermesRelease: validateRuntimeContract(),
  fixtureFiles: Object.fromEntries(Object.entries(fixturePaths)
    .map(([name, relative]) => [name, { path: relative, sha256: sha256(fixtures[name]) }])),
  gate: 'actual Valdi/Hermes application with managed SnapDrawing TGFX output; not an iOS screen compositor gate',
};
writeFileSync(path.join(moduleDirectory, 'src/Fixtures.ts'),
  `// Original fixture bytes; generated in the isolated stage, never rasterized at build time.\n`
  + `export const FIXTURES = ${JSON.stringify(Object.fromEntries(Object.entries(fixtures).map(([name, bytes]) => [name, [...bytes]])))};\n`
  + `export const PROVENANCE = ${JSON.stringify(provenance)};\n`);
mkdirSync(path.join(moduleDirectory, 'res'), { recursive: true });
writeFileSync(path.join(moduleDirectory, 'res/pattern.png'), fixtures.png);

// Explicit drawing dependency fixes writeTsconfig's intentionally small runtime
// allowlist. buildModule leaves drawing/* external; it needs its own archive.
const drawingDirectory = path.join(sourceRoot, 'src/valdi_modules/src/valdi/drawing');
const drawing = { name: 'drawing', dir: drawingDirectory, generatedDirs: [] };
const generated = path.join(stage, 'generated');
await compileNodeModule({
  name: moduleName, dir: moduleDirectory, generatedDirs: [],
  nodeModules: path.join(main, 'valdi/node_modules'),
}, generated, 'TGFXAcceptance', 'release', [drawing], 'objc');

const modules = path.join(stage, 'package/modules');
mkdirSync(modules, { recursive: true });
const drawingOutputs = ['android/release/assets/drawing.valdimodule', 'android/release/assets/drawing.map.json'];
function files(directory: string): string[] {
  return readdirSync(directory, { withFileTypes: true }).flatMap(entry => {
    const file = path.join(directory, entry.name);
    return entry.isDirectory() ? files(file) : [file];
  });
}
// Real known-runtime adapter; only request JS assets, not another native runtime
// or generated native drawing implementations.
await compileRuntimeModule({
  name: 'drawing', moduleDirectory: drawingDirectory,
  sources: files(path.join(drawingDirectory, 'src')).filter(file => /\.(ts|tsx|js)$/.test(file) && !file.includes('.spec.')),
  outputs: drawingOutputs.map(relative => ({ relative, path: path.join(stage, 'drawing', relative), directory: false })),
  dependencies: ['valdi_core', 'valdi_tsx', 'coreutils', 'source_map', 'jasmine', 'drawing', 'client_sql']
    .map(module_name => ({ module_name, module_path: '', module_content: '' })),
  iosModuleName: 'SCCDrawing', iosClassPrefix: '__valdi_unset_ios_class_prefix__',
  androidClassPath: 'com.snap.modules.drawing', disableAnnotations: false,
  asyncStrictMode: false, compilationMode: 'js_bytecode',
});
for (const name of [moduleName + '.valdimodule', moduleName + '.map.json'])
  cpSync(path.join(generated, name), path.join(modules, name));
for (const relative of drawingOutputs) cpSync(path.join(stage, 'drawing', relative), path.join(modules, path.basename(relative)));
const requiredEntries = {
  tgfx_acceptance: ['src/Acceptance.js', 'src/Fixtures.js', 'res.js', 'res.assetcatalog'],
  drawing: ['src/ManagedContextFactory.js', 'src/BitmapFactory.js'],
};
for (const [name, required] of Object.entries(requiredEntries)) {
  const entries = unpack(readFileSync(path.join(modules, name + '.valdimodule')));
  for (const entry of required) if (!entries.has(entry)) throw new Error(`Missing ${name}/${entry}`);
  for (const [entry, bytes] of entries) if (entry.endsWith('.js') && bytes.readUInt32LE(0) !== 0x0100c634)
    throw new Error(`${name}/${entry} is not Valdi-wrapped Hermes bytecode`);
}
// Verify generated names against the real codegen before hosts try to compile.
const kotlin = readFileSync(path.join(generated, 'android/tgfx_acceptance.kt'), 'utf8');
const objc = readFileSync(path.join(generated, 'ios/TGFXAcceptanceTypes/TGFXAcceptanceTypes.h'), 'utf8');
if (!kotlin.includes('package com.snap.modules.tgfx_acceptance') || !kotlin.includes('class AcceptanceContext')
  || !objc.includes('TGFXAcceptanceAcceptanceContext') || !objc.includes('initWithReport:'))
  throw new Error('Generated context contract changed; inspect generated sources and update app-local hosts only');
validateBoundReceipt(receipt, sourceRoot, app);
mkdirSync(path.join(stage, 'runtime'), { recursive: true });
const copiedRuntime = path.join(stage, 'runtime',
  receipt.profile === 'android_arm64' ? 'valdi_runtime.aar' : 'ValdiRuntime.xcframework');
cpSync(runtime, copiedRuntime, { recursive: true, dereference: false, verbatimSymlinks: true });
if (artifactDigest(copiedRuntime) !== receipt.runtimeSha256) throw new Error('Copied runtime hash mismatch');
writeFileSync(path.join(stage, 'package/provenance.json'), JSON.stringify(provenance, null, 2) + '\n');
writeFileSync(path.join(stage, 'prepared.json'), JSON.stringify({
  schema: 2, kind: 'tgfx-valdi-app-stage', profile: receipt.profile, sourceRoot, moduleName, provenance,
  generatedSha256: artifactDigest(generated),
  packageSha256: artifactDigest(path.join(stage, 'package')),
  moduleHashes: Object.fromEntries(['tgfx_acceptance', 'drawing'].map(name =>
    [name, sha256(readFileSync(path.join(modules, name + '.valdimodule')))])),
}, null, 2) + '\n');
console.log(`Prepared ${stage}. Native compilation and runtime acceptance have NOT run.`);
