package io.valdiroutes.renderbenchmark

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.AtomicFile
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.TextView
import com.snap.modules.render_benchmark.BenchmarkContext
import com.snap.valdi.ExceptionReporter
import com.snap.valdi.RenderBackend
import com.snap.valdi.ValdiContextConfiguration
import com.snap.valdi.ValdiRuntimeManager
import com.snap.valdi.ValdiTweaks
import com.snap.valdi.bundle.IValdiCustomModuleProvider
import com.snap.valdi.exceptions.ValdiException
import com.snap.valdi.makeScoped
import com.snap.valdi.modules.ModuleFactoryRegistry
import com.snap.valdi.support.SupportValdiRuntimeManager
import com.snap.valdi.views.ValdiRootView
import com.snapchat.client.valdi_core.JavaScriptEngineType
import java.io.File
import java.util.UUID
import org.json.JSONArray
import org.json.JSONObject

/** Same native root and runtime setup for Skia and TGFX managed drawing. */
class BenchmarkActivity : Activity() {
    private val main = Handler(Looper.getMainLooper())
    private var manager: ValdiRuntimeManager? = null
    private var root: ValdiRootView? = null
    private lateinit var status: TextView
    private lateinit var column: LinearLayout
    private var directory: File? = null
    private var exportDirectory: File? = null
    private var provenance = JSONObject()
    private var backend = "unknown"
    private val events = JSONArray()
    private var startedAtMs: Double? = null
    private var startedCpuMs: Long? = null
    private var cpuProxy: JSONObject? = null
    private val suiteIds = listOf("cached-vector", "shaped-text", "image-svg", "lottie")
    private val sceneIds = mutableListOf<String>()
    private var sceneFailed = false
    private var terminal = false
    private var destroyed = false
    private var foreground = false
    private var startupReady = false
    private var runtimeStarted = false
    private val watchdog = Runnable { fail("No finished report within 180 seconds of native startup") }
    private val delayedStartup = Runnable {
        if (!destroyed && !terminal && foreground && !runtimeStarted) {
            runtimeStarted = true
            startRuntime()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        status = TextView(this).apply {
            id = R.id.render_benchmark_status
            contentDescription = "render-benchmark-status"
            text = "unknown STARTING"
            textSize = 18f
        }
        column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(status, LinearLayout.LayoutParams(-1, -2))
        }
        setContentView(column)
        try {
            val base = File(filesDir, "render-benchmark")
            directory = File(base, UUID.randomUUID().toString()).also {
                check(it.mkdirs()) { "Cannot create report directory $it" }
            }
            atomic(File(base, "latest.txt"), directory!!.absolutePath)
            val external = getExternalFilesDir(null) ?: error("Benchmark export storage unavailable")
            exportDirectory = File(external, "render-benchmark/${directory!!.name}").also {
                check(it.mkdirs()) { "Cannot create benchmark export directory $it" }
            }
            atomic(File(exportDirectory!!.parentFile, "latest.txt"), exportDirectory!!.absolutePath)
            provenance = JSONObject(assets.open("provenance.json").bufferedReader().use { it.readText() })
            backend = provenance.getString("backend")
            require(backend in setOf("skia", "tgfx")) { "Invalid backend" }
            require(provenance.getString("config").equals("release", ignoreCase = true)) { "Stage is not Release" }
            require(provenance.getString("runtimeSha256").matches(Regex("[0-9a-f]{64}"))) {
                "Missing runtime hash"
            }
            persist("SETTLING")
            if (terminal) return
            main.postDelayed(watchdog, 180_000)
            startupReady = true
        } catch (error: Throwable) {
            fail("Native startup: ${error.stackTraceToString()}")
        }
    }

    override fun onResume() {
        super.onResume()
        foreground = true
        if (startupReady && !runtimeStarted && !terminal && !destroyed)
            main.postDelayed(delayedStartup, 8_000)
    }

    override fun onPause() {
        foreground = false
        main.removeCallbacks(delayedStartup)
        super.onPause()
    }

