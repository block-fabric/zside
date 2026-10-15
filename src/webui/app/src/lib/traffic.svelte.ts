// The traffic of the network (TrafficGraphWidget of chains-qt): the totals the node counts, sampled
// every second from the "traffic" events, kept for a day, and the rates over a range of time.

import { t } from './i18n.svelte';

export interface TrafficSample {
  /** Milliseconds since 1970. */
  t: number;
  recv: number;
  sent: number;
}

/** A day of samples, one a second. */
export const MAX_SAMPLES = 24 * 60 * 60;
/** The ranges of the graph: 5 minutes to 24 hours, in steps of 5 minutes (sldGraphRange). */
export const MIN_RANGE_MINUTES = 5;
export const MAX_RANGE_MINUTES = 24 * 60;
export const DEFAULT_RANGE_MINUTES = 30;
/** Points drawn across the graph (DESIRED_SAMPLES). */
export const DESIRED_POINTS = 800;

export const traffic = $state({
  samples: [] as TrafficSample[],
  /** The totals of the node now. */
  recv: 0,
  sent: 0,
  /** Samples before this time are hidden (Clear). */
  clearedAt: 0,
});

export function addSample(sample: TrafficSample): void {
  traffic.recv = sample.recv;
  traffic.sent = sample.sent;
  traffic.samples.push(sample);
  if (traffic.samples.length > MAX_SAMPLES) traffic.samples.splice(0, traffic.samples.length - MAX_SAMPLES);
}

export function clearGraph(now = Date.now()): void {
  traffic.clearedAt = now;
}

export interface RatePoint {
  /** Seconds before now (0 at the right). */
  age: number;
  /** kB/s in and out. */
  inRate: number;
  outRate: number;
}

/**
 * The rates over the last `minutes`, as at most DESIRED_POINTS points: each the bytes between
 * samples, over the time between them.
 */
export function rates(samples: readonly TrafficSample[], minutes: number, now: number, clearedAt = 0): RatePoint[] {
  const from = Math.max(now - minutes * 60_000, clearedAt);
  const shown = samples.filter((s) => s.t >= from);
  if (shown.length < 2) return [];
  const bucket = Math.max(1, Math.ceil(shown.length / DESIRED_POINTS));
  const points: RatePoint[] = [];
  for (let i = bucket; i < shown.length; i += bucket) {
    const a = shown[i - bucket];
    const b = shown[i];
    const secs = (b.t - a.t) / 1000;
    if (secs <= 0) continue;
    points.push({ age: (now - b.t) / 1000, inRate: Math.max(0, (b.recv - a.recv) / 1000 / secs), outRate: Math.max(0, (b.sent - a.sent) / 1000 / secs) });
  }
  return points;
}

/** A range of the slider in words: "30 m", "1 h 5 m", "24 h". */
export function rangeText(minutes: number): string {
  const h = Math.floor(minutes / 60);
  const m = minutes % 60;
  if (!h) return t('%1 m', m);
  return m ? t('%1 h %2 m', h, m) : t('%1 h', h);
}
