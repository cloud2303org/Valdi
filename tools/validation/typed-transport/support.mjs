import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
export const fixtures = path.join(root, 'tools/validation/typed-transport/fixtures');
export const baseline = '15feb6530ceecb971f4c8519ea8ab77474b959db';

export function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: root, encoding: 'utf8', maxBuffer: 16 * 1024 * 1024, timeout: 60000, ...options,
  });
  if (result.error) throw result.error;
  return result;
}

export function success(command, args, options) {
  const result = run(command, args, options);
  assert.equal(result.status, 0,
    `${command} failed (${result.signal ?? result.status}):\n${result.stdout}\n${result.stderr}`);
  return result.stdout;
}

export function current(name) {
  return readFileSync(path.join(root, name), 'utf8');
}

export function pinned(name) {
  return success('git', ['-C', root, 'show', `${baseline}:${name}`]);
}

export function temporary(t) {
  const directory = mkdtempSync(path.join(tmpdir(), 'valdi-typed-transport-'));
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  return directory;
}

export function write(directory, name, text) {
  const filename = path.join(directory, name);
  mkdirSync(path.dirname(filename), { recursive: true });
  writeFileSync(filename, text);
}
