import { StatefulComponent } from 'valdi_core/src/Component';
import { onIdleInterruptible } from 'valdi_core/src/utils/OnIdle';
import { createManagedContext } from 'drawing/src/ManagedContextFactory';
import { createBitmap } from 'drawing/src/BitmapFactory';
import { BitmapAlphaType, BitmapColorType, IBitmap } from 'drawing/src/IBitmap';
import { IManagedContext } from 'drawing/src/IManagedContext';
import { PROVENANCE } from './Fixtures';
import {
  ANIMATION_DELTA_MS, AssetEvidence, HEIGHT, MEASURED_COUNT, renderScene, SceneId,
  SUITE_IDS, WIDTH, WARMUP_COUNT,
} from './Scenes';
import {
  Canary, emptyPhases, inspectBitmap, METRIC, PhaseTimings, SCOPE, summarize,
  TimingSummary, validPhases, validTimings,
} from './Results';

/**
 * @ViewModel
 * @ExportModel
 */
export interface BenchmarkViewModel {}

/**
 * Host supplies a callable monotonic native clock in milliseconds, on the JS
 * calling thread. It must not synchronously dispatch onto the UI thread.
 * @Context
 * @ExportModel
 */
export interface BenchmarkContext {
  report: (event: string) => void;
  nowMs: () => number;
}

declare const globalThis: {
  HermesInternal?: { getRuntimeProperties?: () => { [key: string]: unknown } };
};

interface SceneResult {
  id: SceneId;
  completed: boolean;
  status: 'PASS' | 'FAIL';
  metric: string;
  scope: string;
  workload: string;
  width: number;
  height: number;
  warmupCount: number;
  measuredCount: number;
  warmupDurationsMs: number[];
  durationsMs: number[];
  warmupPhaseDurationsMs: PhaseTimings;
  phaseDurationsMs: PhaseTimings;
  preparation: {
    renderMs?: number;
    initialLayoutMs?: number;
    preloadMs?: number;
    settledLayoutMs?: number;
  };
  summary?: TimingSummary;
  first?: Canary;
  last?: Canary;
  outputRelationshipPassed: boolean;
  assets: AssetEvidence;
  error?: string;
}

const PLAN = {
  metric: METRIC,
  scope: SCOPE,
  clock: 'host-native-monotonic-ms',
  timedSection: 'processFrame -> await draw -> rasterInto(bitmap,true)',
  disposal: 'each frame, after timer stops',
  execution: 'sequential unpaced async managed draw; full bitmap readback; delta rasterization disabled',
  width: WIDTH, height: HEIGHT, warmupCount: WARMUP_COUNT, measuredCount: MEASURED_COUNT,
  animationDeltaMs: ANIMATION_DELTA_MS,
  renderAndLayout: 'setup only, excluded from per-iteration total; native Lottie animation advances in processFrame',
};

function errorText(error: unknown): string {
  return error instanceof Error ? error.stack || error.message : String(error);
}

/** Setup-only watchdog. Never races a draw/frame allocation against a timeout. */
async function boundedSetup<T>(operation: Promise<T>, label: string): Promise<T> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([operation, new Promise<T>((_resolve, reject) => {
      timer = setTimeout(() => reject(new Error(`${label}: 30000ms timeout`)), 30000);
    })]);
  } finally {
    if (timer !== undefined) clearTimeout(timer);
  }
}

const idle = () => boundedSetup(new Promise<void>(resolve => onIdleInterruptible(resolve)), 'idle');

/**
 * @Component
 * @ExportModel
 */
export class BenchmarkApp extends StatefulComponent<BenchmarkViewModel, { status: string }, BenchmarkContext> {
  state = { status: 'WAITING for layout' };
  private started = false;
  private destroyed = false;
  private active?: IManagedContext;
  private lastNow = -Infinity;

  onDestroy(): void {
    this.destroyed = true;
    this.active?.dispose();
    this.active = undefined;
  }

