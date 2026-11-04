package io.valdiroutes.tgfxapp

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.AtomicFile
import android.util.Base64
import android.view.PixelCopy
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.snap.valdi.ExceptionReporter
import com.snap.valdi.RenderBackend
import com.snap.valdi.ValdiContextConfiguration
import com.snap.valdi.ValdiRuntimeManager
import com.snap.valdi.ValdiTweaks
import com.snap.valdi.bundle.IValdiCustomModuleProvider
import com.snap.valdi.exceptions.ValdiException
import com.snap.valdi.makeScoped
import com.snap.valdi.modules.ModuleFactoryRegistry
import com.snap.valdi.snapdrawing.SnapDrawingOptions
import com.snap.valdi.snapdrawing.SnapDrawingRenderMode
import com.snap.valdi.support.SupportValdiRuntimeManager
import com.snap.valdi.views.ValdiRootView
import com.snap.modules.tgfx_acceptance.AcceptanceContext
import com.snapchat.client.valdi_core.JavaScriptEngineType
import java.io.File
import java.io.ByteArrayOutputStream
import java.util.UUID
import org.json.JSONArray
import org.json.JSONObject

/**
 * Real Valdi host. No JNI definitions or native test library. Both modes execute
 * the same managed-context cases; snapDrawingRoot additionally checks pixels
 * copied from the real Android presentation SurfaceView.
 */
