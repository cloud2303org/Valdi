import assert from 'node:assert/strict';
import { mkdirSync, readdirSync } from 'node:fs';
import { homedir } from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { root, fixtures, current, pinned, run, success, temporary, write } from './support.mjs';

const helpers = path.join(fixtures, 'android-promise-rejection');
const javaRoot = 'valdi/src/java/com/snap/valdi';
const marker = `${javaRoot}/exceptions/PromiseRejectionException.kt`;
const callback = `${javaRoot}/promise/CppPromiseCallback.kt`;
const productionFiles = [
  'exceptions/ValdiException.kt', 'exceptions/ExceptionUtils.kt',
  'promise/CppPromiseCallback.kt', 'promise/Promise.kt', 'promise/PromiseCallback.kt',
  'promise/ResolvablePromise.kt', 'promise/CancelableResolvablePromise.kt',
  'promise/ResolvedPromise.kt', 'promise/RejectedPromise.kt',
  'utils/CppNativeHandlePair.kt', 'utils/Disposable.kt', 'utils/DisposablePrivate.kt',
  'utils/DisposableUtils.kt',
].map(name => `${javaRoot}/${name}`);

function fixture(directory) {
  for (const name of [...productionFiles, marker]) write(directory, name, current(name));
}

test('current marker belongs to valdi_java; existing Promise and diagnostic units retain baseline bytes', () => {
  for (const name of productionFiles.filter(name => name !== callback)) {
    assert.equal(current(name), pinned(name), name);
  }
  const build = current('valdi/BUILD.bazel');
  assert.match(build, /name = "valdi_java",\s+srcs = glob\(\[\s+"src\/java\/\*\*\/\*\.java",\s+"src\/java\/\*\*\/\*\.kt",\s+\]\)/);
  const nestedBuilds = success('git', ['-C', root, 'ls-files', 'valdi/src/java'])
    .split('\n').filter(name => /\/BUILD(?:\.bazel)?$/.test(name));
  assert.ok(!nestedBuilds.some(name => marker.startsWith(`${path.posix.dirname(name)}/`)),
    'A nested Bazel package would exclude the marker from valdi_java');
  assert.match(current(marker), /@Keep\s+class PromiseRejectionException @JvmOverloads constructor/);
});

function toolchain() {
  const gradle = path.join(homedir(), '.gradle');
  let lib = process.env.VALDI_TEST_KOTLIN_LIB;
  if (!lib) {
    const jars = success('rg', ['--files', path.join(gradle, 'wrapper/dists')]).trim().split('\n');
    const compiler = jars.filter(name => /\/kotlin-compiler-embeddable-[^/]+\.jar$/.test(name)).sort().at(-1);
    lib = compiler && path.dirname(compiler);
  }
  assert.ok(lib, 'Set VALDI_TEST_KOTLIN_LIB to an existing Kotlin compiler/Gradle lib directory');
  const stdlib = readdirSync(lib).find(name => /^kotlin-stdlib-[\d.]+\.jar$/.test(name));
  assert.ok(stdlib, `Missing kotlin-stdlib in ${lib}`);
  const annotations = process.env.VALDI_TEST_ANDROIDX_ANNOTATION ?? success('rg', ['--files',
    path.join(gradle, 'caches/modules-2/files-2.1/androidx.annotation')]).trim().split('\n')
    .find(name => /\/annotation(?:-jvm)?-[\d.]+\.jar$/.test(name));
  assert.ok(annotations, 'Set VALDI_TEST_ANDROIDX_ANNOTATION to an existing AndroidX annotation jar');
  const settings = run('java', ['-XshowSettings:properties', '-version']);
  assert.equal(settings.status, 0, settings.stderr);
  const javaHome = settings.stderr.match(/^\s*java\.home = (.+)$/m)?.[1];
  assert.ok(javaHome, 'Cannot determine JDK home from java');
  return { lib, stdlib: path.join(lib, stdlib), annotations, javaHome };
}

