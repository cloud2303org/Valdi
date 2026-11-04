package io.valdiroutes.renderperformance;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;

public final class BenchmarkActivity extends Activity {
    private static final AtomicBoolean STARTED = new AtomicBoolean(false);
    private static final String REPORT = "render-performance.json";
    private static final long STARTUP_SETTLE_MS = 8_000L;
    private final Handler startupHandler = new Handler(Looper.getMainLooper());
    private volatile boolean foreground;
    private TextView status;
    private TextView results;
    private static native String runNative(String fixturesDirectory);
    private final Runnable startAfterSettle = () -> {
        if (!foreground || isFinishing() || isDestroyed() || !STARTED.compareAndSet(false, true)) { return; }
        status.setText("RUNNING");
        new Thread(this::runBenchmark, "render-performance").start();
    };

    @Override public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(24, 24, 24, 24);
        status = new TextView(this);
        status.setId(R.id.render_performance_status);
        status.setTextSize(24);
        status.setText("WAITING");
        status.setContentDescription("render-performance-status");
        results = new TextView(this);
        results.setId(R.id.render_performance_results);
        results.setTextIsSelectable(true);
        results.setText("GPU render + completed readback throughput; not display FPS. Energy is unmeasured.");
        ScrollView scroll = new ScrollView(this);
        scroll.addView(results);
        content.addView(status);
        content.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(content);
    }

    @Override public void onResume() {
        super.onResume();
        foreground = true;
        if (STARTED.get()) { return; }
        status.setText("SETTLING");
        startupHandler.removeCallbacks(startAfterSettle);
        // Launch screenshot/tree collection settles before the worker exists.
        // No core timer or warmup sample includes this foreground delay.
        startupHandler.postDelayed(startAfterSettle, STARTUP_SETTLE_MS);
    }

    @Override public void onPause() {
        foreground = false;
        startupHandler.removeCallbacks(startAfterSettle);
        if (!STARTED.get()) { status.setText("WAITING"); }
        super.onPause();
    }

    @Override public void onDestroy() {
        startupHandler.removeCallbacks(startAfterSettle);
        super.onDestroy();
    }

    private void copyFixtures(File destination) throws Exception {
        if (!destination.isDirectory() && !destination.mkdirs()) {
            throw new IllegalStateException("Cannot create fixture directory");
        }
        String[] names = getAssets().list("fixtures");
        if (names == null || names.length == 0) { throw new IllegalStateException("Missing bundled fixtures"); }
        byte[] buffer = new byte[32768];
        for (String name : names) {
            try (InputStream source = getAssets().open("fixtures/" + name);
                 FileOutputStream output = new FileOutputStream(new File(destination, name))) {
                int count;
                while ((count = source.read(buffer)) != -1) { output.write(buffer, 0, count); }
            }
        }
    }

    private static void save(File file, String report) throws Exception {
        try (FileOutputStream stream = new FileOutputStream(file, false)) {
            stream.write(report.getBytes(StandardCharsets.UTF_8));
            stream.getFD().sync();
        }
    }

    private static String failure(Throwable error) {
        try {
            JSONObject result = new JSONObject();
            result.put("schema_version", 1);
            result.put("success", false);
            result.put("error", error.toString());
            result.put("expected_backend", BuildConfig.EXPECTED_BACKEND);
            return result.toString();
        } catch (Exception ignored) {
            return "{\"schema_version\":1,\"success\":false,\"error\":\"Host failure formatting failed\"}";
        }
    }

    private void runBenchmark() {
        String report;
        boolean passed = false;
        try {
            if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
                throw new IllegalStateException("A non-debuggable Release host is required");
            }
            File fixtures = new File(getFilesDir(), "render-fixtures");
            copyFixtures(fixtures);
            System.loadLibrary("render_performance");
            if (!foreground) {
                throw new IllegalStateException("App left foreground before native benchmark start");
            }
            report = runNative(fixtures.getAbsolutePath());
            JSONObject parsed = new JSONObject(report);
            // A valid native failure remains verbatim. A wrong backend or a
            // non-optimized native build is a host validation failure.
            if (!BuildConfig.EXPECTED_BACKEND.equals(parsed.getString("backend")) ||
                !parsed.getBoolean("compiler_optimized")) {
                throw new IllegalStateException("Wrong backend or non-optimized native library");
            }
            passed = parsed.getBoolean("success") && parsed.getInt("completed_scenes") == 5;
        } catch (Throwable error) {
            report = failure(error);
        }
        String location = "";
        try {
            // No storage permissions or run-as/debuggable access is needed.
            File external = getExternalFilesDir(null);
            if (external == null) { throw new IllegalStateException("External app files directory unavailable"); }
            File internalFile = new File(getFilesDir(), REPORT);
            File externalFile = new File(external, REPORT);
            save(internalFile, report);
            save(externalFile, report);
            location = "\nReport: " + externalFile.getAbsolutePath();
        } catch (Throwable error) {
            passed = false;
            // Keep the original native JSON as evidence when persistence fails.
            try {
                JSONObject failed = new JSONObject();
                failed.put("success", false);
                failed.put("error", "Report persistence failed: " + error);
                failed.put("native_report", new JSONObject(report));
                report = failed.toString();
                save(new File(getFilesDir(), REPORT), report);
                File external = getExternalFilesDir(null);
                if (external != null) { save(new File(external, REPORT), report); }
            } catch (Throwable ignored) {
                location = "\nFAIL: could not persist the failure report; UI retains report";
            }
        }
        final String display = report + location;
        final boolean success = passed;
        runOnUiThread(() -> {
            status.setText(success ? "PASS" : "FAIL");
            results.setText(display);
        });
    }
}
