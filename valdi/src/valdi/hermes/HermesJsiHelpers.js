(function () {
  'use strict';
  const define = Object.defineProperty;
  const descriptor = Object.getOwnPropertyDescriptor;
  const prototypeOf = Object.getPrototypeOf;
  const apply = Reflect.apply;
  const string = String;
  const error = Error;
  const typeError = TypeError;
  const typedTag = descriptor(prototypeOf(Uint8Array.prototype), Symbol.toStringTag).get;
  const constructors = {Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array,
    Int32Array, Uint32Array, Float32Array, Float64Array};
  return {
    toBoolean(value) { return !!value; },
    toNumber(value) { return +value; },
    toInt32(value) { return value | 0; },
    toString(value) {
      if (typeof value === 'symbol') throw new typeError('Cannot convert a Symbol value to a string');
      return string(value);
    },
    defineData(object, key, value, enumerable, writable, configurable) {
      define(object, key, {value, enumerable, writable, configurable});
    },
    defineAccessor(object, key, get, set, enumerable, configurable) {
      define(object, key, {get, set, enumerable, configurable});
    },
    makeConstructor(initializer, name) {
      const Constructor = function (...args) {
        if (!new.target) throw new typeError('Native class constructor must be called with new');
        if (new.target !== Constructor) throw new typeError('Native class subclassing is not supported');
        apply(initializer, this, args);
      };
      define(Constructor, 'name', {value: name, configurable: true});
      define(Constructor, 'prototype', {writable: false});
      return Constructor;
    },
    typedArrayKind(value) { return apply(typedTag, value, []); },
    makeTypedArray(name, buffer) { return new constructors[name](buffer); },
    isError(value) { return value instanceof error; },
    installRejectionTracker(callback) {
      HermesInternal.enablePromiseRejectionTracker({allRejections: true, onUnhandled: callback});
    }
  };
})();