test('actual Kotlin Promise callback crosses JNI: baseline is red, current fork is green', { timeout: 120000 }, t => {
  const directory = temporary(t);
  fixture(directory);
  const tools = toolchain();
  t.diagnostic(`Kotlin compiler: ${tools.lib}; JDK: ${tools.javaHome}`);
  const classes = path.join(directory, 'classes');
  mkdirSync(classes);
  const library = path.join(directory, process.platform === 'darwin' ? 'capture.dylib' : 'capture.so');
  success(process.env.CC ?? 'cc', [
    process.platform === 'darwin' ? '-dynamiclib' : '-shared', '-fPIC', '-Wall', '-Wextra',
    '-Wno-unused-parameter', '-Werror',
    `-I${tools.javaHome}/include`, `-I${tools.javaHome}/include/${process.platform === 'darwin' ? 'darwin' : 'linux'}`,
    path.join(helpers, 'capture.c'), '-o', library,
  ]);
  success(path.join(tools.javaHome, 'bin/javac'), ['-d', classes, path.join(helpers, 'NativeBridge.java')]);
  const classpath = [classes, tools.stdlib, tools.annotations].join(path.delimiter);
  const compile = () => success('java', [
    '-cp', path.join(tools.lib, '*'), 'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler',
    '-no-stdlib', '-no-reflect', '-jvm-target', '17', '-classpath', classpath, '-d', classes,
    ...[...productionFiles, marker].map(name => path.join(directory, name)),
    path.join(helpers, 'PromiseRejectionConsumer.kt'),
  ], { timeout: 45000 });
  const consume = () => run('java', [
    '-Xcheck:jni', '-cp', classpath, 'PromiseRejectionConsumerKt', library,
  ], { timeout: 15000 });
  // Give the old callback the new marker API only. This isolates the behavioral
  // regression rather than treating an unresolved class as a useful red result.
  write(directory, callback, pinned(callback));
  compile();
  const red = consume();
  assert.equal(red.status, 1, red.stderr);
  assert.deepEqual([...red.stdout.matchAll(/^FAIL ([^:]+):/gm)].map(match => match[1]), [
    'public-cancel', 'public-error', 'explicit-empty-failure', 'explicit-whitespace-failure',
    'explicit-coded-cancel', 'explicit-other-code', 'pending-resolvable-cancel',
    'late-resolvable-empty-failure', 'producer-settles-during-cancel',
  ], red.stdout);
  assert.match(red.stdout, /Expected message <cancel>, got <PromiseRejectionException: 'cancel'>/);
  t.diagnostic('RED: old callback fails 9 raw-message cases; existing diagnostics/cancellation cases pass.');
  write(directory, callback, current(callback));
  compile();
  const green = consume();
  assert.equal(green.status, 0, `${green.stdout}\n${green.stderr}`);
  assert.equal([...green.stdout.matchAll(/^PASS /gm)].length, 19, green.stdout);
  assert.ok(!green.stderr.includes('WARNING in native method'), green.stderr);
  t.diagnostic(green.stdout.trim());
  // Trace compatibility with this checkout's actual Android native registration.
  const jni = current('valdi/src/valdi/android/CppPromiseCallbackJNI.hpp');
  assert.match(jni, /jstring error,\s+jint errorCode/);
  assert.match(jni, /Valdi::Error\(toInternedString\(JavaEnv\(\), error\), errorCode\)/);
  assert.match(jni, /makeNativeMethod\("nativeOnFailure", CppPromiseCallbackJNI::nativeOnFailure\)/);
  const bytecode = success(path.join(tools.javaHome, 'bin/javap'),
    ['-classpath', classes, '-s', '-p', 'com.snap.valdi.promise.CppPromiseCallback']);
  assert.match(bytecode, /native void nativeOnFailure\(long, long, java.lang.String, int\);\s+descriptor: \(JJLjava\/lang\/String;I\)V/);
});
