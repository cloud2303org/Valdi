# TGFX graphics backend integration

This implementation is selected by `SNAP_DRAWING_TGFX_EXPERIMENT`. The default branch remains Skia.
The APIs were inspected in the local TGFX checkout at `4cf8991d`. No compiler, configure, build,
test, device, or network command was run for this implementation.

## Source contracts

- `DrawableSurfaceCanvas(NativeSurface*, int, int)` and its legacy `getSkiaCanvas()` /
  `getSkiaSurface()` accessors now use the real backend aliases. `getNativeCanvas()` and
  `getNativeSurface()` expose the same borrowed objects. They are valid on the preparing thread
  until `DrawableSurface::flush()`. TGFX snapshots synchronously read back into independently
  owned immutable TGFX image pixels, so snapshots can escape the frame/device lifetime.
  Reading a directly supplied TGFX window surface also presents its recorded frame, as specified
  by TGFX's readPixels contract. Production EGL and Metal drawables always stage user drawing on
  an offscreen target, so their snapshot/readback does not present an unvalidated native frame.
- In a TGFX build, `GrGraphicsContext(std::shared_ptr<tgfx::Device>, options)` owns real device
  storage. `lockContext()` returns `std::unique_ptr<TGFXContextLock>`; check `*lock` before using
  `lock->get()`. `getGrContext()` returns `tgfx::Context&` and requires an active lock for this
  device on this thread. A prepared drawable keeps that lock through drawing and flush.
  Nested device operations must finish in stack order. All consumers of a device shared with
  this implementation should acquire it through `GrGraphicsContext::lockContext()`, so recursive
  acquisitions share one underlying non-recursive TGFX lock.
- Composition may use `tgfx::Surface::Make(&graphicsContext.getGrContext(), ...)` during a prepared
  frame, or acquire `lockContext()` explicitly. Stored GPU surfaces must retain their owning
  graphics context/device and release GPU objects under a lock for that device.
- `GLGraphicsContext::create(options)` requires a current host GL context. It creates an owned
  TGFX GL/EGL context in the host's texture share group, with its own pbuffer. Locking restores
  the prior native context/surfaces on release. This avoids host GL state changes invalidating
  TGFX's private state cache; the pin has no public GL state-reset API. The explicit constructor
  requires a device dedicated to TGFX. Native GL texture IDs, targets and formats use `unsigned`.
  `GLDrawableSurface` takes a real `tgfx::BackendTexture`; owned allocations retain a real
  `tgfx::Texture`, while borrowed textures require the caller to retain their native allocation.
  Host writes must complete before TGFX drawing; flush waits for TGFX writes before host reuse.
- Android display surfaces use real `EGLWindow` devices, connected lazily in `prepareCanvas()`
  after the presenter releases its previous drawable. Repeated SurfaceView creation/change
  callbacks therefore do not connect two EGL surfaces to the same native window.
  Each window owns its EGL context; `getGraphicsContext()` returns the retaining manager.
  The manager tracks live
  devices for cache changes/cleanup. It acquires/releases `ANativeWindow` references, recreates
  TGFX surfaces after resize, preserves vsync selection, and resolves offscreen drawing (with
  optional MSAA) before TGFX presents. TGFX texture-backed images cannot be reused across device IDs; use the
  portable `DrawableSurfaceCanvas::snapshot()` when content crosses windows/devices.
- `BitmapGraphicsContext(const Ref<GrGraphicsContext>&)` optionally reuses an existing GPU device.
  The default constructor creates a real offscreen device lazily. Bitmap surfaces retain the
  bitmap, lock its bytes, upload an owned copy of existing content for damage-only rendering,
  draw on a GPU surface, synchronously read back into owned staging pixels with the caller's
  alpha type/row stride, and copy only validated visible row bytes before unlocking at flush.
  Native failure or readback failure leaves caller bytes/padding unchanged.
  The raw-pointer overload requires caller memory through flush;
  the info-only overload owns its backing bytes. Gray output uses an RGBA render target and
  converts on readback. RGBA, BGRA, alpha and gray 8-bit formats are supported subject to GPU
  renderability. Initial device/format/upload failures are returned by `prepareCanvas()`.
