// JS glue only: native algorithms and Hermes behavior are tested by the host suite.
// Usage: node test/installer.cjs /absolute/path/to/typescript
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function verifyInstaller(sources, tslibSource, valueNames) {
  function load(failDefinition) {
    const context = vm.createContext({});
    const globals = vm.runInContext('globalThis', context);
    if (failDefinition) {
      globals.globalThis = new Proxy(globals, {
        defineProperty(target, name, descriptor) {
          if (failDefinition(name)) throw new Error('recoverable definition failure');
          return Reflect.defineProperty(target, name, descriptor);
        },
      });
    }
    const raw = {};
    const originals = {};
    const requests = [];
    let nativeInstalls = 0;
    let networkCalls = 0;
    for (const name of valueNames) {
      const value = function () {
        networkCalls++;
        throw new Error('Installation must not invoke native Web APIs');
      };
      originals[name] = value;
      Object.defineProperty(raw, name, {
        value, writable: true, configurable: true, enumerable: false,
      });
    }
    // The baseline has a native installer. It deliberately does nothing here:
    // only evaluated JS can satisfy the global-installation contract.
    if (!sources.has('src/NativeWeb.js')) {
      Object.defineProperty(raw, 'installNativeWeb', {
        value() { nativeInstalls++; }, enumerable: false,
      });
    }
    const modules = new Map();
    function evaluate(id) {
      if (modules.has(id)) return modules.get(id);
      if (id === 'src/NativeWebNative.js' ||
          (id === 'src/NativeWeb.js' && !sources.has(id))) return raw;
      const source = id === 'tslib' ? tslibSource : sources.get(id);
      assert.equal(typeof source, 'string', `Missing compiled module ${id}`);
      const exports = {};
      modules.set(id, exports);
      const requireModule = request => {
        requests.push({ from: id, request });
        if (request === 'tslib') return evaluate('tslib');
        assert.ok(request.startsWith('./'), `Unexpected require: ${request}`);
        return evaluate(path.posix.normalize(path.posix.join(
          path.posix.dirname(id), request + '.js',
        )));
      };
      vm.runInContext(`(function(require, module, exports) {${source}\n})`,
        context, { filename: id })(requireModule, { exports }, exports);
      return exports;
    }
    const publicModule = evaluate('src/NativeWeb.js');
    return {
      globals, raw, originals, publicModule, evaluate, requests,
      nativeInstalls: () => nativeInstalls, networkCalls: () => networkCalls,
    };
  }

  const basic = load();
  for (const name of valueNames) {
    assert.equal(Object.hasOwn(basic.globals, name), false, `Import changed ${name}`);
  }
  basic.publicModule.installNativeWeb();
  for (const name of valueNames) {
    assert.equal(basic.globals[name], basic.originals[name],
      `JS installer must install ${name} without delegating to the native stub`);
    assert.deepEqual(Object.getOwnPropertyDescriptor(basic.globals, name), {
      value: basic.originals[name], writable: true, enumerable: false, configurable: true,
    });
  }
  assert.equal(basic.nativeInstalls(), 0);
  assert.equal(basic.networkCalls(), 0);
  assert.equal(Object.hasOwn(basic.globals, 'installNativeWeb'), false);
  assert.equal(basic.publicModule.installNativeWeb.length, 0);
  const barrel = basic.evaluate('src/index.js');
  assert.deepEqual(Object.keys(barrel).sort(), [...valueNames, 'installNativeWeb'].sort());
  for (const name of [...valueNames, 'installNativeWeb']) {
    assert.equal(barrel[name], basic.publicModule[name], `Barrel export ${name}`);
  }
  assert.deepEqual(Object.keys(basic.raw), [], 'Raw fixture must be non-enumerable');
  for (const name of valueNames) {
    assert.equal(basic.publicModule[name], basic.originals[name], `Public export ${name}`);
  }
  assert.equal(basic.requests.some(item => item.request.includes('NativeWebNative')), true);

  // Capture at wrapper evaluation, before mutation of raw or public exports.
  const captured = load();
  for (const name of valueNames) {
    captured.raw[name] = { replaced: name };
    Object.defineProperty(captured.publicModule, name, {
      value: { publicReplacement: name }, configurable: true,
    });
    Object.defineProperty(captured.globals, name, {
      value: { globalReplacement: name }, configurable: true, writable: false,
    });
  }
  captured.raw.lateAddedProperty = 'must not be installed';
  captured.publicModule.installNativeWeb();
  for (const name of valueNames) assert.equal(captured.globals[name], captured.originals[name]);
  assert.equal(Object.hasOwn(captured.globals, 'lateAddedProperty'), false);
  assert.equal(captured.networkCalls(), 0);

  // Repeated success must not inspect or restore globals changed by the app.
  const replacement = {};
  for (const name of valueNames) {
    Object.defineProperty(captured.globals, name, {
      value: replacement, writable: false, configurable: false,
    });
  }
  captured.publicModule.installNativeWeb();
  for (const name of valueNames) assert.equal(captured.globals[name], replacement);

  // Failure must not mark installation complete; retry uses original captures.
  const conflict = valueNames[valueNames.length - 1];
  let rejectDefinition = true;
  const retry = load(name => name === conflict && rejectDefinition);
  Object.defineProperty(retry.globals, conflict, { value: 'old', configurable: true });
  // A Proxy gives a recoverable defineProperty failure, unlike an irreversible
  // nonconfigurable global; it lets the same wrapper instance retry.
  assert.throws(() => retry.publicModule.installNativeWeb(), /recoverable definition failure/);
  rejectDefinition = false;
  retry.publicModule.installNativeWeb();
  for (const name of valueNames) assert.equal(retry.globals[name], retry.originals[name]);

  const blocked = load();
  Object.defineProperty(blocked.globals, conflict, {
    value: 'blocked', configurable: false, writable: true,
  });
  assert.throws(() => blocked.publicModule.installNativeWeb(),
    error => error.name === 'TypeError');
  assert.throws(() => blocked.publicModule.installNativeWeb(),
    error => error.name === 'TypeError');
  assert.equal(blocked.globals[conflict], 'blocked');
  assert.equal(load().globals.fetch, undefined, 'Installation is per JS context');
  process.stdout.write(`JS installer glue passed (${valueNames.length} native exports)\n`);
  return { valueNames, requires: basic.requests, nativeInstallCalls: basic.nativeInstalls() };
}

