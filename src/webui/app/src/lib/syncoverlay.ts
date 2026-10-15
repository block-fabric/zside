// The estimates of the sync overlay (ModalOverlay of chains-qt): blocks left, progress per hour and
// the time left, from samples of the verification progress.
import { niceTimeOffset } from './format';
import { HEADER_HEIGHT_DELTA_SYNC } from './sync';

export interface ProgressSample {
  /** Milliseconds since 1970. */
  t: number;
  progress: number;
}

const MAX_SAMPLES = 5000;

/** Whether the overlay is open because it opened by itself, and whether the user closed it (once a session). */
export const overlay = { shown: false, userClosed: false };

/** Keep a sample, the newest first (ModalOverlay::tipUpdate). */
export function addProgressSample(samples: ProgressSample[], sample: ProgressSample): void {
  samples.unshift(sample);
  if (samples.length > MAX_SAMPLES) samples.length = MAX_SAMPLES;
}

export interface Estimate {
  /** Percent per hour, as text ("1.25%"), or "calculating…". */
  perHour: string;
  /** The time left, in words, "unknown", or "calculating…". */
  timeLeft: string;
}

/** The speed and the time left: from the newest sample and the first one older than 500 s, or the oldest. */
export function estimate(samples: readonly ProgressSample[], now: number): Estimate {
  if (samples.length < 2) return { perHour: 'calculating…', timeLeft: 'calculating…' };
  const remaining = 1 - samples[0].progress;
  for (let i = 1; i < samples.length; i++) {
    const s = samples[i];
    if (s.t < now - 500_000 || i === samples.length - 1) {
      const progressDelta = samples[0].progress - s.progress;
      const timeDelta = samples[0].t - s.t;
      const perHour = progressDelta > 0 && timeDelta > 0 ? (progressDelta / timeDelta) * 1000 * 3600 : 0;
      const remainingMs = progressDelta > 0 ? (remaining / progressDelta) * timeDelta : -1;
      return { perHour: `${(perHour * 100).toFixed(2)}%`, timeLeft: remainingMs >= 0 ? niceTimeOffset(remainingMs / 1000) : 'unknown' };
    }
  }
  return { perHour: 'calculating…', timeLeft: 'calculating…' };
}

/** The blocks left, or what the headers sync is at (ModalOverlay::UpdateHeaderSyncLabel). */
export function blocksLeft(tipHeight: number, headerHeight: number, headerTime: number, spacing: number, nowSecs: number, presync?: { height: number; time: number }): string {
  if (presync && presync.height > headerHeight) {
    const left = Math.floor((nowSecs - presync.time) / spacing);
    return `Unknown. Pre-syncing Headers (${presync.height}, ${((100 / (presync.height + left)) * presync.height).toFixed(1)}%)…`;
  }
  if (!headerTime) return 'Unknown…';
  const estLeft = Math.floor((nowSecs - headerTime) / spacing);
  if (estLeft < HEADER_HEIGHT_DELTA_SYNC && headerHeight >= tipHeight) return String(headerHeight - tipHeight);
  return `Unknown. Syncing Headers (${headerHeight}, ${((100 / (headerHeight + estLeft)) * headerHeight).toFixed(1)}%)…`;
}
