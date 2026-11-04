package io.valdiroutes.tgfxprobe;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.TextView;

public final class ProbeActivity extends Activity {
    private CheckRun checkRun;
    private TextView results;

    public static native String runChecks();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.probe);
        results = findViewById(R.id.probe_results);

        Object retained = getLastNonConfigurationInstance();
        checkRun = retained instanceof CheckRun ? (CheckRun) retained : new CheckRun();
        checkRun.attach(this);
        checkRun.start();
    }

    @Override
    public Object onRetainNonConfigurationInstance() {
        return checkRun;
    }

    @Override
    protected void onDestroy() {
        if (checkRun != null) {
            checkRun.detach(this);
        }
        super.onDestroy();
    }

    // State and the Activity reference are accessed only on the main thread.
    // Retaining this object preserves the single native call across rotation.
    private static final class CheckRun {
        private final Handler main = new Handler(Looper.getMainLooper());
        private ProbeActivity activity;
        private boolean started;
        private String text = "RUNNING\nExecuting 16 native TGFX vector checks...\n";

        void attach(ProbeActivity next) {
            activity = next;
            activity.results.setText(text);
        }

        void detach(ProbeActivity previous) {
            if (activity == previous) {
                activity = null;
            }
        }

        void start() {
            if (started) {
                return;
            }
            started = true;
            new Thread(() -> {
                String report;
                try {
                    System.loadLibrary("snap_tgfx_probe");
                    report = runChecks();
                    if (report == null) {
                        report = "FAIL\nJNI returned no report.\n";
                    }
                } catch (LinkageError | RuntimeException failure) {
                    report = "FAIL\n" + failure.getClass().getSimpleName()
                            + ": " + failure.getMessage() + "\n";
                }
                final String completed = report;
                main.post(() -> {
                    // JNI owns the status decision; display the full report verbatim.
                    text = completed;
                    if (activity != null) {
                        activity.results.setText(text);
                    }
                });
            }, "tgfx-vector-checks").start();
        }
    }
}
