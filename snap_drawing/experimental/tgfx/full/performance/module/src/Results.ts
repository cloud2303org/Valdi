import { IBitmap } from 'drawing/src/IBitmap';
import { HEIGHT, MEASURED_COUNT, SceneId, WIDTH, WARMUP_COUNT } from './Scenes';

export const METRIC = 'GPU-synchronized render/readback throughput';
export const SCOPE = 'managed bitmap API throughput; not screen FPS or GPU-vs-GPU';

export interface PhaseTimings {
  processFrameMs: number[];
  drawMs: number[];
  rasterMs: number[];
}

export function emptyPhases(): PhaseTimings {
  return { processFrameMs: [], drawMs: [], rasterMs: [] };
}

export function validPhases(phases: PhaseTimings, count: number): boolean {
  return [phases.processFrameMs, phases.drawMs, phases.rasterMs].every(
    values => values.length === count && values.every(ms => Number.isFinite(ms) && ms >= 0),
  );
}

export interface Canary {
  hashAlgorithm: 'fnv1a32-rgba';
  hash: string;
  byteLength: number;
  nonWhitePixels: number;
  opaquePixels: number;
  sceneInkPixels: number;
  samples: { [name: string]: number[] };
  passed: boolean;
}
export interface TimingSummary {
  totalMs: number;
  meanMs: number;
  minMs: number;
  p50Ms: number;
  p95Ms: number;
  maxMs: number;
  rendersPerSecond: number;
}

function close(actual: number[], expected: number[], tolerance: number): boolean {
  return actual.every((value, i) => Math.abs(value - expected[i]) <= tolerance);
}

/** Read and hash actual RGBA output after the timer stops. No encoding. */
export function inspectBitmap(bitmap: IBitmap, id: SceneId): Canary {
  return bitmap.accessPixels(view => {
    const pixels = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
    const sample = (x: number, y: number) => {
      const at = (y * WIDTH + x) * 4;
      return Array.from(pixels.subarray(at, at + 4));
    };
    let hash = 2166136261;
    let nonWhitePixels = 0;
    let opaquePixels = 0;
    let sceneInkPixels = 0;
    for (let i = 0; i < pixels.length; i++) hash = Math.imul(hash ^ pixels[i], 16777619) >>> 0;
    for (let y = 0; y < HEIGHT; y++) for (let x = 0; x < WIDTH; x++) {
      const at = (y * WIDTH + x) * 4;
      if (pixels[at + 3] === 255) opaquePixels++;
      if (pixels[at] < 245 || pixels[at + 1] < 245 || pixels[at + 2] < 245) nonWhitePixels++;
      // Text canary requires dark glyphs in the label region, not background.
      if (x >= 16 && x < 344 && y >= 24 && y < 544 &&
          pixels[at] < 100 && pixels[at + 1] < 100 && pixels[at + 2] < 100) sceneInkPixels++;
    }
    const samples: { [name: string]: number[] } = { background: sample(350, 620) };
    let scenePassed = false;
    if (id === 'cached-vector') {
      samples.vector = sample(34, 46);
      // Red at 85% opacity over white, away from rotated antialiased edges.
      scenePassed = close(samples.vector, [234, 87, 98, 255], 8) && nonWhitePixels > 40000;
    } else if (id === 'shaped-text') {
      scenePassed = sceneInkPixels > 1200 && sceneInkPixels < 60000;
    } else if (id === 'image-svg') {
      samples.pngRed = sample(20, 28);
      samples.catalogRed = sample(20, 164);
      samples.svgRed = sample(200, 28);
      samples.svgCenter = sample(268, 72);
      scenePassed = close(samples.pngRed, [255, 0, 0, 255], 5)
        && close(samples.catalogRed, [255, 0, 0, 255], 50)
        && close(samples.svgRed, [255, 59, 48, 255], 5)
        && close(samples.svgCenter, [255, 255, 255, 255], 5)
        && nonWhitePixels > 40000;
    } else {
      samples.lottieCenter = sample(180, 300);
      samples.lottieOutside = sample(294, 300);
      scenePassed = close(samples.lottieCenter, [0, 122, 255, 255], 5)
        && close(samples.lottieOutside, [255, 255, 255, 255], 5)
        && nonWhitePixels > 7000 && nonWhitePixels < 40000;
    }
    return {
      hashAlgorithm: 'fnv1a32-rgba', hash: hash.toString(16).padStart(8, '0'),
      byteLength: pixels.length, nonWhitePixels, opaquePixels, sceneInkPixels, samples,
      passed: pixels.length === WIDTH * HEIGHT * 4 && opaquePixels === WIDTH * HEIGHT
        && close(samples.background, [255, 255, 255, 255], 0) && scenePassed,
    };
  });
}

export function validTimings(warmup: number[], measured: number[]): boolean {
  return warmup.length === WARMUP_COUNT && measured.length === MEASURED_COUNT
    && warmup.every(ms => Number.isFinite(ms) && ms > 0)
    && measured.every(ms => Number.isFinite(ms) && ms > 0);
}

export function summarize(durations: number[]): TimingSummary {
  const ordered = durations.slice().sort((a, b) => a - b);
  const totalMs = durations.reduce((total, value) => total + value, 0);
  return {
    totalMs, meanMs: totalMs / durations.length, minMs: ordered[0],
    p50Ms: ordered[Math.ceil(ordered.length * 0.50) - 1],
    p95Ms: ordered[Math.ceil(ordered.length * 0.95) - 1],
    maxMs: ordered[ordered.length - 1], rendersPerSecond: durations.length * 1000 / totalMs,
  };
}
