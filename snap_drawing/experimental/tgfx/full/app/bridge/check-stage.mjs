// Parent host-build input check. Reads files only; never invokes native code.
import { readFileSync, realpathSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { artifactDigest, sha256 } from './artifact.mjs';
import { inspectRuntime, validateBoundReceipt } from './runtime-contract.mjs';

const app = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const [input, ...extra] = process.argv.slice(2);
if (!input || extra.length) throw new Error('Usage: node check-stage.mjs APP_LOCAL_STAGE');
const stage = realpathSync(input);
if (!stage.startsWith(realpathSync(path.join(app, '.local')) + path.sep))
  throw new Error('Stage must be below this app/.local/');
const prepared = JSON.parse(readFileSync(path.join(stage, 'prepared.json'), 'utf8'));
if (prepared.schema !== 2 || prepared.kind !== 'tgfx-valdi-app-stage')
  throw new Error('A runtime-bound application stage is required, not compile-only diagnostics');
validateBoundReceipt(prepared.provenance, realpathSync(prepared.sourceRoot), app);
const copy = path.join(stage, 'runtime',
  prepared.profile === 'android_arm64' ? 'valdi_runtime.aar' : 'ValdiRuntime.xcframework');
inspectRuntime(copy, prepared.profile, app);
if (artifactDigest(copy) !== prepared.provenance.runtimeSha256 ||
    artifactDigest(path.join(stage, 'generated')) !== prepared.generatedSha256 ||
    artifactDigest(path.join(stage, 'package')) !== prepared.packageSha256)
  throw new Error('Runtime, generated bindings/resources, or module package changed after preparation');
for (const name of ['tgfx_acceptance', 'drawing']) {
  if (sha256(readFileSync(path.join(stage, 'package/modules', name + '.valdimodule'))) !== prepared.moduleHashes[name])
    throw new Error(`Wrong bound module archive: ${name}`);
}
console.log(`Verified reviewed ${prepared.profile} stage; runtime acceptance is still pending.`);
