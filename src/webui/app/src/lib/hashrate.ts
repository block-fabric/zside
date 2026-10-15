// The network hash estimator of the Mine page: hash rates with their units, parsed and shown, the share
// of the network a miner has, and what that share finds on average. A block takes difficulty × 2^32
// hashes on average (the target of difficulty 1 is 2^32 hashes); getnetworkhashps estimates the rate of
// the network from the work and the times of the last blocks.

export const HASH_UNITS = ['H/s', 'kH/s', 'MH/s', 'GH/s', 'TH/s', 'PH/s', 'EH/s'] as const;
export type HashUnit = (typeof HASH_UNITS)[number];

/** The hashes a block takes on average at a difficulty. */
export const HASHES_PER_DIFFICULTY = 4294967296;

/** A rate split for the page: the whole part, the decimals (dimmed), the unit. */
export interface HashParts {
  whole: string;
  decimals: string;
  unit: HashUnit;
}

/** The unit a rate reads best in (1 ≤ value < 1000, H/s and EH/s at the ends). */
export function hashUnitOf(rate: number): number {
  let i = 0;
  let h = Math.abs(rate);
  while (h >= 1000 && i < HASH_UNITS.length - 1) {
    h /= 1000;
    i++;
  }
  return i;
}

/** A rate in its unit, with digits decimals past H/s ("123.45 TH/s"), whole hashes in H/s. */
export function hashParts(rate: number, digits = 2): HashParts {
  if (!Number.isFinite(rate) || rate < 0) return { whole: '–', decimals: '', unit: 'H/s' };
  let i = hashUnitOf(rate);
  let text = (rate / 1000 ** i).toFixed(i === 0 ? 0 : digits);
  // 999.995 TH/s rounds to "1000.00": one unit up.
  if (Number(text) >= 1000 && i < HASH_UNITS.length - 1) {
    i++;
    text = (rate / 1000 ** i).toFixed(digits);
  }
  const dot = text.indexOf('.');
  return { whole: dot < 0 ? text : text.slice(0, dot), decimals: dot < 0 ? '' : text.slice(dot), unit: HASH_UNITS[i] };
}

/** A rate as text ("12.40 MH/s"). */
export function formatHashRate(rate: number, digits = 2): string {
  const p = hashParts(rate, digits);
  return p.whole === '–' ? '–' : `${p.whole}${p.decimals} ${p.unit}`;
}

/**
 * A rate the user typed: a number, with a unit or not ("100", "100 TH/s", "1.5e3 gh", "2,5 MH/s").
 * Without a unit, the unit given. Null when it is not a rate.
 */
export function parseHashRate(text: string, unit: HashUnit = 'H/s'): { rate: number; unit: HashUnit } | null {
  const m = /^\s*((?:[0-9]+(?:[.,][0-9]*)?|[.,][0-9]+)(?:e[+-]?[0-9]+)?)\s*([kmgtpe])?\s*(h(?:\/s|ps)?)?\s*$/i.exec(text);
  if (!m) return null;
  const value = Number(m[1].replace(',', '.'));
  if (!Number.isFinite(value) || value < 0) return null;
  const u = m[2] || m[3] ? HASH_UNITS['hkmgtpe'.indexOf((m[2] || 'h').toLowerCase())] : unit;
  return { rate: value * 1000 ** HASH_UNITS.indexOf(u), unit: u };
}

/** The share of the network a miner has, 0 to 1 (its rate counts in the network's: it mines there). */
export function networkShare(mine: number, network: number): number {
  if (!(mine > 0)) return 0;
  return mine / (Math.max(network, 0) + mine);
}

/** Seconds a miner takes to find a block on average (difficulty × 2^32 / rate); Infinity at no rate. */
export function expectedSeconds(difficulty: number, rate: number): number {
  if (!(rate > 0) || !(difficulty > 0)) return Infinity;
  return (difficulty * HASHES_PER_DIFFICULTY) / rate;
}

/** Blocks a miner finds in a day on average. */
export function blocksPerDay(difficulty: number, rate: number): number {
  const secs = expectedSeconds(difficulty, rate);
  return Number.isFinite(secs) ? 86400 / secs : 0;
}

/** A window of blocks of the estimate, from a time span and the block spacing of the chain. */
export interface HashWindow {
  id: string;
  label: string;
  blocks: number;
}

/** The windows of the estimate: the last 20 blocks, 2 hours, 12 hours, a day (Chains: 60 s blocks). */
export function hashWindows(spacing: number): HashWindow[] {
  const s = spacing > 0 ? spacing : 60;
  const blocks = (secs: number) => Math.max(1, Math.round(secs / s));
  return [
    { id: '20', label: 'last 20 blocks', blocks: 20 },
    { id: '2h', label: '2 hours', blocks: blocks(7200) },
    { id: '12h', label: '12 hours', blocks: blocks(43200) },
    { id: '1d', label: '1 day', blocks: blocks(86400) },
  ];
}

/** The heights of the points of the chart: every step blocks back from the tip over span blocks, oldest first. */
export function chartHeights(tip: number, span: number, step: number): number[] {
  const out: number[] = [];
  if (tip < 1) return out;
  const st = Math.max(1, step);
  // Fixed heights (multiples of the step) besides the tip: their estimates never change, and are kept.
  const last = tip - (tip % st);
  for (let h = last; h > tip - span && h >= 1; h -= st) out.push(h);
  out.reverse();
  if (out[out.length - 1] !== tip) out.push(tip);
  return out;
}

/** The estimates already asked for, by window and height (blocks below the tip do not change). */
const cache = new Map<string, number>();
let cachedTip = -1;

/**
 * The estimates of the network's rate at the heights, with a window of blocks: the ones known from the
 * cache, the others asked for one after the other (a few RPC calls at a new block, not a burst).
 * ask(nblocks, height) is getnetworkhashps. A tip lower than before (a reorganisation) empties the cache.
 */
export async function estimatesAt(heights: number[], window: number, tip: number, ask: (nblocks: number, height: number) => Promise<number>, signal?: AbortSignal): Promise<number[]> {
  if (tip < cachedTip) cache.clear();
  cachedTip = tip;
  const out: number[] = [];
  for (const h of heights) {
    if (signal?.aborted) throw new DOMException('aborted', 'AbortError');
    // The tip moves on: its estimate is asked anew each time.
    const key = `${window}:${h}`;
    let v = h === tip ? undefined : cache.get(key);
    if (v === undefined) {
      v = await ask(window, h);
      if (h !== tip) cache.set(key, v);
    }
    out.push(v);
  }
  return out;
}

/** For the tests: forget the estimates. */
export function clearEstimates(): void {
  cache.clear();
  cachedTip = -1;
}
