// Keep the exported-header contract of the former C++ RewriteHeader tool.
// This is a textual transform, not a C preprocessor: only directives at the
// current line start are recognized, and preserved modules are never flattened.
import { readFileSync, realpathSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

export function rewriteHeader(source, moduleName, preservedModuleNames = [], flatten = false) {
  const preserved = new Set(preservedModuleNames);
  let output = '';
  let position = 0;
  while (position < source.length) {
    const directive = ['#include', '#import'].find(value => source.startsWith(value, position));
    if (!directive) {
      const newline = source.indexOf('\n', position);
      const end = newline < 0 ? source.length : newline + 1;
      output += source.slice(position, end);
      position = end;
      continue;
    }
    position += directive.length;
    // TextParser::tryParseWhitespaces accepts precisely space and LF.
    while (source[position] === ' ' || source[position] === '\n') position++;
    const opening = source[position++];
    if (opening !== '"' && opening !== '<') {
      throw new Error(`Expected a quoted or angle-bracket header after ${directive} at offset ${position - 1}`);
    }
    const closing = opening === '"' ? '"' : '>';
    const end = source.indexOf(closing, position);
    if (end < 0) throw new Error(`Unterminated header after ${directive} at offset ${position - 1}`);
    const importPath = source.slice(position, end);
    let parts = importPath.split('/');
    // StringBox::split keeps internal empty segments but omits the last empty one.
    if (parts.at(-1) === '') parts.pop();
    if (parts.length > 1 && !preserved.has(parts[0])) {
      parts[0] = moduleName;
      if (flatten && parts.length > 2) parts = [parts[0], parts.at(-1)];
    }
    output += `${directive} ${opening}${parts.join('/')}${closing}`;
    position = end + 1;
  }
  return output;
}

export function main(args) {
  const { values } = parseArgs({
    args,
    options: {
      input: { type: 'string', short: 'i' },
      output: { type: 'string', short: 'o' },
      module: { type: 'string', short: 'm' },
      preserve: { type: 'string', short: 'p', multiple: true },
      flatten: { type: 'boolean', short: 'f' },
    },
  });
  for (const key of ['input', 'output', 'module']) {
    if (!values[key]) throw new Error(`Missing --${key}`);
  }
  // Latin-1 provides a lossless byte mapping, including non-UTF-8 comments.
  const source = readFileSync(values.input).toString('latin1');
  const moduleName = Buffer.from(values.module).toString('latin1');
  const preserved = (values.preserve ?? []).map(value => Buffer.from(value).toString('latin1'));
  const result = rewriteHeader(source, moduleName, preserved, values.flatten);
  writeFileSync(values.output, Buffer.from(result, 'latin1'));
}

if (process.argv[1] && realpathSync(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    main(process.argv.slice(2));
  } catch (error) {
    console.error(`rewrite_header: ${error.message}`);
    process.exitCode = 1;
  }
}