  onRender(): void {
    <view width="100%" height="100%" backgroundColor="white" padding={12}>
      <label font="system-bold 16" color="black" numberOfLines={0} value={this.state.status} />
    </view>;
    if (!this.started) {
      this.started = true;
      this.renderer.onLayoutComplete(() => { if (!this.destroyed) void this.run(); });
    }
  }

  private alive(): void {
    if (this.destroyed) throw new Error('Benchmark component destroyed');
  }

  private now(): number {
    const value = this.context.nowMs();
    if (!Number.isFinite(value) || value < this.lastNow) throw new Error('Host nowMs is not a finite monotonic clock');
    this.lastNow = value;
    return value;
  }

  private emit(event: object): void {
    this.alive();
    this.context.report(JSON.stringify({ schema: 1, metric: METRIC, scope: SCOPE, ...event }));
  }

  private async scene(id: SceneId): Promise<SceneResult> {
    this.alive();
    const assets: AssetEvidence = { tracked: 0, loadedCallbacks: 0, errors: [] };
    const result: SceneResult = {
      id, completed: false, status: 'FAIL', metric: METRIC, scope: SCOPE,
      workload: id === 'lottie' ? 'cached tree with native animation updates' : 'cached static tree',
      width: WIDTH, height: HEIGHT,
      warmupCount: WARMUP_COUNT, measuredCount: MEASURED_COUNT,
      warmupDurationsMs: [], durationsMs: [], warmupPhaseDurationsMs: emptyPhases(),
      phaseDurationsMs: emptyPhases(), preparation: {}, outputRelationshipPassed: false, assets,
    };
    const managed = createManagedContext({ useAsyncPath: true, deltaRasterization: false });
    this.active = managed;
    let bitmap: IBitmap | undefined;
    try {
      const beforeRender = this.now();
      renderScene(managed, id, assets);
      const afterRender = this.now();
      result.preparation.renderMs = afterRender - beforeRender;
      await boundedSetup(managed.layout(WIDTH, HEIGHT, false), `${id} layout`);
      const afterLayout = this.now();
      result.preparation.initialLayoutMs = afterLayout - afterRender;
      await idle();
      this.alive();
      const loaded = await boundedSetup(managed.onAllAssetsLoaded(), `${id} assets`);
      assets.tracked = loaded.loadedAssetsCount;
      assets.errors.push(...(loaded.errors ?? []));
      await idle();
      this.alive();
      const afterPreload = this.now();
      result.preparation.preloadMs = afterPreload - afterLayout;
      // Font/image arrival can invalidate intrinsic sizes. Settle them outside
      // both warmup and measurement, then prime caches through the warmups.
      await boundedSetup(managed.layout(WIDTH, HEIGHT, false), `${id} settled layout`);
      await idle();
      this.alive();
      result.preparation.settledLayoutMs = this.now() - afterPreload;
      if (assets.errors.length) throw new Error(`Asset preload failed: ${assets.errors.join('; ')}`);
      if ((id === 'image-svg' || id === 'lottie') && (assets.tracked < 1 || assets.loadedCallbacks < 1))
        throw new Error('No native asset load evidence');
      bitmap = createBitmap({
        width: WIDTH, height: HEIGHT, rowBytes: WIDTH * 4,
        colorType: BitmapColorType.RGBA8888, alphaType: BitmapAlphaType.Premul,
      });
      // Prime the event queue at t=0 so every subsequent processFrame gets the
      // requested 16ms animation delta, including the first warmup frame.
      managed.processFrame(0);
      for (let i = 0; i < WARMUP_COUNT + MEASURED_COUNT; i++) {
        this.alive();
        const begin = this.now();
        managed.processFrame(ANIMATION_DELTA_MS);
        const afterProcess = this.now();
        const { frame } = await managed.draw();
        let duration: number;
        let drawMs: number;
        let rasterMs: number;
        try {
          this.alive();
          const afterDraw = this.now();
          drawMs = afterDraw - afterProcess;
          frame.rasterInto(bitmap, true);
          const end = this.now();
          rasterMs = end - afterDraw;
          duration = end - begin;
        } finally {
          frame.dispose();
        }
        // Bookkeeping, correctness checks and JS pixel access are outside timing.
        const phases = i < WARMUP_COUNT ? result.warmupPhaseDurationsMs : result.phaseDurationsMs;
        phases.processFrameMs.push(afterProcess - begin);
        phases.drawMs.push(drawMs);
        phases.rasterMs.push(rasterMs);
        if (i < WARMUP_COUNT) result.warmupDurationsMs.push(duration);
        else {
          result.durationsMs.push(duration);
          if (i === WARMUP_COUNT) result.first = inspectBitmap(bitmap, id);
          if (i === WARMUP_COUNT + MEASURED_COUNT - 1) result.last = inspectBitmap(bitmap, id);
        }
      }
      this.alive();
      let finite = validTimings(result.warmupDurationsMs, result.durationsMs)
        && validPhases(result.warmupPhaseDurationsMs, WARMUP_COUNT)
        && validPhases(result.phaseDurationsMs, MEASURED_COUNT);
      if (finite) {
        result.summary = summarize(result.durationsMs);
        finite = Object.values(result.summary).every(value => Number.isFinite(value) && value > 0);
      }
      result.outputRelationshipPassed = !!result.first && !!result.last
        && (id === 'lottie' ? result.first.hash !== result.last.hash : result.first.hash === result.last.hash);
      result.completed = true;
      result.status = finite && !!result.first?.passed && !!result.last?.passed
        && result.outputRelationshipPassed && assets.errors.length === 0 ? 'PASS' : 'FAIL';
      if (result.status === 'FAIL') result.error = JSON.stringify({
        finiteTimings: finite, firstCanary: result.first?.passed === true,
        lastCanary: result.last?.passed === true, outputRelationship: result.outputRelationshipPassed,
        assetErrors: assets.errors,
      });
    } catch (error) {
      result.error = errorText(error);
    } finally {
      bitmap?.dispose();
      if (this.active === managed) {
        this.active = undefined;
        managed.dispose();
      }
    }
    return result;
  }