    private fun startRuntime() {
        try {
            System.loadLibrary("valdi_runtime")
            ModuleFactoryRegistry.registerModuleFactoriesFromContext(applicationContext)
            val provider = object : IValdiCustomModuleProvider {
                override fun getCustomModuleData(path: String): ByteArray? {
                    if (path !in setOf("drawing.valdimodule", "drawing.map.json",
                            "render_benchmark.valdimodule", "render_benchmark.map.json")) return null
                    // Debug maps are optional in the Release stage.
                    if (path.endsWith(".map.json") && !assets.list("modules").orEmpty().contains(path)) return null
                    return try {
                        assets.open("modules/$path").use { it.readBytes() }
                    } catch (error: Exception) {
                        main.post { fail("Module $path: ${error.message}") }
                        throw ValdiException("Cannot load benchmark module $path")
                    }
                }
            }
            val runtimeManager = ValdiRuntimeManager(
                applicationContext,
                tweaks = ValdiTweaks(javaScriptEngineType = JavaScriptEngineType.HERMES),
                customModuleProvider = provider)
            manager = runtimeManager
            runtimeManager.exceptionReporter = object : ExceptionReporter {
                override fun reportNonFatal(errorCode: Int, message: String, module: String?, stackTrace: String?) {
                    main.post { fail("Valdi $errorCode $module: $message\n${stackTrace.orEmpty()}") }
                }
                override fun reportCrash(message: String, module: String?, stackTrace: String?, isANR: Boolean) {
                    main.post { fail("Valdi crash/ANR=$isANR $module: $message\n${stackTrace.orEmpty()}") }
                }
            }
            runtimeManager.registerGlobalAttributesBindersFromContext(applicationContext)
            SupportValdiRuntimeManager.configureSupportLibs(runtimeManager)
            val scoped = runtimeManager.mainRuntime.makeScoped(
                ValdiContextConfiguration(renderBackend = RenderBackend.ANDROID))
            val view = ValdiRootView(scoped.context).apply { id = R.id.render_benchmark_root }
            root = view
            column.addView(view, LinearLayout.LayoutParams(-1, 0, 1f))
            // Real compiler-generated context. nowMs is safe on the JS thread:
            // no UIKit/View access, main-thread hop, or blocking dispatch.
            val context = BenchmarkContext(
                report = { text ->
                    val receiptMs = System.nanoTime().toDouble() / 1_000_000.0
                    val receiptCpuMs = Process.getElapsedCpuTime()
                    main.post { receive(text, receiptMs, receiptCpuMs) }
                },
                nowMs = { System.nanoTime().toDouble() / 1_000_000.0 })
            scoped.inflateViewAsync(view, "BenchmarkApp@render_benchmark/src/Benchmark",
                null, context, null, null)
        } catch (error: Throwable) {
            fail("Native startup: ${error.stackTraceToString()}")
        }
    }

    private fun receive(text: String, receiptMs: Double, receiptCpuMs: Long) {
        if (terminal) return
        try {
            val event = JSONObject(text)
            require(event.getInt("schema") == 1) { "Unknown event schema" }
            // Preserve the complete JS payload, with a separate native receipt.
            val received = JSONObject().put("event", event).put("nativeReceipt",
                JSONObject().put("monotonicMs", receiptMs).put("processCpuMs", receiptCpuMs))
            events.put(received)
            when (event.getString("kind")) {
                "started" -> {
                    require(startedAtMs == null) { "Duplicate started" }
                    require(readIds(event.getJSONArray("suiteIds")) == suiteIds) { "Unexpected benchmark suite" }
                    startedAtMs = receiptMs
                    startedCpuMs = receiptCpuMs
                    checkProvenance(event.getJSONObject("provenance"))
                }
                "scene" -> {
                    require(startedAtMs != null) { "Scene before started" }
                    val result = event.getJSONObject("result")
                    val index = sceneIds.size
                    require(index < suiteIds.size && result.getString("id") == suiteIds[index]
                        && event.getInt("index") == index) { "Missing/reordered/duplicate scene" }
                    sceneIds.add(result.getString("id"))
                    if (event.getString("status") != "PASS" || !result.getBoolean("completed")
                        || result.getString("status") != "PASS") sceneFailed = true
                    // Preserve failed scenes and the module's complete final report.
                }
                "finished" -> {
                    require(startedAtMs != null) { "Finished before started" }
                    val elapsed = receiptMs - startedAtMs!!
                    val cpu = receiptCpuMs - startedCpuMs!!
                    require(elapsed > 0 && cpu >= 0) { "Invalid native timing samples" }
                    cpuProxy = JSONObject()
                        .put("method", "android.os.Process.getElapsedCpuTime")
                        .put("scope", "Whole process, native started/finished callback entry")
                        .put("startedProcessCpuMs", startedCpuMs)
                        .put("finishedProcessCpuMs", receiptCpuMs)
                        .put("processCpuMs", cpu).put("elapsedMonotonicMs", elapsed)
                        .put("processCpuMsPerWallMs", cpu / elapsed)
                        .put("interpretation", "CPU activity proxy; ratio may exceed 1; not watts or energy")
                    require(event.getString("status") == "PASS" && event.getBoolean("completed")
                        && sceneIds == suiteIds && readIds(event.getJSONArray("suiteIds")) == suiteIds
                        && !sceneFailed) { "Incomplete/failed benchmark" }
                    checkProvenance(event.getJSONObject("provenance"))
                    val engine = event.getJSONObject("engine")
                    require(engine.getBoolean("verified") && engine.getString("name") == "Hermes") {
                        "Hermes not verified"
                    }
                    val scenes = event.getJSONArray("scenes")
                    require(scenes.length() == suiteIds.size) { "Incomplete final scene payload" }
                    for (i in suiteIds.indices) {
                        val scene = scenes.getJSONObject(i)
                        require(scene.getString("id") == suiteIds[i] && scene.getBoolean("completed")
                            && scene.getString("status") == "PASS") { "Final scene failed" }
                    }
                    terminal = true
                    main.removeCallbacks(watchdog)
                    persist("PASS")
                    return
                }
                else -> error("Unknown event kind")
            }
            persist("RUNNING")
        } catch (error: Throwable) {
            fail("Report rejected: ${error.stackTraceToString()}\n$text")
        }
    }

