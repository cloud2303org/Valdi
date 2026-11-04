// Pure file hashing for the parent's final export input receipt. No git process.
import { existsSync, lstatSync, readFileSync, readdirSync, readlinkSync, realpathSync } from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { sha256 } from './artifact.mjs';

export function sourceState(sourceRoot, profile) {
  // Conservative local source/build envelope, not a configured Bazel query.
  // Include whole production roots, even files unused by a selected profile.
  // In particular valdi_runtime/core/protobuf link libs/utils, compiler/compiler
  // supplies the runtime wire proto, and local third-party rules/patches matter.
  const roots = [
    'BUILD.bazel', 'MODULE.bazel', 'MODULE.bazel.lock',
    '.bazelrc', '.bazelversion', '.bazelignore', 'package.json',
    ...readdirSync(sourceRoot).filter(name => name.endsWith('_install.json')).sort(),
    'bin', 'bzl', 'bzlmod', 'compiler', 'libs', 'modules', 'npm_modules',
    'registry', 'scripts', 'snap_drawing', 'src', 'third-party', 'tools', 'tsn',
    'valdi', 'valdi_core', 'valdi_protobuf',
    '.tgfx-deps/receipt.json', `.tgfx-deps/${profile}/manifest.json`,
  ];
  // Version the source envelope so an old narrow snapshot cannot be reused.
  const entries = [['@source-state-policy', 'local-runtime-v2']];
  function visit(relative) {
    // Production targets select snap_drawing/src, not the isolated experimental
    // apps/suite shells. Their fixture bytes are bound separately by prepare.
    if (relative === 'snap_drawing/experimental') return;
    const file = path.join(sourceRoot, relative);
    if (relative === '.tgfx-deps/receipt.json') {
      const receipt = JSON.parse(readFileSync(file, 'utf8'));
      if (!receipt.profiles?.[profile]) throw new Error(`Missing prepared profile ${profile}`);
      // Installing a different architecture later does not alter this profile's
      // native inputs. Pins and this selected manifest identity remain bound.
      entries.push([relative, sha256(JSON.stringify({
        schema: receipt.schema, pins: receipt.pins, profile: receipt.profiles[profile],
      }))]);
      return;
    }
    const stat = lstatSync(file);
    if (stat.isSymbolicLink()) entries.push([relative, 'link:' + readlinkSync(file)]);
    else if (stat.isDirectory()) {
      for (const name of readdirSync(file).sort()) {
        if (name.startsWith('bazel-') ||
            ['.git', '.local', '.cache', '.gradle', '.build', '.DS_Store',
              'node_modules', '__pycache__'].includes(name)) continue;
        visit(relative + '/' + name);
      }
    } else if (stat.isFile()) entries.push([relative, sha256(readFileSync(file))]);
  }
  for (const relative of roots) {
    if (!existsSync(path.join(sourceRoot, relative))) throw new Error(`Missing source input ${relative}`);
    visit(relative);
  }
  return sha256(entries.sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0)
    .map(([name, digest]) => `${name}\0${digest}\n`).join(''));
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [root, profile, ...extra] = process.argv.slice(2);
  if (!root || !['ios_sim_arm64', 'ios_device_arm64', 'android_arm64'].includes(profile) || extra.length)
    throw new Error('Usage: node source-state.mjs EXPERIMENTAL_SOURCE_ROOT PROFILE');
  console.log(sourceState(realpathSync(root), profile));
}
