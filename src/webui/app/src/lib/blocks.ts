// The block ribbon of the explorer (components/BlockRibbon.svelte, BlockBox.svelte): what a box of a block
// shows, from the numbers of the node. Apart from the components so that it can be tested on its own.

import { CHAIN } from '../chain/config';

/** The most weight a block of Chains may have (consensus.h CHAINS_MAX_BLOCK_WEIGHT). */
export const CHAINS_MAX_BLOCK_WEIGHT = 800_000;
/** The most weight a block of this chain may have when the node has not said (app.info.blockWeight, per network): chain.json blockWeight, else Chains'. */
export const BLOCK_WEIGHT = CHAIN.blockWeight ?? CHAINS_MAX_BLOCK_WEIGHT;
/** The pace Chains aims at: a block a minute (nPowTargetSpacing). */
export const TARGET_SPACING = 60;

/** A block of the ribbon: the row of the table, and what getblockstats, the coinbase and the sidechains add. */
export interface RibbonBlock {
  height: number;
  hash: string;
  time: number;
  nTx: number;
  /** null until read (the explorer reads the blocks of the boxes it details). */
  weight: number | null;
  /** Subsidy and fees (sats); null until known. */
  subsidy: number | null;
  fees: number | null;
  /** Average fee rate of the block (sat/vB); null until known. */
  feeRate: number | null;
  /** The text of the coinbase (the miner's tag), printable runs only; '' when it has none. */
  tag: string | null;
  /** The sidechains (slots) whose block this block committed to (blind merged mining). A mainchain only. */
  bmm: number[];
  /**
   * A sidechain: the block of the mainchain that committed to this one (blind merged mining), when the
   * events of the mainchain the app follows have it; null when not known (older, or not followed yet).
   */
  main?: { height: number; hash: string } | null;
}

/** The block being mined (the outline at the head of the ribbon). */
export interface NextBlock {
  height: number;
  /** Transactions waiting, their size (vbytes) and their fees (sats). */
  txs: number;
  vbytes: number;
  fees: number;
  /** Subsidy plus the fees waiting (sats), or null. */
  reward: number | null;
  /** Seconds expected at the hash rate of the network, or null. */
  eta: number | null;
}

/** Seconds between a block and the one before it (the next in a newest-first list); null when unknown. */
export function intervalOf(blocks: readonly Pick<RibbonBlock, 'time'>[], i: number): number | null {
  const prev = blocks[i + 1];
  if (!prev) return null;
  const dt = blocks[i].time - prev.time;
  return dt >= 0 ? dt : null;
}

/** The median of the intervals of the list: the recent pace of the chain. */
export function medianInterval(blocks: readonly Pick<RibbonBlock, 'time'>[]): number | null {
  const gaps: number[] = [];
  for (let i = 0; i < blocks.length - 1; i++) {
    const g = intervalOf(blocks, i);
    if (g !== null) gaps.push(g);
  }
  if (gaps.length === 0) return null;
  gaps.sort((a, b) => a - b);
  return gaps[Math.floor(gaps.length / 2)];
}

/**
 * An interval against the recent pace of the chain (not a fixed threshold: a busy regtest and the one
 * minute of Chains are both normal for themselves): fast under half of it, slow over twice.
 */
export function intervalTone(secs: number | null, median: number | null): 'fast' | 'slow' | 'normal' | 'unknown' {
  if (secs === null || median === null || median <= 0) return 'unknown';
  if (secs < median * 0.5) return 'fast';
  if (secs > median * 2) return 'slow';
  return 'normal';
}

/** An interval, short: 42s, 3m 5s, 1.5h. */
export function shortInterval(secs: number | null): string {
  if (secs === null) return '–';
  if (secs < 60) return `${Math.round(secs)}s`;
  if (secs < 3600) return `${Math.floor(secs / 60)}m ${Math.round(secs % 60)}s`;
  return `${(secs / 3600).toFixed(1)}h`;
}

/** How full a block is, in percent of the most weight it may have: 0 for an empty one, at least 6 when it holds any. */
export function fillPercent(weight: number, nTx: number, max = CHAINS_MAX_BLOCK_WEIGHT): number {
  // The coinbase alone is an empty block.
  if (nTx <= 1 || weight <= 0 || max <= 0) return 0;
  return Math.max(6, Math.min(100, (weight / max) * 100));
}

/** The tint of the fill: what the transactions paid, on average (sat/vB). */
export function feeTint(feeRate: number | null): 'low' | 'mid' | 'high' | 'none' {
  if (feeRate === null || feeRate <= 0) return 'none';
  if (feeRate >= 10) return 'high';
  if (feeRate >= 2) return 'mid';
  return 'low';
}

/**
 * The text a miner put in the coinbase (its tag): the runs of 3 printable ASCII characters or more of
 * the script, after the height (BIP34), joined by a space. Text of the chain: shown with SafeText.
 */
export function coinbaseTag(hex: string, max = 40): string {
  if (!/^[0-9a-fA-F]*$/.test(hex) || hex.length % 2) return '';
  const bytes: number[] = [];
  for (let i = 0; i < hex.length; i += 2) bytes.push(parseInt(hex.slice(i, i + 2), 16));
  // The height first: a push of 1 to 8 bytes (or OP_1..OP_16, OP_0).
  let start = 0;
  if (bytes.length && bytes[0] >= 1 && bytes[0] <= 8) start = 1 + bytes[0];
  else if (bytes.length && (bytes[0] === 0 || (bytes[0] >= 0x51 && bytes[0] <= 0x60))) start = 1;
  const runs: string[] = [];
  let run = '';
  for (const b of bytes.slice(start)) {
    if (b >= 0x20 && b < 0x7f) run += String.fromCharCode(b);
    else {
      if (run.trim().length >= 3) runs.push(run.trim());
      run = '';
    }
  }
  if (run.trim().length >= 3) runs.push(run.trim());
  const text = runs.join(' ');
  return text.length > max ? `${text.slice(0, max - 1)}…` : text;
}

/** The expected time to the next block at the hash rate of the network (difficulty × 2³² / hashes a second). */
export function expectedSeconds(difficulty: number, hashesPerSecond: number): number | null {
  if (!(difficulty > 0) || !(hashesPerSecond > 0)) return null;
  const secs = (difficulty * 2 ** 32) / hashesPerSecond;
  // The hash rate is guessed from the times of the last blocks: when they are far off the pace of the chain
  // (a test chain, blocks made at once), the guess means nothing and the target is shown instead.
  return secs >= 1 && secs <= 86400 ? secs : null;
}