  private async run(): Promise<void> {
    const scenes: SceneResult[] = [];
    let engine: object | undefined;
    try {
      this.emit({ kind: 'started', status: 'RUNNING', completed: false, suiteIds: SUITE_IDS,
        plan: PLAN, provenance: PROVENANCE });
      const properties = globalThis.HermesInternal?.getRuntimeProperties?.();
      if (!properties || properties['OSS Release Version'] !== PROVENANCE.hermesRelease
          || !(Number(properties['Bytecode Version']) > 0))
        throw new Error(`Executing Hermes contract mismatch: ${JSON.stringify(properties)}`);
      engine = { name: 'Hermes', verified: true, properties };
      this.now(); // Validate the required clock before setup or timing.
      for (const id of SUITE_IDS) {
        this.alive();
        const result = await this.scene(id);
        this.alive();
        scenes.push(result);
        this.emit({ kind: 'scene', status: result.status, completed: result.completed,
          suiteIds: SUITE_IDS, index: scenes.length - 1, result });
      }
      const completed = scenes.length === SUITE_IDS.length
        && scenes.every((scene, i) => scene.completed && scene.id === SUITE_IDS[i]);
      const passed = completed && scenes.every(scene => scene.status === 'PASS');
      this.emit({ kind: 'finished', status: passed ? 'PASS' : 'FAIL', completed,
        suiteIds: SUITE_IDS, plan: PLAN, engine, provenance: PROVENANCE, scenes });
      this.setState({ status: `${passed ? 'PASS' : 'FAIL'} ${scenes.filter(scene => scene.status === 'PASS').length}/${SUITE_IDS.length}` });
    } catch (error) {
      if (!this.destroyed) {
        this.emit({ kind: 'finished', status: 'FAIL', completed: false, suiteIds: SUITE_IDS,
          plan: PLAN, engine, provenance: PROVENANCE, scenes, error: errorText(error) });
        this.setState({ status: 'FAIL benchmark aborted' });
      }
    }
  }
}