- `MetalGraphicsContext::createSurfaceWrappingTexture(CFTypeRef texture, int sampleCount = 1)`
  accepts and retains an actual `id<MTLTexture>` with RenderTarget and ShaderRead usage on the
  same MTLDevice. External writes must complete before drawing; its flush waits for TGFX writes.
  Layer surfaces retain the CAMetalLayer, use a real `MetalWindow`, recreate on resize, and
  render via a sampleable offscreen BGRA surface. This preserves snapshots for framebuffer-only
  layers and implements MSAA before compositing to/presenting the actual drawable texture.
- The existing `MetalGraphicsContext.mm` supplies the Metal factories and native texture bridge.
  `makeTGFXOffscreenMetalDevice()` is defined there and used by `GrGraphicsContext::makeOffscreen()`.
  TGFX's MetalDevice owns its command queue. The supplied host queue is retained for the legacy
  getter but is not used to encode TGFX work; presentation/submission use TGFX's own queue.
- Implementation helper headers are included by the existing `.cpp` files. They are covered by
  the existing `**/*.hpp` header glob and do not introduce new source translation units.
  Apple uses TGFX Metal; non-Apple builds use TGFX OpenGL/EGL.

## Pinned API limits and parent-owned integration

- The public TGFX Device/Context/GPU APIs have no persistent shader cache load/store hook.
  `GrGraphicsContextOptions::supportsPersistentShaderCache()` returns false. The provided host
  `IShaderCache` is retained, but it is not passed to a fabricated TGFX cache API. TGFX continues
  to own its in-memory shader/resource caches. Retention and `getShaderCache()` expose host storage
  only: the TGFX backend never invokes `IShaderCache::load()` or `store()`.
- The non-Ganesh cache interface uses `NativeData load(const NativeDataObject& key)` and
  `void store(const NativeDataObject& key, const NativeDataObject& data)`. In a TGFX build these
  are actual `std::shared_ptr<tgfx::Data>` and `tgfx::Data` references, with no SkData include.
  The non-TGFX Ganesh branch still inherits the real `GrContextOptions::PersistentCache` unchanged.
  The parent owns adapting `valdi/src/valdi/snap_drawing/Graphics/ShaderCache` to these aliases.
  This source interface alignment does not add persistent shader caching support to TGFX.
- TGFX's GPU PixelFormat contains no floating-point texture format. Although ColorType declares
  RGBA_F16, `ColorTypeToPixelFormat` maps it to Unknown and Surface::Make cannot render it.
  Bitmap preparation explicitly returns an error for F16/F32; native GL allocation returns null
  for these formats. No silent conversion to 8-bit is performed.
- TGFX exposes purgeable resources but no scratch-only category. Scratch purge requests map to
  purging all currently purgeable resources; age-based cleanup uses purgeResourcesNotUsedSince.
- TGFX DrawableSurface adds the internal virtual `Valdi::Result<Valdi::Void> flushWithResult()`.
  Its base fallback calls the existing void flush and returns Void; every real TGFX drawable
  overrides it. The non-TGFX interface and function bodies remain unchanged. DrawOperation::drawNext
  propagates completion failure before notifying the presenter; delta and non-delta raster methods
  propagate it before returning successful pixels/counts. No TS or public application API changes.
- Bitmap, EGL, Metal and native texture result completion check actual Canvas::hasError state
  and release frame/bitmap locks on failure. The last failed completion result is retained until
  a new prepareCanvas attempt; repeating completion does not turn a failed frame into success.
  A new frame recreates a failed rendering surface, allowing supported content to recover.
- Legacy void flush wrappers log completion failure instead of aborting. Destructors never
  complete/publish unfinished work: they log known native failure and release resources/locks.
  No rendering-failure abort, global error boundary, or fabricated backend error flag is added.

## Checked native completion

These TGFX-only methods were added to the existing DrawableSurfaceCanvas seam:

```cpp
Valdi::Result<Valdi::Void> validate() const;
void discard() const;
Valdi::Result<Valdi::Void> flush(bool syncCpu = false) const;
Valdi::Result<NativeData> readPixels(const tgfx::ImageInfo& info) const;
Valdi::Result<NativeImage> makeImageSnapshot() const;
```

- `validate()` consumes the real destination Canvas::hasError() sticky state. On failure it
  closes/discards current unsubmitted work; it never submits failed work.
- `discard()` closes current deferred work and cancels only the Recording returned by that
  flush. Cleanup of abandoned bitmap/GL/Metal/EGL frames uses this path before releasing locks.
- `flush()` checks before Context::flush(), after deferred compositor materialization and before
  Context::submit(), and after submission. Post-flush rejection calls Context::discard() with
  that exact Recording before returning its error. Releasing the token alone is insufficient.
