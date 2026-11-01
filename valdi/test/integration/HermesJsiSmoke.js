// Compile as raw HBC (not a Valdi module envelope), with -emit-async-break-check.
// The completion value is the fixture object; no source evaluation is needed in lean builds.
(function () {
  'use strict';
  return {
    answer42: function () { return 42; },
    makePendingPromise: function (owner, adopted) {
      var promise = new Promise(function () {});
      promise.owner = owner;
      return adopted ? new Promise(function (resolve) { resolve(promise); }) : promise;
    },
    hasNativePromiseOwnershipBridge: function () {
      var descriptor = Object.getOwnPropertyDescriptor(globalThis, '__valdiNativePromiseOwnership');
      return !!descriptor && !descriptor.enumerable && typeof descriptor.value === 'function';
    },
    makeAdoptionFixtures: function (later) {
      var reason = {marker: 91};
      // Rejection is tracked before native ownership exists. Its timer must
      // retire the tracker record once ownership reaches this inner promise.
      var inner = Promise.reject(reason);
      var middle = new Promise(function (resolve) { resolve(inner); });
      var resolveOuter;
      var outer = new Promise(function (resolve) { resolveOuter = resolve; });
      function adopt() {
        resolveOuter(middle);
        resolveOuter = null;
      }
      if (!later) adopt();
      return {outer: outer, middle: middle, inner: inner, reason: reason, adopt: adopt};
    },
    makeRejectionFixtures: function () {
      var reason = {marker: 73};
      var owned = Promise.reject(reason);
      var rejectLater;
      var pending = new Promise(function (_, reject) { rejectLater = reject; });
      Promise.resolve().then(function () { rejectLater(reason); });
      var unrelated = Promise.reject(reason);
      var hooks = {then: 0, constructor: 0, species: 0};
      [owned, pending].forEach(function (promise) {
        var constructor = {};
        // Species lookup belongs to promise.constructor, not the promise.
        // Some Hermes builds do not expose Symbol.species.
        if (Symbol.species) Object.defineProperty(constructor, Symbol.species, {get: function () {
          hooks.species++;
          return Promise;
        }});
        Object.defineProperty(promise, 'then', {get: function () {
          hooks.then++;
          return Promise.prototype.then;
        }});
        Object.defineProperty(promise, 'constructor', {get: function () {
          hooks.constructor++;
          return constructor;
        }});
      });
      return {reason: reason, owned: owned, pending: pending, unrelated: unrelated, hooks: hooks};
    },
    sum: function (a, b) { return a + b; },
    loop: function (ready) {
      ready();
      // No native calls or allocations after the readiness handshake.
      while (true) {}
    },
    makeOffsetTypedArray: function (buffer) {
      buffer = buffer || new ArrayBuffer(16);
      var bytes = new Uint8Array(buffer);
      for (var i = 0; i < bytes.length; i++) bytes[i] = i;
      return {
        buffer: buffer,
        bytes: bytes,
        offset: new Uint16Array(buffer, 4, 3),
        dataView: new DataView(buffer, 3, 5),
        empty: new Uint8Array(buffer, buffer.byteLength, 0)
      };
    },
    invokeNative: function (fn, value) { return fn(value); },
    exerciseNativeClass: function (Constructor, nativeCreated) {
      var instance = new Constructor(40);
      function rejects(callback) {
        try { callback(); } catch (error) { return error instanceof TypeError; }
        return false;
      }
      var descriptor = Object.getOwnPropertyDescriptor(Constructor.prototype, 'add');
      return instance.add(2) === 42 && instance.value === 42 && nativeCreated.add(2) === 12 &&
        !descriptor.writable && descriptor.enumerable && !descriptor.configurable &&
        rejects(function () { Constructor(0); }) &&
        rejects(function () { return new (class extends Constructor {})(0); }) &&
        rejects(function () { Constructor.prototype.add.call(Object.create(instance), 1); }) &&
        rejects(function () { Constructor.prototype.add.call({}, 1); });
    },
    catchNative: function (fn, token) {
      try { fn(token); } catch (error) { return error === token; }
      return false;
    },
    inspectDescriptors: function (object) {
      var hidden = Object.getOwnPropertyDescriptor(object, 'hidden');
      var visible = Object.getOwnPropertyDescriptor(object, 'visible');
      return hidden.value === 7 && !hidden.enumerable && hidden.writable && hidden.configurable &&
        visible.value === 9 && visible.enumerable && visible.writable && visible.configurable &&
        Object.keys(object).join(',') === 'visible';
    },
    buildOtherFixtures: function (native) {
      var renamed = new Uint16Array([10, 20]);
      // A constructor's mutable name is not the engine's internal typed-array kind.
      Object.defineProperty(Uint16Array, 'name', {value: 'Float64Array', configurable: true});
      var tagged = new Int8Array([1, -2]);
      Object.defineProperty(tagged, Symbol.toStringTag, {value: 'Float64Array'});
      var fake = {constructor: {name: 'Uint8Array'}, buffer: new ArrayBuffer(8), byteOffset: 0, byteLength: 8};
      fake[Symbol.toStringTag] = 'Uint8Array';
      var unsupported = new BigInt64Array(1);
      Object.defineProperty(unsupported, 'constructor', {value: {name: 'Uint8Array'}});
      Object.defineProperty(unsupported, Symbol.toStringTag, {value: 'Uint8Array'});
      var token = {marker: 73};
      var properties = Object.create({inherited: 1});
      Object.defineProperty(properties, 'hidden', {value: 2});
      Object.defineProperty(properties, 'throwing', {
        enumerable: true,
        get: function () { throw token; }
      });
      properties.visible = 3;
      properties[Symbol('private')] = 4;
      return {
        renamed: renamed,
        tagged: tagged,
        fake: fake,
        unsupported: unsupported,
        inheritedNative: Object.create(native || null),
        copiedNative: Object.assign({}, native),
        properties: properties,
        token: token
      };
    }
  };
})();
