// The state of the peg and of merged mining, kept up to date while the app runs (the Peg and Merged
// mining pages and the cards of the home read it): the node's view every few seconds, the mainchain's
// when its tip moves. Merged mining keeps a log of what happened since the app started.
import { every } from './poll';
import { rpc } from './rpc';
import { node } from './state.svelte';
import {
  EventLog,
  methodMissing,
  peg,
  pegInfoFromLegacy,
  type BmmInfo,
  type Connection,
  type MainBundle,
  type MainchainInfo,
  type PegInfo,
  type SideBundle,
  type SideEvent,
  type Withdrawal,
} from './side';

export interface BmmLogEntry {
  time: number;
  kind: 'request' | 'mined' | 'outbid' | 'error' | 'on' | 'off' | 'manual';
  /** For a request: the fees of the block, and the offer for it (coins). */
  fees?: number;
  offer?: number;
  count?: number;
  text?: string;
}

export const side = $state({
  info: null as MainchainInfo | null,
  connection: null as Connection | null,
  bmm: null as BmmInfo | null,
  bundle: null as SideBundle | null,
  withdrawals: [] as Withdrawal[],
  /** Why the node could not be asked (not a sidechain, starting). */
  error: '',
  main: {
    /** What the mainchain node says of the peg (getpeginfo): the votes a bundle needs, the escrow, the slot. */
    peg: null as PegInfo | null,
    /** The bundles of this sidechain being voted on, their votes now. */
    bundles: [] as MainBundle[],
    events: [] as SideEvent[],
    /** Why the mainchain node could not be asked (its blocks: the events). */
    error: '',
    /** Why getpeginfo could not be had (the pages leave out what it gives, quietly). */
    pegError: '',
    /** The mainchain height the above are of. */
    height: -1,
  },
  log: [] as BmmLogEntry[],
  /** Counts the refreshes (pages that follow it). */
  version: 0,
});

/** How often the node is asked (getmainchaininfo, getbmminfo). */
export const POLL_MS = 3000;
/** How often the mainchain node is asked when its tip does not move. */
const MAIN_POLL_MS = 30_000;
const LOG_MAX = 300;

let timer: (() => void) | null = null;
let running = false;
let lastTip = '';
let lastMainAsk = 0;
let eventLog: EventLog | null = null;

function log(entry: Omit<BmmLogEntry, 'time'>) {
  side.log = [{ time: Math.floor(Date.now() / 1000), ...entry }, ...side.log].slice(0, LOG_MAX);
}

/** What changed in merged mining since the last look, in the log. */
export function noteBmm(before: BmmInfo | null, after: BmmInfo): void {
  if (!before) return;
  if (after.mining !== before.mining) log({ kind: after.mining ? 'on' : 'off' });
  if (after.requests > before.requests) log({ kind: 'request', count: after.requests - before.requests, fees: after.lastfees, offer: after.lastoffer });
  if (after.blocks > before.blocks) log({ kind: 'mined', count: after.blocks - before.blocks });
  if (after.outbid > before.outbid) log({ kind: 'outbid', count: after.outbid - before.outbid });
  if (after.error && after.error !== before.error) log({ kind: 'error', text: after.error });
}

/** A block asked for by hand (the Merged mining page), in the log. */
export function noteManualRequest(fees: number, offer: number): void {
  log({ kind: 'manual', fees, offer, count: 1 });
}

/**
 * What the mainchain node says of the peg: getpeginfo of the node, the same over JSON-RPC and through the enforcer's
 * interface. A node of before it, over JSON-RPC: getdrivechaininfo and listwithdrawalbundles of the mainchain node
 * (peg.call); through the enforcer's interface, which has neither, nothing is asked.
 */
export async function loadPegInfo(slot: number, connection: Connection | null): Promise<PegInfo | null> {
  try {
    const info = await rpc.raw<PegInfo>('getpeginfo');
    return info && typeof info === 'object' && Array.isArray(info.bundles) ? info : null;
  } catch (e) {
    if (!methodMissing(e)) throw e;
    if (connection?.via === 'enforcer') return null;
    const [drivechain, bundles] = await Promise.all([peg.call<Record<string, number>>('getdrivechaininfo'), peg.call<unknown>('listwithdrawalbundles', [slot])]);
    return pegInfoFromLegacy(drivechain, bundles, slot, connection?.via);
  }
}

async function refreshMain(force: boolean) {
  const info = side.info;
  if (!info) return;
  const now = Date.now();
  if (!force && info.height === side.main.height && now - lastMainAsk < MAIN_POLL_MS) return;
  lastMainAsk = now;
  try {
    side.connection = await peg.connection();
  } catch {
    // (The host's own answer: kept as it was.)
  }
  if (!info.connected || info.height < 0) {
    side.main.error = info.error ?? '';
    return;
  }
  eventLog ??= new EventLog(info.slot);
  // The blocks (the events) and the peg apart: one failing leaves the other shown.
  const [events, pegInfo] = await Promise.allSettled([eventLog.update(info.height), loadPegInfo(info.slot, side.connection)]);
  if (events.status === 'fulfilled') {
    side.main.events = [...events.value];
    side.main.height = info.height;
    side.main.error = '';
  } else {
    side.main.error = events.reason instanceof Error ? events.reason.message : String(events.reason);
  }
  if (pegInfo.status === 'fulfilled') {
    side.main.peg = pegInfo.value;
    side.main.bundles = pegInfo.value?.bundles ?? [];
    side.main.pegError = '';
  } else {
    side.main.pegError = pegInfo.reason instanceof Error ? pegInfo.reason.message : String(pegInfo.reason);
  }
}

/** Ask the node (and the mainchain node when its tip moved) again now. */
export async function refreshSide(force = false): Promise<void> {
  if (running && !force) return;
  running = true;
  try {
    const [info, bmm] = await Promise.all([rpc.raw<MainchainInfo>('getmainchaininfo'), rpc.raw<BmmInfo>('getbmminfo')]);
    // A reply of another shape (a node that is not a sidechain, a pretend node) is taken as none.
    side.info = info && typeof info === 'object' && typeof info.height === 'number' ? info : null;
    if (bmm && typeof bmm === 'object' && typeof bmm.requests === 'number') {
      noteBmm(side.bmm, bmm);
      side.bmm = bmm;
    }
    side.error = '';
    const tip = node.tip?.hash ?? '';
    if (force || tip !== lastTip) {
      lastTip = tip;
      const [bundle, withdrawals] = await Promise.all([rpc.raw<SideBundle>('getwithdrawalbundle'), rpc.raw<Withdrawal[]>('listwithdrawals')]);
      side.bundle = bundle && typeof bundle === 'object' && typeof bundle.status === 'string' ? bundle : null;
      side.withdrawals = Array.isArray(withdrawals) ? withdrawals : [];
    }
    await refreshMain(force);
  } catch (e) {
    side.error = e instanceof Error ? e.message : String(e);
  } finally {
    running = false;
    side.version++;
  }
}

/** Start following the peg (once; the pages and the home call it when they show). */
export function startSide(): void {
  if (timer !== null) return;
  void refreshSide();
  // Resting while the window is hidden, as the timers of the pages (lib/poll.ts).
  timer = every(POLL_MS, () => void refreshSide());
}

/** Stop following (the tests). */
export function stopSide(): void {
  timer?.();
  timer = null;
  eventLog = null;
  lastTip = '';
  lastMainAsk = 0;
}
