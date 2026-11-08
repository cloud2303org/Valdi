// Regression fixture only; no Web API implementation. Parent C++ harness owns
// evaluating this Promise and draining the real Hermes microtask queue.
(async () => {
    function check(condition, message) {
        if (!condition) throw new Error(message);
    }
    async function rejectsTypeError(promise, label) {
        try { await promise; } catch (error) {
            check(error instanceof TypeError, label + ': expected TypeError');
            return error;
        }
        throw new Error(label + ': expected rejection');
    }
    {
        const writer = new WritableStream().getWriter();
        await writer.close();
        const old = writer.closed;
        await old;
        writer.releaseLock();
        check(writer.closed !== old, 'closed must be replaced after close/release');
        await rejectsTypeError(writer.closed, 'closed after release');
        await old;
    }
    {
        const writer = new WritableStream().getWriter();
        const write = writer.write('chunk');
        const old = writer.ready;
        await write;
        await old;
        writer.releaseLock();
        check(writer.ready !== old, 'ready must be replaced after drain/release');
        await rejectsTypeError(writer.ready, 'ready after release');
        await rejectsTypeError(writer.closed, 'unclosed released writer');
    }
    {
        const writer = new WritableStream(undefined, { highWaterMark: 0 }).getWriter();
        const old = writer.ready;
        await writer.close();
        await old;
        writer.releaseLock();
        check(writer.ready !== old, 'ready resolved by close must be replaced');
        await rejectsTypeError(writer.ready, 'zero-HWM ready after release');
        await rejectsTypeError(writer.closed, 'zero-HWM closed after release');
    }
    {
        const input = { ['\uD800']: 'x' };
        check(new URLSearchParams(input).toString() === '%EF%BF%BD=x',
              'record lookup must preserve original surrogate key');
        let rawReads = 0, replacementReads = 0;
        const collision = {};
        Object.defineProperty(collision, '\uD800', {
            enumerable: true, get() { ++rawReads; return 'raw'; }
        });
        Object.defineProperty(collision, '\uFFFD', {
            enumerable: true, get() { ++replacementReads; return 'replacement'; }
        });
        check(new URLSearchParams(collision).toString() ===
              '%EF%BF%BD=raw&%EF%BF%BD=replacement', 'distinct original properties');
        check(rawReads === 1 && replacementReads === 1, 'each original getter exactly once');
        const saved = globalThis.Reflect;
        try {
            globalThis.Reflect = { get() { return 'wrong'; } };
            check(new URLSearchParams(input).toString() === '%EF%BF%BD=x',
                  'record lookup must use captured intrinsic Reflect.get');
        } finally {
            globalThis.Reflect = saved;
        }
    }
    return 'nsh-web-review-r1:ok';
})()
