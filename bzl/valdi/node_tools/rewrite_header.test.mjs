import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';
import { rewriteHeader } from './rewrite_header.mjs';

test('rewrites quoted and angled module prefixes and preserves trailing text', () => {
  assert.equal(
    rewriteHeader('#import "valdi_core/ios/A.h" // comment\r\n#include <valdi/x/B.hpp>\n#include <stdint.h>',
      'Runtime'),
    '#import "Runtime/ios/A.h" // comment\r\n#include <Runtime/x/B.hpp>\n#include <stdint.h>',
  );
});

test('flattens renamed modules only; preserves system modules and local headers', () => {
  assert.equal(rewriteHeader(
    '#import <UIKit/nested/A.h>\n#import <Foundation/B.h>\n#include "Local.h"\n#include "old/dir/C.h"\n',
    'Runtime', ['UIKit', 'Foundation'], true),
  '#import <UIKit/nested/A.h>\n#import <Foundation/B.h>\n#include "Local.h"\n#include "Runtime/C.h"\n');
});

test('retains the original scanner behavior for indentation, comments and whitespace', () => {
  const source = ' #import "old/A.h"\n// #include "old/B.h"\n# include <old/C.h>\n#import   \n"old/D.h"\n';
  assert.equal(rewriteHeader(source, 'Runtime'),
    ' #import "old/A.h"\n// #include "old/B.h"\n# include <old/C.h>\n#import "Runtime/D.h"\n');
  assert.equal(rewriteHeader('', 'Runtime'), '');
  assert.equal(rewriteHeader('#include ""\n#include "old//A.h"\n#include "old/A.h/"', 'Runtime'),
    '#include ""\n#include "Runtime//A.h"\n#include "Runtime/A.h"');
});

test('rejects unsupported macro imports and malformed directives', () => {
  for (const source of ['#include HEADER', '#import "broken', '#include\t"old/A.h"', '#include']) {
    assert.throws(() => rewriteHeader(source, 'Runtime'));
  }
});

test('CLI preserves bytes and validates arguments before writing output', () => {
  const directory = mkdtempSync(join(tmpdir(), 'valdi-header-'));
  try {
    const input = join(directory, 'input header.h');
    const output = join(directory, 'output header.h');
    const script = fileURLToPath(new URL('./rewrite_header.mjs', import.meta.url));
    const original = Buffer.concat([Buffer.from('// '), Buffer.from([0xff, 0xe9]), Buffer.from('\r\n#import <old/deep/A.h>\n#import <UIKit/deep/B.h>\n#import <Foundation/C.h>')]);
    writeFileSync(input, original);
    const args = [script, '-i', input, '-o', output, '-m', 'Runtime', '-f', '-p', 'UIKit', '-p', 'Foundation'];
    const result = spawnSync(process.execPath, args, { encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr);
    const expected = Buffer.from(original.toString('latin1').replace('<old/deep/A.h>', '<Runtime/A.h>'), 'latin1');
    assert.deepEqual(readFileSync(output), expected);
    const link = join(directory, 'bazel-script.mjs');
    symlinkSync(script, link);
    rmSync(output);
    const linked = spawnSync(process.execPath, [link, ...args.slice(1)], { encoding: 'utf8' });
    assert.equal(linked.status, 0, linked.stderr);
    assert.deepEqual(readFileSync(output), expected);
    for (const extra of [['--unknown'], ['-m'], []]) {
      const invalid = spawnSync(process.execPath, [script, ...extra], { encoding: 'utf8' });
      assert.notEqual(invalid.status, 0);
    }
    writeFileSync(input, '#include MACRO');
    assert.notEqual(spawnSync(process.execPath, args).status, 0);
    assert.deepEqual(readFileSync(output), expected);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
