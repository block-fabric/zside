// What the app is doing in several blocks for the user, shown as it goes (the progress cards of the pages, the
// indicator of the top bar): a name or an asset being taken in two steps (a reservation that hides it, then, once it
// is deep enough, the registration that reveals it, sent by the app by itself). Each chain's store publishes its own
// (publishPending), from what it keeps in its journal and what the wallet says: they survive page switches and a
// restart of the app as the journal does.
import { go, type PageId } from './nav.svelte';
import { t } from './i18n.svelte';
import { app, node } from './state.svelte';

export type PendingStage =
  /** The reservation is sent, not in a block yet. */
  | 'reserved'
  /** In a block, not deep enough yet: the app registers it by itself then. */
  | 'waiting'
  /** Deep enough, and the app needs the user's word (after a restart) or the passphrase of a locked wallet. */
  | 'confirm'
  /** The registration is sent: in the next block. */
  | 'registering'
  | 'done'
  | 'failed';

export interface PendingOp {
  /** Unique among those of its source. */
  key: string;
  /** What it is ("Asset", "Name"): a word of the app. */
  what: string;
  /** Its name, as typed: text of the user. */
  name: string;
  wallet: string;
  stage: PendingStage;
  /** How many blocks deep the reservation is (null before it is in one), and how deep it must be to be registered. */
  depth: number | null;
  needed: number;
  error?: string;
  /** The page (and tab) of its card. */
  page?: PageId;
  tab?: string;
  /** What the user can do now: confirm the registration, try again. */
  action?: { label: string; run: () => void };
  /** When it was done or failed (ms), for how long the indicator still shows it. */
  ended?: number;
}

export const pending = $state({ bySource: {} as Record<string, PendingOp[]> });

/** A chain's store says what it is doing now: all of it (the ones it no longer lists are gone). */
export function publishPending(source: string, ops: PendingOp[]): void {
  pending.bySource[source] = ops;
}

/** Every operation going on, of the wallet shown first. */
export function pendingList(wallet?: string | null): PendingOp[] {
  const all = Object.values(pending.bySource).flat();
  return wallet === undefined ? all : all.filter((o) => o.wallet === wallet);
}

/** Done or failed this long ago (ms) and still shown by the indicator. */
export const SHOW_ENDED_MS = 60_000;

/** The ones the indicator shows: going on, or ended a moment ago. */
export function activeOps(now = Date.now(), list = pendingList()): PendingOp[] {
  return list.filter((o) => (o.stage !== 'done' && o.stage !== 'failed') || (o.ended !== undefined && now - o.ended < SHOW_ENDED_MS));
}

/**
 * The seconds between blocks of this chain, as they come now: the median of the last blocks the app saw (a sidechain
 * follows the blocks of its mainchain, a chain of slots its slots), else the chain's target.
 */
export function blockSeconds(recent: readonly { time: number }[] = node.recent, target = app.info?.blockSpacing ?? 60): number {
  const gaps: number[] = [];
  for (let i = 1; i < recent.length; i++) {
    const gap = recent[i].time - recent[i - 1].time;
    if (gap > 0) gaps.push(gap);
  }
  if (gaps.length < 3) return target > 0 ? target : 60;
  gaps.sort((a, b) => a - b);
  return Math.max(1, gaps[Math.floor(gaps.length / 2)]);
}

/** The blocks still to come before it is registered: those the reservation still needs, and the one that registers it. */
export function blocksLeft(op: Pick<PendingOp, 'stage' | 'depth' | 'needed'>): number {
  switch (op.stage) {
    case 'reserved':
      return op.needed + 1;
    case 'waiting':
      return Math.max(0, op.needed - (op.depth ?? 0)) + 1;
    case 'registering':
      return 1;
    default:
      return 0;
  }
}

/** About how long until it is registered (seconds), or null when it waits for the user or is over. */
export function etaSeconds(op: Pick<PendingOp, 'stage' | 'depth' | 'needed'>, spacing = blockSeconds()): number | null {
  const left = blocksLeft(op);
  return left > 0 ? left * spacing : null;
}

/** "about 45 s", "about 3 min". */
export function etaText(seconds: number | null): string {
  if (seconds === null) return '';
  if (seconds < 90) return t('~%1 s', String(Math.max(5, Math.round(seconds / 5) * 5)));
  return t('~%1 min', String(Math.round(seconds / 60)));
}

/** How far it is, from 0 to 1: the reservation, each block of its depth, the registration. */
export function progressOf(op: Pick<PendingOp, 'stage' | 'depth' | 'needed'>): number {
  const total = op.needed + 2;
  switch (op.stage) {
    case 'reserved':
      return 0.5 / total;
    case 'waiting':
      return (1 + Math.min(op.needed, op.depth ?? 0)) / total;
    case 'confirm':
      return (1 + op.needed) / total;
    case 'registering':
      return (1.5 + op.needed) / total;
    case 'done':
      return 1;
    default:
      return 0;
  }
}

/** Open the page of its card. */
export function openOp(op: PendingOp): void {
  if (op.page) go(op.page, op.tab ?? '');
}
