// Parent-run Node/TS/codegen/Hermes diagnostic entry point. No runtime export,
// TGFX archive access, host build, application launch, or native graphics calls.
// This is intentionally independent of prepare.mts's native runtime receipt.
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, realpathSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { sha256 } from './artifact.mjs';

const app = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
function option(name: string): string {
  const at = args.indexOf(name);
  if (at < 0 || !args[at + 1] || args[at + 1].startsWith('--')) throw new Error(`${name} required`);
  return args.splice(at, 2)[1];
}
const tools = realpathSync(option('--tools'));
const sourceRoot = realpathSync(option('--source-root'));
const sourceRevision = option('--source-revision');
const runtimeBinding = option('--runtime-binding');
const output = path.resolve(option('--out'));
if (args.length) throw new Error(`Unknown arguments: ${args.join(' ')}`);
if (runtimeBinding !== 'unbound') throw new Error('Compile-only requires --runtime-binding unbound; use prepare.mts for a runtime-bound stage');
if (!/^[a-f0-9]{40}$/.test(sourceRevision)) throw new Error('Full experimental source HEAD required');
if (realpathSync(path.join(sourceRoot, 'snap_drawing/experimental/tgfx/full/app')) !== realpathSync(app))
  throw new Error('Explicit source root must be the experimental worktree containing this entry point');
if (!output.startsWith(path.join(app, '.local') + path.sep))
  throw new Error('Diagnostic output must remain under this app/.local/');
if (existsSync(output)) throw new Error(`Select a fresh diagnostic output directory: ${output}`);
const main = path.resolve(tools, '../..');
if (!existsSync(path.join(main, 'valdi/node_modules'))) throw new Error('Installed app main dependencies required');
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
const { validateRuntimeContract } = await tool('hermes.ts');
const { unpack } = await tool('archive.ts');
const fixtureFiles = {
  png: 'snap_drawing/experimental/tgfx/full/tests/media-data/pattern.png',
  svg: 'snap_drawing/experimental/tgfx/full/app/fixtures/image.svg',
  lottie: 'apps/integration_test/src/valdi/integration_test_app/res/animation.json',
};
const fixtures = Object.fromEntries(Object.entries(fixtureFiles)
  .map(([name, relative]) => [name, readFileSync(path.join(sourceRoot, relative))]));
const provenance = {
  schema: 1, gate: 'Node compile diagnostics ONLY', runtimeBinding: 'unbound',
  runtimeArtifact: null, nativeAcceptance: false, backendIntent: 'tgfx', engineIntent: 'hermes',
  sourceRoot, sourceRevision, hermesRelease: validateRuntimeContract(), tools,
  acceptanceSourceSha256: sha256(readFileSync(path.join(app, 'module/src/Acceptance.tsx'))),
  compilerEntrySha256: sha256(readFileSync(fileURLToPath(import.meta.url))),
  toolFiles: Object.fromEntries(['module-build.ts', 'runtime-module.ts', 'pipeline.ts', 'valdiTs.ts', 'hermes.ts', 'resources.ts']
    .map(name => [name, sha256(readFileSync(path.join(tools, name)))])),
  fixtureFiles: Object.fromEntries(Object.entries(fixtureFiles)
    .map(([name, relative]) => [name, { path: relative, sha256: sha256(fixtures[name]) }])),
};
writeFileSync(path.join(output, 'compile-inputs.json'), JSON.stringify(provenance, null, 2) + '\n');
const moduleDirectory = path.join(output, 'module');
cpSync(path.join(app, 'module'), moduleDirectory, { recursive: true });
writeFileSync(path.join(moduleDirectory, 'src/Fixtures.ts'),
  `export const FIXTURES = ${JSON.stringify(Object.fromEntries(Object.entries(fixtures).map(([name, bytes]) => [name, [...bytes]])))};\n`
  + `export const PROVENANCE = ${JSON.stringify(provenance)};\n`);
