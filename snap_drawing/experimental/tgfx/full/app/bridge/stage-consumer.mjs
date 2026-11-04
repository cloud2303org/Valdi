// Source staging only. Does NOT invoke Bazel, Node compiler, export scripts,
// downloads, or a native executable. Parent runs this after review.
import { cpSync, existsSync, mkdirSync, readFileSync, realpathSync, symlinkSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { sha256 } from './artifact.mjs';

const app = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
function option(name) {
  const at = args.indexOf(name);
  if (at < 0 || !args[at + 1] || args[at + 1].startsWith('--')) throw new Error(`${name} required`);
  return args.splice(at, 2)[1];
}
const main = realpathSync(option('--app-main'));
const source = realpathSync(option('--source-root'));
if (args.length) throw new Error(`Unknown arguments: ${args.join(' ')}`);
if (realpathSync(path.join(source, 'snap_drawing/experimental/tgfx/full/app')) !== realpathSync(app))
  throw new Error('Source must be the experimental worktree containing this bridge');
const root = path.join(app, '.local/export-consumer');
if (existsSync(root)) throw new Error(`Parent must remove/select existing staging directory explicitly: ${root}`);
const workspace = path.join(root, 'valdi');
mkdirSync(path.join(workspace, 'bzl'), { recursive: true });
// The real repository adapter resolves ../tools relative to its root MODULE.
// Symlinks live only beneath app/.local, and point to installed tools read-only.
symlinkSync(path.join(main, 'tools'), path.join(root, 'tools'), 'dir');
cpSync(path.join(main, 'valdi/bzl/node_compiler'), path.join(workspace, 'bzl/node_compiler'), { recursive: true });
// The runtime CLI compares argv with import.meta.url; tsx resolves the tools
// symlink, so Bazel must invoke that same canonical entry point.
const repositoryFile = path.join(workspace, 'bzl/node_compiler/repositories.bzl');
const repositorySource = readFileSync(repositoryFile, 'utf8');
const toolsPath = '    tools = root.get_child("tools/valdi-rolldown")\n';
if (repositorySource.split(toolsPath).length !== 2)
  throw new Error('Node tool repository shape changed; inspect tools path resolution');
writeFileSync(repositoryFile, repositorySource.replace(toolsPath,
  '    tools = root.get_child("tools/valdi-rolldown").realpath\n'));
// Root MODULE has a real root-local rules_python patch label. Preserve its
// checked-in source too; don't rewrite it to the fork or omit the patch.
cpSync(path.join(main, 'valdi/bzl/patches'), path.join(workspace, 'bzl/patches'), { recursive: true });
cpSync(path.join(main, 'valdi/.bazelrc'), path.join(workspace, '.bazelrc'));
if (existsSync(path.join(source, '.bazelversion')))
  cpSync(path.join(source, '.bazelversion'), path.join(workspace, '.bazelversion'));
cpSync(path.join(app, 'consumer/BUILD.bazel'), path.join(workspace, 'BUILD.bazel'));
const original = readFileSync(path.join(main, 'valdi/MODULE.bazel'), 'utf8');
let module = original;
const overrides = {
  valdi: '', android_macros: 'bzl/macros', snap_macros: 'bzl/valdi/snap_macros',
  snap_client_toolchains: 'bzl/toolchains', snap_platforms: 'bzl/platforms',
  skia_user_config: 'third-party/skia_user_config', rules_hdrs: 'third-party/rules_hdrs',
  valdi_toolchain: 'bin',
};
for (const [name, relative] of Object.entries(overrides)) {
  // Bounded to the exact simple archive_override blocks used by app main.
  // Stop if their shape changes instead of silently retaining an old pin.
  const expression = new RegExp(`archive_override\\(\\s*module_name\\s*=\\s*"${name}"\\s*,[^)]*\\)`, 'g');
  const matches = [...module.matchAll(expression)];
  if (matches.length !== 1) throw new Error(`Expected one ${name} archive override; parent must inspect new MODULE shape`);
  module = module.replace(expression,
    `local_path_override(module_name = "${name}", path = ${JSON.stringify(path.join(source, relative))})`);
}
// The custom restricted XCFramework declarations load the Apple macro from the
// consumer BUILD itself. App main uses it only through @valdi's repo mapping.
if (/bazel_dep\(\s*name\s*=\s*"rules_apple"/.test(module))
  throw new Error('App main now declares rules_apple directly; parent must inspect its repo_name mapping');
module += '\nbazel_dep(name = "rules_apple", version = "4.0.0", repo_name = "build_bazel_rules_apple")\n';
writeFileSync(path.join(workspace, 'MODULE.bazel'), module);
writeFileSync(path.join(root, 'source-inputs.json'), JSON.stringify({
  appMain: main, experimentalSource: source, rootModuleSha256: sha256(original),
  runtimeAdapterSha256: sha256(readFileSync(path.join(main, 'valdi/bzl/node_compiler/adapter.bzl'))),
  stagedOverrides: overrides,
  scope: 'Only this staged consumer changes module resolution; main MODULE pins and runtime artifacts are untouched',
}, null, 2) + '\n');
console.log(workspace);
