package io.valdiroutes.tgfxfullprobe;

// JNI constructs this real class, never an untyped PASS string.
public final class NativeResult {
    public final boolean success;
    public final int passed;
    public final int failed;
    public final int registered;
    public final int runnerFailures;
    public final String status;
    public final String details;

    public NativeResult(boolean success, int passed, int failed, int registered,
                        int runnerFailures, String status, String details) {
        this.success = success;
        this.passed = passed;
        this.failed = failed;
        this.registered = registered;
        this.runnerFailures = runnerFailures;
        this.status = status;
        this.details = details;
    }

    static NativeResult startupFailure(String details) {
        return new NativeResult(false, 0, 0, 0, 1, "FAIL startup", details);
    }
}