    private fun readIds(array: JSONArray): List<String> =
        (0 until array.length()).map { array.getString(it) }

    private fun checkProvenance(declared: JSONObject) {
        // The shared module is intentionally backend-unbound. Its source/input
        // provenance is separate from the externally supplied runtime artifact.
        require(declared.getString("module") == "render_benchmark"
            && declared.getString("compilation") == "release"
            && declared.getString("runtimeBinding") == "unbound") { "Unexpected module provenance" }
        for (key in listOf("backend", "runtimeSha256", "config")) {
            if (declared.has(key) && !declared.isNull(key))
                require(declared.get(key) == provenance.get(key)) { "JS/native $key mismatch" }
        }
    }

    private fun atomic(file: File, text: String) {
        file.parentFile?.mkdirs()
        val target = AtomicFile(file)
        val stream = target.startWrite()
        try {
            stream.write(text.toByteArray(Charsets.UTF_8))
            target.finishWrite(stream)
        } catch (error: Throwable) {
            target.failWrite(stream)
            throw error
        }
    }

    private fun persist(value: String) {
        val record = JSONObject().put("schema", 1).put("backend", backend)
            .put("runId", directory?.name ?: "unavailable")
            .put("status", value).put("completed", terminal).put("sceneCount", sceneIds.size)
            .put("startupSettleMs", 8_000)
            .put("rootMode", "native Android")
            .put("measurement", "Managed render + synchronous readback throughput/GPU frame latency; not onscreen FPS")
            .put("provenance", provenance).put("events", events)
            .put("cpuProxy", cpuProxy ?: JSONObject.NULL)
            .put("directory", directory?.absolutePath ?: "unavailable")
            .put("exportDirectory", exportDirectory?.absolutePath ?: "unavailable")
            .put("updatedUnixMs", System.currentTimeMillis())
        val line = "$backend $value"
        try {
            val output = directory ?: error("No report directory")
            val json = record.toString(2)
            atomic(File(output, "results.json"), json)
            // done is the commit marker: results.json must have succeeded first.
            if (terminal) {
                val exported = exportDirectory ?: error("No benchmark export directory")
                atomic(File(exported, "results.json"), json)
                atomic(File(exported.parentFile, "latest.txt"), exported.absolutePath)
                atomic(File(exported, "done"), "$line\n")
                atomic(File(output, "done"), "$line\n")
            }
            showStatus("$line scenes=${sceneIds.size}")
        } catch (error: Throwable) {
            terminal = true
            main.removeCallbacks(watchdog)
            events.put(JSONObject().put("nativeFailure", JSONObject()
                .put("message", "Report persistence/export: ${error.stackTraceToString()}")))
            record.put("status", "FAIL").put("completed", true)
                .put("persistenceError", error.stackTraceToString())
            // Remove every possible PASS marker before the bounded FAIL writes.
            for (output in listOfNotNull(directory, exportDirectory)) File(output, "done").delete()
            for (output in listOfNotNull(directory, exportDirectory)) {
                try {
                    atomic(File(output, "results.json"), record.toString(2))
                    if (output == exportDirectory)
                        atomic(File(output.parentFile, "latest.txt"), output.absolutePath)
                    atomic(File(output, "done"), "$backend FAIL\n")
                } catch (_ignored: Throwable) { /* UI cannot claim persisted success. */ }
            }
            showStatus("$backend FAIL report persistence/export")
        }
    }

    private fun showStatus(line: String) {
        status.text = "$line\nPrivate: ${directory?.absolutePath ?: "unavailable"}" +
            "\nExport: ${exportDirectory?.absolutePath ?: "unavailable"}\nResult: results.json; terminal: done"
    }

    private fun fail(message: String) {
        if (terminal) return
        terminal = true
        main.removeCallbacks(watchdog)
        main.removeCallbacks(delayedStartup)
        events.put(JSONObject().put("nativeFailure", JSONObject().put("message", message)
            .put("monotonicMs", System.nanoTime().toDouble() / 1_000_000.0)))
        persist("FAIL")
    }

    override fun onDestroy() {
        destroyed = true
        main.removeCallbacks(delayedStartup)
        main.removeCallbacks(watchdog)
        if (!terminal) fail("Activity destroyed before benchmark finished")
        root?.dispose()
        manager?.destroy()
        super.onDestroy()
    }
}