module.exports = { verifyInstaller };

if (require.main === module) {
  const ts = require(process.argv[2] || 'typescript');
  const moduleRoot = path.resolve(__dirname, '..');
  const files = ts.sys.readDirectory(path.join(moduleRoot, 'src'), ['.ts']);
  const outputs = new Map();
  const program = ts.createProgram(files, {
    strict: true, target: ts.ScriptTarget.ES2019, module: ts.ModuleKind.CommonJS,
    lib: ['lib.es2019.d.ts'], types: [], importHelpers: false,
  });
  const diagnostics = ts.getPreEmitDiagnostics(program);
  assert.equal(diagnostics.length, 0, ts.formatDiagnosticsWithColorAndContext(diagnostics, {
    getCurrentDirectory: () => process.cwd(), getCanonicalFileName: file => file, getNewLine: () => '\n',
  }));
  program.emit(undefined, (file, contents) => outputs.set(
    path.relative(moduleRoot, file).split(path.sep).join('/'), contents,
  ));
  const declaration = files.find(file => file.endsWith('NativeWebNative.d.ts')) ||
    files.find(file => file.endsWith('NativeWeb.d.ts'));
  const valueNames = [...fs.readFileSync(declaration, 'utf8').matchAll(
    /^export (?:class|function) (\w+)/gm,
  )].map(match => match[1]).filter(name => name !== 'installNativeWeb');
  // This standalone path emits inline helpers; the actual runtime-compiler
  // evidence uses the fork's own tslib and the same verifier.
  verifyInstaller(outputs, '', valueNames);
}
