import assert from 'node:assert/strict';
import { existsSync, readFileSync, realpathSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { root, fixtures, current, pinned, run, success, temporary } from './support.mjs';

const objc = 'valdi_core/src/valdi_core/ios/valdi_core/SCValdiObjCConversionUtils.mm';

function nativePrerequisites(t) {
  if (process.platform !== 'darwin' || process.arch !== 'arm64') {
    t.skip('The linked native fixtures require existing macOS arm64 host archives');
    return;
  }
  const names = ['VALDI_NATIVE_TEST_HOST', 'VALDI_NATIVE_TEST_EXECROOT',
    'VALDI_NATIVE_TEST_HERMES_SOURCE', 'VALDI_NATIVE_TEST_HERMESC'];
  const absent = names.filter(name => !process.env[name]);
  if (absent.length) {
    t.skip(`Explicit read-only host inputs required: ${absent.join(', ')}`);
    return;
  }
  const [host, execroot, hermes, hermesc] = names.map(name => realpathSync(process.env[name]));
  assert.ok(existsSync(path.join(host, 'compile_commands.json')));
  t.diagnostic(`Current source: ${root}; host: ${host}; execroot: ${execroot}`);
  t.diagnostic(`Hermes headers: ${hermes}; hermesc: ${hermesc}`);
  return { host, execroot, hermes, hermesc };
}

function nativeFlags({ host, execroot, hermes }) {
  const rows = JSON.parse(readFileSync(path.join(host, 'compile_commands.json'), 'utf8'));
  const suffix = '/valdi/src/valdi/runtime/Runtime.cpp';
  const row = rows.find(row => row.file.endsWith(suffix));
  assert.ok(row, 'Missing real host runtime compiler command');
  const cachedSource = row.file.slice(0, -suffix.length);
  const base = row.arguments ?? row.command.match(/(?:[^\s"]+|"[^"]*")+/g).map(arg => arg.replace(/^"|"$/g, ''));
  const output = base.indexOf('-o');
  assert.ok(output > 0, 'Expected compiler command with -o after compile flags');
  // Replace every cached framework include with this checkout. Generated headers
  // and third-party dependencies remain read-only inputs from the supplied build.
  const flags = base.slice(1, output).map(arg => arg.replaceAll(cachedSource, root));
  return {
    compiler: base[0],
    flags: [`-I${root}/valdi/src`, `-I${root}/valdi_core/src`, ...flags,
      ...[
        `${root}/third-party/hermes_v1/prepared_include/apple`,
        `${root}/third-party/hermes_v1/prepared_include`,
        `${execroot}/bazel-out/darwin_arm64-fastbuild/bin/external/valdi++_repo_rules+hermes`,
        ...['include', 'public', 'API', 'API/jsi', 'external', 'external/llvh/include',
          'external/llvh/gen/include'].map(name => path.join(hermes, name)),
      ].map(name => `-I${name}`),
      '-DNDEBUG', '-DHERMESVM_ALLOW_CONCURRENT_GC', '-DHERMESVM_ALLOW_INLINE_ASM',
      '-DHERMESVM_INDIRECT_THREADING', '-DHERMES_CHECK_NATIVE_STACK',
      '-DHERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES', '-DHERMES_USE_BOOST_CONTEXT=0',
      '-DSTATIC_HERMES', '-DHERMES_PLATFORM_UNICODE=HERMES_PLATFORM_UNICODE_LITE',
    ],
  };
}
function linkFlags(execroot, mode, fixture) {
  const params = path.join(execroot, 'bazel-out/darwin_arm64-fastbuild/bin/external/valdi+/valdi',
    mode === 'full' ? 'test_hermes_boundary-0.params' : 'test_hermes_jsi_lean-0.params');
  const original = readFileSync(params, 'utf8').trim().split('\n');
  const flags = [];
  for (let i = 0; i < original.length; i++) {
    let flag = original[i];
    if (flag === '-o') { i++; continue; }
    if (flag.includes('force_load') && (flag.includes('test_') || flag.includes('djinni'))) continue;
    if (flag.endsWith('libgtest_main.a')) continue;
    // Bazel can leave the constituent objects and archive response file after
    // removing an unrequested archive. Reassemble ONLY a temporary archive.
    if (flag.endsWith('.a') && !existsSync(path.join(execroot, flag))) {
      const objects = readFileSync(path.join(execroot, `${flag}-0.params`), 'utf8')
        .split('\n').filter(name => name.endsWith('.o')).map(name => path.join(execroot, name));
      const archive = path.join(fixture, path.basename(flag));
      success('/usr/bin/libtool', ['-static', '-o', archive, ...objects]);
      flag = archive;
    }
    flags.push(flag);
  }
  return flags;
}

test('real full and LEAN Hermes consume both native map paths; reverting either path fails', { timeout: 120000 }, t => {
  const native = nativePrerequisites(t);
  if (!native) return;
  const fixture = temporary(t);
  const { compiler, flags } = nativeFlags(native);
  const full = [...flags, '-DHERMES_ENABLE_DEBUGGER', '-DHERMES_MEMORY_INSTRUMENTATION',
    '-DHERMES_API', '-DVALDI_HAS_WEBSOCKET_SERVER'];
  const lean = [...flags, '-DHERMESVM_LEAN'];
  const consumer = path.join(fixtures, 'lossless-map-consumer.cpp');
  const programs = [...readFileSync(consumer, 'utf8').matchAll(/R"JS\(([\s\S]*?)\)JS"/g)];
  assert.equal(programs.length, 3);
  const { hermesc } = native;
  programs.forEach((program, i) => {
    const input = path.join(fixture, `${i}.js`);
    writeFileSync(input, program[1]);
    success(hermesc, ['-emit-binary', '-O', '-strict', '-Xes6-block-scoping', '-fno-static-builtins',
      '-out', path.join(fixture, `${i}.hbc`), input]);
  });
  const compile = (name, source, options = full) => {
    const object = path.join(fixture, `${name}.o`);
    success(compiler, [...options, '-c', source, '-o', object]);
    return object;
  };
  const units = [
    ['IJavaScriptContext', 'Interfaces/IJavaScriptContext.cpp'],
    ['JavaScriptUtils', 'JavaScript/JavaScriptUtils.cpp'],
    ['JavaScriptValueDelegate', 'JavaScript/JavaScriptValueDelegate.cpp'],
  ];
  const objects = units.map(([name, file]) => compile(name, path.join(root, 'valdi/src/valdi/runtime', file)));
  for (const mode of ['full', 'lean']) {
    const options = mode === 'full' ? full : lean;
    // Recompile the actual derived engine with the new base-class layout.
    const engine = compile(`HermesContext-${mode}`,
      path.join(root, 'valdi/src/valdi/hermes/HermesJavaScriptContext.cpp'), options);
    const main = compile(`consumer-${mode}`, consumer, options);
    const compilerObjects = mode === 'full' ? [compile('HermesCompiler',
      path.join(root, 'valdi/src/valdi/hermes/HermesJavaScriptCompiler.cpp'), options)] : [];
    const libraries = linkFlags(native.execroot, mode, fixture);
    const link = (suffix, runtimeObjects) => {
      const executable = path.join(fixture, `consumer-${mode}-${suffix}`);
      success(compiler, ['-Wl,-dead_strip', main, engine, ...compilerObjects, ...runtimeObjects, ...libraries,
        '-o', executable], { cwd: native.execroot });
      return executable;
    };
    const passed = success(link('current', objects), [fixture], { timeout: 15000 });
    assert.match(passed, mode === 'lean' ? /PASS real LEAN Hermes initialize/ : /PASS real full Hermes initialize/);
    t.diagnostic(passed.trim());
    if (mode === 'lean') {
      for (const [index, expected] of [[1, 'untyped'], [2, 'delegate']]) {
        const [name, file] = units[index];
        const original = path.join(fixture, `original-${name}.cpp`);
        writeFileSync(original, pinned(`valdi/src/valdi/runtime/${file}`));
        const oldObject = compile(`original-${name}`, original);
        const changed = [...objects];
        changed[index] = oldObject;
        const rejected = run(link(`negative-${name}`, changed), [fixture], { timeout: 15000 });
        assert.notEqual(rejected.status, 0, `Reverting ${name} must fail the JS consumer`);
        assert.match(rejected.stderr, new RegExp(`${expected}: (prototype|keys)`));
        t.diagnostic(`RED ${mode}: baseline ${name} fails the ${expected} consumer`);
      }
    }
  }
});

test('Foundation NSNumber branch preserves CFBoolean and numeric 0/1; old production branch fails', {
  timeout: 60000,
}, t => {
  const native = nativePrerequisites(t);
  if (!native) return;
  const fixture = temporary(t);
  const original = pinned(objc);
  const { compiler, flags } = nativeFlags(native);
  const objcFlags = [...flags, '-fobjc-arc', `-I${fixture}`,
    `-I${root}/valdi_core/src/valdi_core/ios`, `-I${root}/valdi_core/generated-src/objc`];
  // Compile the full official production unit too; no invented ABI declarations.
  success(compiler, [...objcFlags, '-c', path.join(root, objc), '-o', path.join(fixture, 'full-conversion.o')]);
  for (const [name, source] of [['current', current(objc)], ['original', original]]) {
    const branch = source.split('} else if ([object isKindOfClass:[NSNumber class]]) {')[1]
      .split('} else if ([object isKindOfClass:[NSDictionary class]])')[0];
    assert.ok(branch.includes('[object doubleValue]'));
    writeFileSync(path.join(fixture, 'NSNumberBranch.inc'), `if ([object isKindOfClass:[NSNumber class]]) {${branch}}\n`);
    const executable = path.join(fixture, `foundation-${name}`);
    success(compiler, [...objcFlags, path.join(fixtures, 'foundation-number-consumer.mm'),
      path.join(native.host, 'libreal_valdi_host.a'), path.join(native.host, 'deps/fmt/libfmt.a'),
      path.join(native.host, 'deps/jsoncpp/src/lib_json/libjsoncpp.a'),
      '-framework', 'Foundation', '-framework', 'CoreFoundation', '-Wl,-dead_strip', '-o', executable]);
    const result = run(executable, [], { timeout: 15000 });
    if (name === 'current') {
      assert.equal(result.status, 0, result.stderr);
      assert.match(result.stdout, /PASS Foundation CFBoolean and numeric NSNumber production branch/);
      t.diagnostic(result.stdout.trim());
    } else {
      assert.notEqual(result.status, 0, 'The old NSNumber-to-double branch must fail on CFBoolean');
      assert.match(result.stderr, /CFBoolean lost its boolean type/);
      t.diagnostic('RED: baseline NSNumber production branch loses CFBoolean type');
    }
  }
});
