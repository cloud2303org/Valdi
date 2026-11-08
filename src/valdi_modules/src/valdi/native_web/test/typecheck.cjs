const assert = require('node:assert/strict');
const path = require('node:path');
const ts = require(process.argv[2] || 'typescript');

const fixture = path.join(__dirname, 'usage.ts');
const program = ts.createProgram([fixture], {
  strict: true,
  noEmit: true,
  target: ts.ScriptTarget.ES2020,
  module: ts.ModuleKind.CommonJS,
  lib: ['lib.es2020.d.ts'],
  types: [],
  skipLibCheck: false,
});
const diagnostics = ts.getPreEmitDiagnostics(program);
assert.equal(
  diagnostics.length,
  0,
  ts.formatDiagnosticsWithColorAndContext(diagnostics, {
    getCurrentDirectory: () => process.cwd(),
    getCanonicalFileName: file => file,
    getNewLine: () => '\n',
  }),
);
process.stdout.write('Native Web API type contract passed\n');
