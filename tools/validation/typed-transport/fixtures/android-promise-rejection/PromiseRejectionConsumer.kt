import com.snap.valdi.exceptions.PromiseRejectionException
import com.snap.valdi.exceptions.ValdiException
import com.snap.valdi.exceptions.messageWithCauses
import com.snap.valdi.promise.CancelableResolvablePromise
import com.snap.valdi.promise.CppPromiseCallback
import com.snap.valdi.promise.Promise
import com.snap.valdi.promise.ResolvablePromise
import java.util.concurrent.CancellationException

// These endpoints only observe the real callback's native call. No Promise or
// serialization implementation is replaced by the fixture.
object NativeProbe {
    @JvmStatic external fun reset()
    @JvmStatic external fun message(): String?
    @JvmStatic external fun errorCode(): Int
    @JvmStatic external fun handles(): LongArray
    @JvmStatic external fun failureCount(): Int
    @JvmStatic external fun successCount(): Int
}

fun main(args: Array<String>) {
    System.load(args.single())
    var failures = 0
    fun verify(name: String, expectedMessage: String, expectedCode: Int = 0,
               settle: (CppPromiseCallback<String>) -> Unit) {
        NativeProbe.reset()
        val callback = CppPromiseCallback<String>(17L, 29L)
        try {
            settle(callback)
            check(NativeProbe.failureCount() == 1) { "Expected exactly one failure" }
            check(NativeProbe.successCount() == 0) { "Failure became success" }
            check(NativeProbe.handles().contentEquals(longArrayOf(17L, 29L))) { "Handles changed" }
            check(NativeProbe.errorCode() == expectedCode) {
                "Expected code $expectedCode, got ${NativeProbe.errorCode()}"
            }
            check(NativeProbe.message() == expectedMessage) {
                "Expected message <$expectedMessage>, got <${NativeProbe.message()}>"
            }
            callback.dispose() // Real CppNativeHandlePair must have transferred both handles.
            println("PASS $name")
        } catch (error: Throwable) {
            failures++
            println("FAIL $name: ${error.message}")
        }
    }
    fun reject(name: String, error: Throwable, message: String, code: Int = 0) {
        verify(name, message, code) { callback -> Promise.reject<String>(error).onComplete(callback) }
    }
    reject("public-cancel", PromiseRejectionException("cancel"), "cancel")
    reject("public-error", PromiseRejectionException("camera unavailable"), "camera unavailable")
    reject("explicit-empty-failure", PromiseRejectionException(""), "")
    reject("explicit-whitespace-failure", PromiseRejectionException(" \t\n"), " \t\n")
    reject("explicit-coded-cancel", PromiseRejectionException("cancel", 101), "cancel", 101)
    reject("explicit-other-code", PromiseRejectionException("denied", 403), "denied", 403)
    reject("nonmarker-cancel", IllegalStateException("cancel"), "IllegalStateException: 'cancel'")
    reject("nonmarker-empty", IllegalStateException(""), "IllegalStateException: ''")
    reject("nonmarker-null", IllegalStateException(), "IllegalStateException: 'Unknown Error'")
    reject("nonmarker-coded", ValdiException("native rejected", 101), "ValdiException: 'native rejected'", 101)
    reject("nonmarker-uncoded-valdi", ValdiException("cancel"), "ValdiException: 'cancel'")
    reject("caused-throwable", IllegalStateException("outer", IllegalArgumentException("inner")),
        "IllegalStateException: 'outer', Caused by: IllegalArgumentException: 'inner'")
    reject("caused-valdi", ValdiException("outer", IllegalArgumentException("inner"), 73),
        "ValdiException: 'outer', Caused by: IllegalArgumentException: 'inner'", 73)
    reject("coroutine-style-cancellation", CancellationException("stop"), "CancellationException: 'stop'", 101)
    // A marker buried in a cause does not opt the outer throwable into raw text.
    reject("caused-marker", IllegalStateException("outer", PromiseRejectionException("inner", 101)),
        "IllegalStateException: 'outer', Caused by: PromiseRejectionException: 'inner'")

    verify("pending-resolvable-cancel", "cancel") { callback ->
        val promise = ResolvablePromise<String>()
        promise.onComplete(callback)
        promise.fulfillFailure(PromiseRejectionException("cancel"))
    }
    verify("late-resolvable-empty-failure", "") { callback ->
        val promise = ResolvablePromise<String>()
        promise.fulfillFailure(PromiseRejectionException(""))
        promise.onComplete(callback)
    }
    verify("producer-settles-during-cancel", "cancel", 101) { callback ->
        val promise = object : CancelableResolvablePromise<String>() {
            override fun onCancel() = fulfillFailure(PromiseRejectionException("cancel", 101))
        }
        promise.onComplete(callback)
        promise.cancel()
        promise.cancel()
        promise.fulfillSuccess("too late")
    }
    verify("unsettled-cancellation", "PromiseCanceledException: 'Promise canceled'", 101) { callback ->
        val promise = object : CancelableResolvablePromise<String>() {
            override fun onCancel() {}
        }
        promise.onComplete(callback)
        promise.cancel()
        promise.cancel()
        promise.fulfillFailure(PromiseRejectionException("too late"))
    }
    // The opt-in applies at the Promise callback only; the shared utility must
    // retain its diagnostic formatting even for the new marker.
    check(PromiseRejectionException("cancel").messageWithCauses() == "PromiseRejectionException: 'cancel'")
    check(ValdiException("cancel").messageWithCauses() == "ValdiException: 'cancel'")
    // Java callers must get both promised constructor signatures.
    val type = PromiseRejectionException::class.java
    check((type.getConstructor(String::class.java).newInstance("java") as ValdiException).errorCode == 0)
    check((type.getConstructor(String::class.java, Int::class.javaPrimitiveType)
        .newInstance("java", 101) as ValdiException).errorCode == 101)
    if (failures != 0) kotlin.system.exitProcess(1)
}