mkdirSync(path.join(moduleDirectory, 'res'), { recursive: true });
writeFileSync(path.join(moduleDirectory, 'res/pattern.png'), fixtures.png);
const drawingDirectory = path.join(sourceRoot, 'src/valdi_modules/src/valdi/drawing');
const generated = path.join(output, 'generated');
console.log('Compiling actual acceptance TSX, resources, generated bindings, and Release Hermes bytecode');
await compileNodeModule({
  name: 'tgfx_acceptance', dir: moduleDirectory, generatedDirs: [],
  nodeModules: path.join(main, 'valdi/node_modules'),
}, generated, 'TGFXAcceptance', 'release',
[{ name: 'drawing', dir: drawingDirectory, generatedDirs: [] }], 'objc');

function files(directory: string): string[] {
  return readdirSync(directory, { withFileTypes: true }).flatMap(entry => {
    const file = path.join(directory, entry.name);
    return entry.isDirectory() ? files(file) : [file];
  });
}
const relative = 'android/release/assets/drawing.valdimodule';
console.log('Compiling actual known-runtime drawing JS wrappers (no native compilation)');
await compileRuntimeModule({
  name: 'drawing', moduleDirectory: drawingDirectory,
  sources: files(path.join(drawingDirectory, 'src')).filter(file => /\.(ts|tsx|js)$/.test(file) && !file.includes('.spec.')),
  outputs: [{ relative, path: path.join(output, 'drawing', relative), directory: false }],
  dependencies: ['valdi_core', 'valdi_tsx', 'coreutils', 'source_map', 'jasmine', 'drawing', 'client_sql']
    .map(module_name => ({ module_name, module_path: '', module_content: '' })),
  iosModuleName: 'SCCDrawing', iosClassPrefix: '__valdi_unset_ios_class_prefix__',
  androidClassPath: 'com.snap.modules.drawing', disableAnnotations: false,
  asyncStrictMode: false, compilationMode: 'js_bytecode',
});
const archives = {
  tgfx_acceptance: path.join(generated, 'tgfx_acceptance.valdimodule'),
  drawing: path.join(output, 'drawing', relative),
};
for (const [name, file] of Object.entries(archives)) {
  const entries = unpack(readFileSync(file));
  const required = name === 'drawing'
    ? ['src/ManagedContextFactory.js', 'src/BitmapFactory.js']
    : ['src/Acceptance.js', 'src/Fixtures.js', 'res.js', 'res.assetcatalog'];
  for (const entry of required) if (!entries.has(entry)) throw new Error(`Missing ${name}/${entry}`);
  for (const [entry, bytes] of entries) {
    if (entry.endsWith('.js') && (bytes.length < 4 || bytes.readUInt32LE(0) !== 0x0100c634))
      throw new Error(`Not Valdi-wrapped Hermes bytecode: ${name}/${entry}`);
  }
}
const kotlin = readFileSync(path.join(generated, 'android/tgfx_acceptance.kt'), 'utf8');
const objc = readFileSync(path.join(generated, 'ios/TGFXAcceptanceTypes/TGFXAcceptanceTypes.h'), 'utf8');
if (!kotlin.includes('package com.snap.modules.tgfx_acceptance') || !kotlin.includes('class AcceptanceContext') ||
    !objc.includes('TGFXAcceptanceAcceptanceContext') || !objc.includes('initWithReport:'))
  throw new Error('Generated native context contract differs from host source; inspect original emitted files');
writeFileSync(path.join(output, 'compile-complete.json'), JSON.stringify({
  ...provenance, compileCompleted: true,
  archiveHashes: Object.fromEntries(Object.entries(archives).map(([name, file]) => [name, sha256(readFileSync(file))])),
}, null, 2) + '\n');
console.log(`Node compilation completed: ${output}. Runtime UNBOUND; no host/native acceptance was run.`);
