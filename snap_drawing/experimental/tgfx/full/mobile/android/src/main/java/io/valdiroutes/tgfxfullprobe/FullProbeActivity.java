package io.valdiroutes.tgfxfullprobe;

import android.app.Activity;
import android.content.Context;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;

public final class FullProbeActivity extends Activity {
    private CheckRun checkRun;
    private TextView status;
    private TextView results;

    public static native NativeResult runChecks(String fixturesDirectory, String textDirectory);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.probe);
        status = findViewById(R.id.tgfx_full_status);
        results = findViewById(R.id.tgfx_full_results);
        Object retained = getLastNonConfigurationInstance();
        checkRun = retained instanceof CheckRun ? (CheckRun) retained : new CheckRun();
        checkRun.attach(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Launch once after foreground entry; rotation reattaches to CheckRun.
        checkRun.start(getApplicationContext());
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

    private static void deleteTree(File path) throws IOException {
        if (path.isDirectory()) {
            File[] children = path.listFiles();
            if (children == null) {
                throw new IOException("Cannot list " + path);
            }
            for (File child : children) {
                deleteTree(child);
            }
        }
        if (path.exists() && !path.delete()) {
            throw new IOException("Cannot delete old fixture " + path);
        }
    }

    private static void copyAssetTree(AssetManager assets, String source, File target) throws IOException {
        String[] children = assets.list(source);
        if (children == null) {
            throw new IOException("Cannot list bundled assets: " + source);
        }
        if (children.length != 0) {
            if (!target.isDirectory() && !target.mkdirs()) {
                throw new IOException("Cannot create fixture directory " + target);
            }
            for (String child : children) {
                copyAssetTree(assets, source + "/" + child, new File(target, child));
            }
        } else {
            // Empty/missing directories fail at open(); never synthesize data.
            try (InputStream input = assets.open(source);
                 FileOutputStream output = new FileOutputStream(target)) {
                byte[] buffer = new byte[16384];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    output.write(buffer, 0, count);
                }
            }
        }
    }

    private static String exceptionDetails(Throwable failure) {
        StringWriter text = new StringWriter();
        failure.printStackTrace(new PrintWriter(text));
        return text.toString();
    }

    private static File persistReport(Context application, NativeResult report) throws IOException {
        File directory = application.getFilesDir();
        if (directory == null || !directory.isDirectory()) {
            throw new IOException("Cannot locate app filesDir for full-results.txt");
        }
        File destination = new File(directory, "full-results.txt");
        File temporary = File.createTempFile("full-results-", ".tmp", directory);
        try {
            try (FileOutputStream output = new FileOutputStream(temporary)) {
                String header = "status=" + report.status + "\nsuccess=" + report.success
                        + "\npassed=" + report.passed + "\nfailed=" + report.failed
                        + "\nregistered=" + report.registered + "\nrunnerFailures=" + report.runnerFailures
                        + "\ncompletedUnixMs=" + System.currentTimeMillis()
                        + "\n\n--- report.details (verbatim) ---\n";
                output.write(header.getBytes(StandardCharsets.UTF_8));
                // No log-sized chunks, trimming or rewriting of the actual JNI report.
                output.write(report.details.getBytes(StandardCharsets.UTF_8));
                output.flush();
                output.getFD().sync();
            }
            // Same-directory POSIX rename atomically replaces the previous report.
            // Unlike AtomicFile.finishWrite(), rename errors are observable here.
            Os.rename(temporary.getAbsolutePath(), destination.getAbsolutePath());
            return destination;
        } catch (ErrnoException failure) {
            throw new IOException("Cannot replace " + destination, failure);
        } finally {
            if (temporary.exists() && !temporary.delete()) {
                Log.w("TGFX_FULL_RESULT", "Cannot remove temporary report " + temporary);
            }
        }
    }

    private static NativeResult persistenceFailure(NativeResult original, Throwable failure) {
        return new NativeResult(false, original.passed, original.failed, original.registered,
                original.runnerFailures + 1, "FAIL report persistence",
                "FAIL: cannot persist full-results.txt\n" + exceptionDetails(failure)
                + "\nOriginal status: " + original.status + "\n\n" + original.details);
    }

    // All state and the Activity reference below are main-thread-owned. The
    // worker captures application context only and posts its immutable result.
    private static final class CheckRun {
        private final Handler main = new Handler(Looper.getMainLooper());
        private FullProbeActivity activity;
        private boolean started;
        private NativeResult result;
        private String reportLocation;

        void attach(FullProbeActivity next) {
            activity = next;
            render();
        }

        void detach(FullProbeActivity previous) {
            if (activity == previous) {
                activity = null;
            }
        }

        void render() {
            if (activity == null) {
                return;
            }
            activity.status.setText(result == null ? (started ? "RUNNING" : "WAITING") : result.status);
            activity.results.setText(result == null
                    ? "Executing real full backend checks. No result yet."
                    : result.details + "\n\n" + reportLocation);
        }

        void start(Context application) {
            if (started) {
                return;
            }
            started = true;
            render();
            new Thread(() -> {
                NativeResult completed;
                try {
                    System.loadLibrary("snap_tgfx_full_probe");
                    File fixtures = new File(application.getFilesDir(), "tgfx-full-fixtures");
                    deleteTree(fixtures);
                    copyAssetTree(application.getAssets(), "fixtures", fixtures);
                    completed = runChecks(new File(fixtures, "media-data").getAbsolutePath(),
                                          new File(fixtures, "text-data").getAbsolutePath());
                    if (completed == null || completed.status == null || completed.details == null) {
                        completed = NativeResult.startupFailure("JNI returned no complete typed report.");
                    }
                } catch (LinkageError | RuntimeException | IOException failure) {
                    completed = NativeResult.startupFailure(exceptionDetails(failure));
                }
                String location;
                try {
                    location = "Saved report: " + persistReport(application, completed).getAbsolutePath();
                } catch (RuntimeException | IOException failure) {
                    completed = persistenceFailure(completed, failure);
                    try {
                        // Save an explicit failure if a transient first write failed.
                        // Even successful recovery cannot turn this run into PASS.
                        location = "Saved report: " + persistReport(application, completed).getAbsolutePath();
                    } catch (RuntimeException | IOException retryFailure) {
                        completed = persistenceFailure(completed, retryFailure);
                        location = "REPORT NOT SAVED: filesDir/full-results.txt";
                    }
                }
                final NativeResult report = completed;
                final String savedLocation = location;
                // Startup failures and successful suites share this persistence
                // path, which completes before any final UI result is posted.
                Log.i("TGFX_FULL_RESULT", report.status + " passed=" + report.passed
                        + " failed=" + report.failed + " registered=" + report.registered
                        + " runnerFailures=" + report.runnerFailures + " " + savedLocation);
                main.post(() -> {
                    result = report;
                    reportLocation = savedLocation;
                    render();
                });
            }, "tgfx-full-checks").start();
        }
    }
}