open class AcceptanceActivity : Activity() {
    protected open fun useSnapDrawingRoot(): Boolean =
        intent.getBooleanExtra("snapDrawingRoot", false)
    private val handler = Handler(Looper.getMainLooper())
    private var manager: ValdiRuntimeManager? = null
    private var root: ValdiRootView? = null
    private lateinit var status: TextView
    private lateinit var results: TextView
    private lateinit var directory: File
    private val events = JSONArray()
    private val captured = linkedSetOf<String>()
    private var failed = false
    private var finished = false
    private var started = false
    private var rootMode = "native"
    private var provenance = JSONObject()
    private val caseIds = listOf("composition", "shape", "label-iiii", "label-mmmm",
        "bitmap-bytes", "bitmap-catalog", "svg", "lottie-t0", "lottie-t1")
    private val watchdog = Runnable { fail("Host watchdog: no completed JS acceptance within 180 seconds") }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        status = TextView(this).apply {
            id = R.id.tgfx_app_status
            contentDescription = "tgfx-app-status"
            text = "STARTING"
            textSize = 20f
        }
        results = TextView(this).apply {
            id = R.id.tgfx_app_results
            contentDescription = "tgfx-app-results"
            setTextIsSelectable(true)
        }
        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        column.addView(status)
        column.addView(ScrollView(this).apply { addView(results) }, LinearLayout.LayoutParams(-1, 0, 1f))
        setContentView(column)
        try {
            directory = File(filesDir, "tgfx-app/${UUID.randomUUID()}").apply { mkdirs() }
            atomic(File(filesDir, "tgfx-app/latest.txt"), directory.absolutePath.toByteArray())
            provenance = JSONObject(assets.open("provenance.json").bufferedReader().use { it.readText() })
            require(provenance.getInt("schema") == 2 && provenance.getString("kind") == "tgfx-valdi-runtime-export"
                && provenance.getString("runtimeBinding") == "exported" && !provenance.getBoolean("nativeAcceptance")
                && provenance.getString("backend") == "tgfx" && provenance.getString("engine") == "hermes"
                && provenance.getString("profile") == "android_arm64")
            rootMode = if (useSnapDrawingRoot()) "snapdrawing" else "native"
            writeState("STARTING")
            if (finished) return
            handler.postDelayed(watchdog, 180000)
            System.loadLibrary("valdi_runtime")
            ModuleFactoryRegistry.registerModuleFactoriesFromContext(applicationContext)
            val provider = object : IValdiCustomModuleProvider {
                override fun getCustomModuleData(path: String): ByteArray? {
                    if (path !in setOf("tgfx_acceptance.valdimodule", "tgfx_acceptance.map.json",
                            "drawing.valdimodule", "drawing.map.json")) return null
                    // Let the real provider contract convert read failures into a
                    // Valdi error; host watchdog remains a fail-closed fallback.
                    return try { assets.open("modules/$path").use { it.readBytes() } }
                    catch (error: Exception) {
                        runOnUiThread { fail("Module $path: ${error.message}") }
                        throw ValdiException("Cannot load acceptance module $path")
                    }
                }
            }
            val runtimeManager = ValdiRuntimeManager(applicationContext,
                tweaks = ValdiTweaks(javaScriptEngineType = JavaScriptEngineType.HERMES),
                customModuleProvider = provider)
            manager = runtimeManager
            runtimeManager.exceptionReporter = object : ExceptionReporter {
                override fun reportNonFatal(errorCode: Int, message: String, module: String?, stackTrace: String?) {
                    runOnUiThread { fail("Valdi $errorCode $module: $message\n${stackTrace.orEmpty()}") }
                }
                override fun reportCrash(message: String, module: String?, stackTrace: String?, isANR: Boolean) {
                    runOnUiThread { fail("Valdi crash/ANR=$isANR $module: $message\n${stackTrace.orEmpty()}") }
                }
            }
            runtimeManager.registerGlobalAttributesBindersFromContext(applicationContext)
            SupportValdiRuntimeManager.configureSupportLibs(runtimeManager)
            val scoped = runtimeManager.mainRuntime.makeScoped(ValdiContextConfiguration(
                renderBackend = if (rootMode == "snapdrawing") RenderBackend.SNAP_DRAWING else RenderBackend.ANDROID,
                snapDrawingOptions = SnapDrawingOptions(enableSynchronousDraw = true,
                    renderMode = SnapDrawingRenderMode.SURFACE_VIEW)))
            val view = ValdiRootView(scoped.context).apply { id = R.id.tgfx_app_preview }
            root = view
            column.addView(view, 1, LinearLayout.LayoutParams(-1, 0, 1f))
            // Actual node-diagnostics-01 emitted constructor(report: (event: String) -> Unit).
            val context = AcceptanceContext(report = { event -> runOnUiThread { receive(event) } })
            // Generated @Context model, not a handwritten native bridge.
            scoped.inflateViewAsync(view, "AcceptanceApp@tgfx_acceptance/src/Acceptance",
                null, context, null, null)
        } catch (error: Throwable) {
            fail("Native startup: ${error.stackTraceToString()}")
        }
    }

    private fun atomic(file: File, bytes: ByteArray) {
        file.parentFile?.mkdirs()
        val atomic = AtomicFile(file)
        val output = atomic.startWrite()
        try { output.write(bytes); atomic.finishWrite(output) }
        catch (error: Throwable) { atomic.failWrite(output); throw error }
    }

    private fun receive(text: String) {
        if (finished) return
        try {
            val event = JSONObject(text)
            require(event.getInt("schema") == 1) { "Unknown report schema" }
            when (event.getString("kind")) {
                "started" -> {
                    require(!started) { "Duplicate started event" }
                    started = true
                    require(event.getJSONObject("engine").getBoolean("verified")
                        && event.getJSONObject("engine").getString("name") == "Hermes") { "Hermes was not verified" }
                    val jsInput = event.getJSONObject("provenance")
                    require(listOf("runtimeSha256", "sourceStateSha256", "sourceRoot").all {
                        jsInput.getString(it) == provenance.getString(it)
                    }) { "JS/native provenance mismatch" }
                }
                "case" -> {
                    require(started) { "Case before engine verification" }
                    val result = event.getJSONObject("result")
                    val id = result.getString("id")
                    require(id in caseIds && captured.add(id)) { "Unknown/duplicate case $id" }
                    require(result.getJSONArray("assertions").length() > 0) { "No content assertions" }
                    val png = Base64.decode(event.getString("pngBase64"), Base64.DEFAULT)
                    val decoded = BitmapFactory.decodeByteArray(png, 0, png.size)
                        ?: error("PNG cannot be decoded")
                    try { require(decoded.width == 160 && decoded.height == 120) { "Wrong snapshot dimensions" } }
                    finally { decoded.recycle() }
                    atomic(File(directory, "$id.png"), png)
                    event.remove("pngBase64")
                    event.put("pngFile", "$id.png")
                    if (!result.getBoolean("passed")) failed = true
                }
                "finished" -> {
                    val cases = event.getJSONArray("cases")
                    require(started && captured.toList() == caseIds && cases.length() == caseIds.size) { "Incomplete case set" }
                    for (i in caseIds.indices) {
                        val item = cases.getJSONObject(i)
                        require(item.getString("id") == caseIds[i] && item.getBoolean("passed")) { "Failed/reordered case" }
                        require(item.getJSONObject("assets").getJSONArray("errors").length() == 0) { "Asset errors" }
                        val assertions = item.getJSONArray("assertions")
                        require(assertions.length() > 0) { "Missing assertions" }
                        for (j in 0 until assertions.length()) require(assertions.getJSONObject(j).getBoolean("passed"))
                    }
                    require(event.getString("status") == "PASS" && !failed) { "JS/native failure" }
                    events.put(event)
                    if (rootMode == "snapdrawing") {
                        writeState("VERIFYING PRESENTATION")
                        root?.postOnAnimation {
                            if (!finished) {
                                try { verifyPresentation() }
                                catch (error: Throwable) { fail("Root presentation: ${error.stackTraceToString()}") }
                            }
                        } ?: error("Root view is unavailable")
                    } else finishAcceptance()
                    return
                }
                else -> error("Unknown event kind")
            }
            events.put(event)
            writeState(if (failed) "FAIL" else "RUNNING")
        } catch (error: Throwable) { fail("Report rejected: ${error.stackTraceToString()}\n$text") }
    }

    private fun finishAcceptance() {
        finished = true
        handler.removeCallbacks(watchdog)
        writeState("PASS")
    }

    private fun presentationSurface(view: View): SurfaceView? {
        if (view is SurfaceView) return view
        if (view is ViewGroup) for (i in 0 until view.childCount) {
            presentationSurface(view.getChildAt(i))?.let { return it }
        }
        return null
    }

    private fun verifyPresentation() {
        val surface = presentationSurface(root ?: error("Root view is unavailable"))
            ?: error("SnapDrawing has no presentation SurfaceView")
        val width = surface.width
        val height = surface.height
        require(width > 0 && height > 0 && width.toLong() * height <= 16 * 1024 * 1024) {
            "Invalid presentation dimensions ${width}x$height"
        }
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        try {
            PixelCopy.request(surface, bitmap, { result ->
                try {
                    if (!finished) {
                        require(result == PixelCopy.SUCCESS) { "PixelCopy failed: $result" }
                        val pixels = IntArray(width * height)
                        bitmap.getPixels(pixels, 0, width, 0, 0, width, height)
                        var white = 0
                        var blue = 0
                        for (color in pixels) {
                            if (Color.alpha(color) < 240) continue
                            if (Color.red(color) >= 240 && Color.green(color) >= 240 &&
                                Color.blue(color) >= 240) white++
                            if (Color.red(color) < 40 && Color.green(color) < 160 &&
                                Color.blue(color) >= 220) blue++
                        }
                        require(white > pixels.size / 4 && blue > 100) {
                            "Presented content missing: white=$white blue=$blue total=${pixels.size}"
                        }
                        val png = ByteArrayOutputStream().use {
                            require(bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)) { "Presentation PNG failed" }
                            it.toByteArray()
                        }
                        atomic(File(directory, "root-presentation.png"), png)
                        events.put(JSONObject().put("kind", "presentation").put("passed", true)
                            .put("method", "Android PixelCopy SurfaceView").put("width", width)
                            .put("height", height).put("whitePixels", white).put("bluePixels", blue)
                            .put("pngFile", "root-presentation.png")
                            .put("scope", "Presented white background and blue preview control; nine exact scenes use managed readback"))
                        finishAcceptance()
                    }
                } catch (error: Throwable) {
                    fail("Root presentation: ${error.stackTraceToString()}")
                } finally {
                    bitmap.recycle()
                }
            }, handler)
        } catch (error: Throwable) {
            bitmap.recycle()
            throw error
        }
    }

    private fun writeState(value: String) {
        val record = JSONObject().put("schema", 1).put("status", value)
            .put("completed", finished).put("rootMode", rootMode)
            .put("gate", if (rootMode == "snapdrawing")
                "Hermes managed TGFX scenes plus presented SurfaceView content control"
                else "Hermes application + managed TGFX output; native Android root")
            .put("provenance", provenance)
            .put("events", events).put("completedUnixMs", System.currentTimeMillis())
            .put("directory", if (::directory.isInitialized) directory.absolutePath else "unavailable")
        val text = record.toString(2)
        try {
            if (::directory.isInitialized) {
                atomic(File(directory, "results.json"), text.toByteArray())
                if (finished) atomic(File(directory, "done"), value.toByteArray())
            } else error("App report directory unavailable")
            status.text = "$value ${captured.size}/${caseIds.size} root=$rootMode"
            results.text = text
        } catch (error: Throwable) {
            failed = true
            finished = true
            handler.removeCallbacks(watchdog)
            var savedFailure = false
            record.put("status", "FAIL").put("completed", true)
                .put("persistenceError", error.stackTraceToString())
            if (::directory.isInitialized) {
                File(directory, "done").delete()
                // One bounded persistence attempt for the explicit failure.
                try {
                    atomic(File(directory, "results.json"), record.toString(2).toByteArray())
                    atomic(File(directory, "done"), "FAIL".toByteArray())
                    savedFailure = true
                } catch (_ignored: Throwable) { /* UI remains FAIL, no PASS marker is accepted. */ }
            }
            status.text = "FAIL report persistence"
            results.text = "${record.toString(2)}\n${if (savedFailure) "Failure report saved" else "REPORT NOT SAVED"}"
        }
    }

    private fun fail(message: String) {
        failed = true
        finished = true
        handler.removeCallbacks(watchdog)
        events.put(JSONObject().put("kind", "nativeFailure").put("message", message))
        writeState("FAIL")
    }

    override fun onDestroy() {
        handler.removeCallbacks(watchdog)
        if (!finished) fail("Activity destroyed before acceptance completed")
        root?.dispose()
        manager?.destroy()
        super.onDestroy()
    }
}

class SnapDrawingAcceptanceActivity : AcceptanceActivity() {
    override fun useSnapDrawingRoot(): Boolean = true
}