- `readPixels()` records Surface::asyncReadPixels(), checks native error state, then uses checked
  flush(true) before mapping the real SurfaceReadback. This prevents native synchronous readPixels
  from implicitly submitting rejected work. It checks error state after mapping and CPU conversion,
  converts through native Pixmap with the requested alpha/color space/stride, handles origin
  flipping, and returns only independently owned validated bytes.
- `makeImageSnapshot()` checks before and after native snapshot/copy materialization. Same-device
  presentation completes and validates pending source work again after taking that snapshot.
- `presentTGFXSurface(source, destination)` requires distinct equally sized surfaces on the same
  device. It completes the offscreen source before drawing the validated texture into the native
  destination, then checks the destination's flush/submit boundaries and source status again.
- `GrGraphicsContext::commit()` is now a no-op. All production drawables have already completed
  checked work at flush; a context-only commit cannot inspect destination canvas errors and must
  not bypass them. Direct-surface composition callers must use the checked seam.

The local TGFX patch adds `bool Context::discard(std::unique_ptr<Recording>)`. It requires the
locked owner thread and matches context ID, DrawingBuffer ID and buffer generation. It removes
only the matching queued batch; earlier/later batches and their ordering remain intact. Null,
foreign, submitted or discarded tokens return false. It consumes the token/windows, destroys task
and frame references, and frees all three allocators plus task vector storage even though the buffer
itself remains in DrawingManager's reuse pool. Cancellation advances the buffer generation.
`DrawingBuffer::discard()` is the new internal buffer operation. Dropped atlas uploads invalidate
the existing atlas residency lookup via AtlasManager::releaseAll(), so future text cannot mistake
unexecuted uploads for resident cells. Already queued tasks retain their own proxies. Text/paint
source and public Canvas error state are unchanged.

ROUND3 cancellation joins real asynchronous writers before releasing their destinations.
AsyncVertexSource first cancels every queued VertexProviderTask, then waits for all executing
providers before dropping its Data owner. AtlasUploadTask likewise cancels all queued cells and
joins every executing cell before clearing ownership or unlocking hardware pixels. Only afterwards
can DrawingBuffer release its vertex/drawing allocator storage. Task::cancel publishes the new
TaskStatus::Canceling while onCancel releases a queued payload; Task::wait joins that cleanup as
well as Executing work, and Canceled is published only after cleanup returns. No task mutex or
TaskPool scheduling lock is held while user/provider cancellation or worker joins run. Standard
vertex generation and cell decode are CPU work and do not require the caller's locked GPU device.

A native Recording is an atomic context-wide batch, not a per-surface transaction. Independent
work must use separate flush boundaries; work dependent on a canceled output must be re-recorded.
Cancellation preserves unrelated queued batches, but does not repair dependent batches or make a
canceled surface reusable. Production completion drops failed rendering surfaces before preparing
a new frame. Direct native callers must likewise abandon canceled outputs. No additional native
translation unit is introduced; rebuild the full TGFX archives and the framework consumers for
the added Context symbol and DrawingBuffer implementation.

A GPU error first observable only after submission cannot undo an already scheduled present;
it is returned as failure and the presenter is not marked updated. A device-loss/recreation policy
for that separate backend failure remains with its owner. No process abort is used to hide this limit.

The local TGFX patch also exposes `void Canvas::markRenderFailure()` for production adapters
that reject an asset before native geometry exists. It marks the receiving destination immediately
without changing saves. Every DrawContext starts with independent error status unless explicitly
sharing an active ancestor's state; active saveLayer contexts therefore mark their parent as well.
PictureContext finish freezes failure as a value-only Picture flag plus an immutable failure record,
including failure-only recordings. Opaque-fill discards preserve sticky status, referenced child
pictures carry it into the receiving recorder, and playback marks only its receiving context.
The direct-image optimization cannot bypass a failed picture. No Picture retains mutable
destination error state. Bounds measurement uses a separate MeasureContext and executes no deferred
callbacks. A new beginRecording resets only that recorder's status; prior finished pictures retain
their immutable failure. Public bool Canvas::hasError() const remains unchanged.

