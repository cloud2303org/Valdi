import assert from 'node:assert/strict';
import test from 'node:test';
import { current, pinned } from './support.mjs';

// Fast source contracts are deliberately separate from the linked consumers.
// Passing these checks alone does not demonstrate engine or bridge behavior.
test('current native map paths call the shared own-property helper', () => {
  const utils = current('valdi/src/valdi/runtime/JavaScript/JavaScriptUtils.cpp');
  const delegate = current('valdi/src/valdi/runtime/JavaScript/JavaScriptValueDelegate.cpp');
  assert.ok(utils.includes('jsContext.setMapProperty(object.get(), it.first.toStringView(), value.get(), exceptionTracker)'));
  assert.ok(delegate.includes('_jsContext->setMapProperty(map.get(), key.get(), value.get(), toJSExceptionTracker(exceptionTracker))'));
  for (const name of ['JavaScriptUtils', 'JavaScriptValueDelegate']) {
    assert.doesNotMatch(pinned(`valdi/src/valdi/runtime/JavaScript/${name}.cpp`), /\.setMapProperty\(/);
  }
});

test('current context captures and releases map intrinsics without source evaluation', () => {
  const source = current('valdi/src/valdi/runtime/Interfaces/IJavaScriptContext.cpp');
  const header = current('valdi/src/valdi/runtime/Interfaces/IJavaScriptContext.hpp');
  const initialize = source.split('void IJavaScriptContext::initialize(')[1]
    .split('void IJavaScriptContext::prepareForTeardown(')[0];
  const teardown = source.split('void IJavaScriptContext::prepareForTeardown(')[1]
    .split('void IJavaScriptContext::setMapProperty(')[0];
  const helper = source.split('void IJavaScriptContext::setMapProperty(').slice(1).join('\n')
    .split('JSValueRef IJavaScriptContext::newBool(')[0];
  assert.doesNotMatch(initialize + helper, /\b(?:evaluate|evaluateScript|evaluateNative)\s*\(/);
  for (const [field, intrinsic] of [['_defineMapProperty', 'defineProperty'], ['_createMapDescriptor', 'create']]) {
    assert.ok(initialize.includes(`${field} = ensureRetainedValue(`));
    assert.ok(initialize.includes(`"${intrinsic}"`));
    assert.ok(teardown.includes(`${field} = JSValueRef();`));
    assert.ok(header.includes(`JSValueRef ${field};`));
    assert.ok(helper.includes(`callObjectAsFunction(${field}.get(),`));
  }
  assert.match(helper, /name\.toStringView\(\) != "__proto__"/);
  assert.match(helper, /auto nullValue = newNull\(\)/);
  assert.match(helper, /"writable", "enumerable", "configurable"/);
});

test('current Foundation NSNumber branch distinguishes CFBoolean by CoreFoundation type', () => {
  const name = 'valdi_core/src/valdi_core/ios/valdi_core/SCValdiObjCConversionUtils.mm';
  const branch = current(name).split('} else if ([object isKindOfClass:[NSNumber class]]) {')[1]
    .split('} else if ([object isKindOfClass:[NSDictionary class]])')[0];
  assert.match(branch, /CFGetTypeID\(\(__bridge CFTypeRef\)object\) == CFBooleanGetTypeID\(\)/);
  assert.match(branch, /return Valdi::Value\(static_cast<bool>\(\[object boolValue\]\)\)/);
  assert.match(branch, /return Valdi::Value\(\[object doubleValue\]\)/);
  assert.doesNotMatch(pinned(name), /CFBooleanGetTypeID\(\)/);
});
