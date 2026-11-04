import { createHash } from 'node:crypto';
import { lstatSync, readFileSync, readdirSync, readlinkSync } from 'node:fs';
import path from 'node:path';

export const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');

// File = SHA256(bytes). Directory = SHA256 of sorted relative path, NUL,
// file SHA256 (or "link:" + link target), LF. Never follows directory symlinks.
export function artifactDigest(input) {
  if (lstatSync(input).isFile()) return sha256(readFileSync(input));
  const entries = [];
  function walk(directory, prefix = '') {
    for (const name of readdirSync(directory).sort()) {
      const file = path.join(directory, name);
      const relative = prefix + name;
      const stat = lstatSync(file);
      if (stat.isSymbolicLink()) entries.push([relative, 'link:' + readlinkSync(file)]);
      else if (stat.isDirectory()) walk(file, relative + '/');
      else if (stat.isFile()) entries.push([relative, sha256(readFileSync(file))]);
      else throw new Error(`Unsupported runtime entry: ${file}`);
    }
  }
  walk(input);
  return sha256(entries.sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0)
    .map(([name, digest]) => `${name}\0${digest}\n`).join(''));
}
