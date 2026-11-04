import { makeAssetFromBytes } from 'valdi_core/src/Asset';
import { IManagedContext } from 'drawing/src/IManagedContext';
import { FIXTURES } from './Fixtures';
import res from 'render_benchmark/res';

export const WIDTH = 360;
export const HEIGHT = 640;
export const WARMUP_COUNT = 30;
export const MEASURED_COUNT = 300;
// Simulation time only. This does not pace work to the display or wall clock.
export const ANIMATION_DELTA_MS = 16;
export const SUITE_IDS = ['cached-vector', 'shaped-text', 'image-svg', 'lottie'] as const;
export type SceneId = typeof SUITE_IDS[number];
export interface AssetEvidence {
  tracked: number;
  loadedCallbacks: number;
  errors: string[];
}

const COLORS = ['#e63946', '#087e8b', '#5546a3', '#e9a820'];
const TEXT = ['office affine fi fl ffi', 'AVATAR To Wa 012345', 'Café naïve coöperate'];

/** Construct the real managed tree once; loading, shaping and layout precede timing. */
export function renderScene(context: IManagedContext, id: SceneId, assets: AssetEvidence): void {
  const onLoad = (success: boolean, error?: string) => {
    if (success) assets.loadedCallbacks++;
    else assets.errors.push(error || 'onAssetLoad returned false');
  };
  // Retain these Assets through the native nodes. No per-frame decode or JS tree work.
  const png = id === 'image-svg' ? makeAssetFromBytes(new Uint8Array(FIXTURES.png)) : undefined;
  const svg = id === 'image-svg' ? makeAssetFromBytes(new Uint8Array(FIXTURES.svg)) : undefined;
  const lottie = id === 'lottie' ? makeAssetFromBytes(new Uint8Array(FIXTURES.lottie)) : undefined;
  context.render(() => {
    <view width={WIDTH} height={HEIGHT} backgroundColor="#ffffff">
      {id === 'cached-vector' && Array.from({ length: 64 }, (_, i) =>
        <view key={`clip-${i}`} position="absolute" left={16 + (i % 8) * 40}
          top={24 + Math.floor(i / 8) * 64} width={36} height={56} slowClipping>
          <view width={44} height={44} backgroundColor={COLORS[i % COLORS.length]}
            translationX={i % 3} rotation={(i % 5 - 2) * 0.04} opacity={0.85} />
        </view>)}
      {id === 'shaped-text' && Array.from({ length: 12 }, (_, i) =>
        <label key={`text-${i}`} position="absolute" left={16} top={24 + i * 44}
          width={328} height={36} numberOfLines={1} font={res.montserratRegular(20)}
          color="#172033" value={TEXT[i % TEXT.length]} />)}
      {id === 'image-svg' && Array.from({ length: 4 }, (_, i) =>
        <view key={`images-${i}`} position="absolute" left={0} top={24 + i * 136}
          width={WIDTH} height={96}>
          <image position="absolute" left={16} top={0} width={144} height={96}
            src={i % 2 === 0 ? png : res.pattern} objectFit="fill" onAssetLoad={onLoad} />
          <image position="absolute" left={196} top={0} width={144} height={96}
            src={svg} objectFit="fill" onAssetLoad={onLoad} />
        </view>)}
      {id === 'lottie' && <animatedimage position="absolute" left={60} top={180}
        width={240} height={240} src={lottie} objectFit="fill" onAssetLoad={onLoad}
        advanceRate={1} loop animationStartTime={0} animationEndTime={2} currentTime={0} />}
    </view>;
  });
}