ROUND3 pictures also retain a value-only recursive deferred-drawing trait. A deferred callback is
not a single primitive: Canvas cannot merge group opacity into each draw it might emit, nor invoke
it while re-recording or measuring another picture. Such pictures retain their layer boundary and
skip direct-image/unroll optimizations. LayerUnrollContext defensively refuses deferred execution.
RenderContext renders deferred groups into a real exact-sized device-coordinate target with the
complete final matrix and an origin translation. The resulting texture is composited once with
the group brush. Nested measurement retains device outsets in device coordinates. Outer image
filters consume an already baked texture mapped back into local filter coordinates, so scalar
PictureImage sampling never re-executes a device callback with a substitute scalar transform.
This is generic DrawDeferred handling for TextLayer, geometry effects and ThorVG media; no
TextLayer scalar workaround, callback-kind dispatch or new public transform getter is introduced.

The new source-only regression fixture is `experimental/tgfx/full/tests/gpu_error_checks.{hpp,cpp}`,
with entry point `GPUErrorCheckReport runGPUErrorChecks()`. The parent must compile/link the new
source and call it from the native runner. Its twenty-nine registered cases cover supported snapshot,
existing native capture failure, failure first raised by lazy PictureImage materialization during
snapshot/readback/flush, rejection before destination presentation, supported presentation, and
successful bitmap publication with unchanged row padding, immediate/deferred production
flushWithResult errors with unchanged caller bytes, supported retry on the same drawable, and
destruction of a failed uncompleted bitmap without publication/abort. Seven cancellation additions
cover middle-batch removal with both neighbors preserved, stale submitted tokens after further
recording, immediate-error/abandoned-frame cleanup, and 32 repeated deferred failures at each of
flush/snapshot/readback/production-bitmap completion. Real RuntimeEffect callbacks copy native
textures on encode and witness task destruction through weak ownership. Each repeated case checks
immediate task release, no growing GPU memory after explicit purge, unchanged failed bitmap bytes,
preserved earlier queued work, and a later successful production submit that executes no rejected
probe. The original presentation rejection assertions remain; its destination blue baseline is
completed in a separate batch before recording the failed source, respecting native batch atomicity.
Two explicit-marker cases additionally cover immediate public marking, opaque-fill retention,
nested picture recording, recorder destruction/reuse, isolated bounds measurement and destinations,
transparent active layer ancestors, failure-only layer replay, and production snapshot/flush rejection.
Six ROUND3 worker cases cover queued/executing vertex and CPU atlas work, executing hardware atlas
work, and a waiter racing queued cancellation cleanup. They use actual native Task workers,
VertexProviderTask, AsyncVertexSource, GPUBufferUploadTask, AtlasUploadTask and Context batch
cancellation. The generic native upload factory exposes the real resource proxy for the vertex
source ownership witness; the canceled batch is never GPU-encoded. An executing writer stays gated
while queued payload destruction witnesses cancellation. A real allocator safety lease prevents a
missing-join negative mutation from freeing writer storage; the fixture checks real upload-owner
retention before releasing the writer and drains workers before releasing that lease. Hardware
storage is relocked and its actual written bytes checked after completion. All waits are bounded;
missing native worker/hardware prerequisites fail rather than skip.
Three deferred-group cases check overlap alpha .5 rather than .75, zero execution during intermediate
recording/measurement, final nonuniform matrix/clip and immutable picture reuse, plus filtered-group
replay without scalar-transform substitution. Existing twenty case bodies/assertions remain intact.
The async fixture target must expose actual TGFX internal headers and their concurrentqueue
dependency: add the vendor src and third_party/concurrentqueue include/header inputs under the
parent's build ownership, using the same native ABI/profile definitions. No new translation unit.
The separately owned effects fixture remains frozen at 97 cases. Its actual Paint recording-only
100001-stamp canary checks hasError and balanced saves before each adversarial negative input;
this native seam supplies that dependency without editing Paint or the effects fixture.
The separately owned ROUND3 text and media sources now register 32 and 78 cases respectively.
Their production paths use the same generic native deferred mechanism; these counts are source
handoff information, not execution results. Text's final-matrix/clip contract is documented in
full/tests/text-data/COMPOSITION_SHADOW_ROUND3.md.
Negative cases use real public
saveBehind rejection of a nonfinite capture subset, balanced saves, real PictureRecorder/Image/
Surface operations, and production Result methods. Missing GPU support is FAIL. The fixture has
not been compiled or executed by this agent; no runtime result is claimed.

The parent owns whole-module compilation and runtime validation. Source inspection and diff hygiene
are the only validation performed here.
