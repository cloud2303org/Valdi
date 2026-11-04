// Parent-only preparation, never invoked automatically by a host/build.
import { mkdirSync, realpathSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { artifactDigest } from './artifact.mjs';
import { sourceState } from './source-state.mjs';
import { bindingEvidence, inspectRuntime } from './runtime-contract.mjs';

const args = process.argv.slice(2);
function option(name) {
  const at = args.indexOf(name);
  if (at < 0 || !args[at + 1] || args[at + 1].startsWith('--')) throw new Error(`${name} required`);
  return args.splice(at, 2)[1];
}
const sourceRoot = realpathSync(option('--source-root'));
const sourceRevision = option('--source-revision');
const sourceStateSha256 = option('--source-state-sha256');
const profile = option('--profile');
const runtime = realpathSync(option('--runtime'));
const safetyFile = realpathSync(option('--safety-review'));
const buildLog = realpathSync(option('--build-log'));
const graphFile = realpathSync(option('--dependency-graph'));
const output = path.resolve(option('--out'));
if (args.length) throw new Error(`Unknown arguments: ${args.join(' ')}`);
if (!/^[a-f0-9]{40}$/.test(sourceRevision) || !/^[a-f0-9]{64}$/.test(sourceStateSha256))
  throw new Error('Supply parent-attested full source revision and final dirty-source state SHA256');
if (!['ios_sim_arm64', 'ios_device_arm64', 'android_arm64'].includes(profile)) throw new Error('Unsupported profile');
const owned = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
if (realpathSync(path.join(sourceRoot, 'snap_drawing/experimental/tgfx/full/app')) !== realpathSync(owned)
    || !output.startsWith(path.join(owned, '.local') + path.sep))
  throw new Error('Receipt must describe this experimental worktree and be written under app/.local/');
if (sourceState(sourceRoot, profile) !== sourceStateSha256)
  throw new Error('Experimental source inputs changed since the parent export input snapshot');
const inspection = inspectRuntime(runtime, profile, owned);
const evidence = bindingEvidence({ sourceRoot, sourceRevision, sourceStateSha256, profile, safetyFile, buildLog, graphFile });
const receipt = {
  schema: 2, kind: 'tgfx-valdi-runtime-export', runtimeBinding: 'exported', nativeAcceptance: false,
  backend: 'tgfx', engine: 'hermes', sourceRoot, sourceRevision, sourceStateSha256,
  profile, runtime, runtimeSha256: artifactDigest(runtime),
  inspection, evidence,
  attestation: 'Parent-supplied build provenance; artifact hash verified, build flags are not inferred from the binary',
};
mkdirSync(path.dirname(output), { recursive: true });
writeFileSync(output, JSON.stringify(receipt, null, 2) + '\n', { flag: 'wx' });
