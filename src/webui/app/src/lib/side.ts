// A sidechain and its mainchain (chain.json role "sidechain"): what the node says of the peg (the
// mainchain it follows, the withdrawals and their bundle, blind merged mining), what the mainchain node
// says of this sidechain (its bundles and their votes, the deposits, the blocks that committed to it),
// and how the pages put them together. The calls to the mainchain node go through the host
// (src/webui/sidechainapi.cpp: peg.*), on the node's own connection to it.
import { ErrorCode, request } from './bridge';
import { rpc } from './rpc';
import { coinsToSats } from './units';

// --- The node of this chain ---

/** getmainchaininfo: how the node follows the mainchain. */
export interface MainchainInfo {
  slot: number;
  connected: boolean;
  error?: string;
  /** host:port of the mainchain node. */
  node: string;
  /** The last mainchain block on record (-1: none). */
  height: number;
  bestblockhash?: string;
  /** The last mainchain block the tip of this chain acted on: deposits after it are not credited yet. */
  tipheight: number;
}

/** listwithdrawals: a withdrawal the mainchain has not paid yet. */
export interface Withdrawal {
  txid: string;
  vout: number;
  amount: number;
  mainchainfee: number;
  mainchainscript: string;
  refundaddress: string;
  height: number;
  /** waiting: for a bundle (it can be refunded); bundled: in the bundle being voted on. */
  status: 'waiting' | 'bundled';
}

/** getwithdrawalbundle: the bundle the mainchain votes on, or the next one. */
export interface SideBundle {
  status: 'pending' | 'next' | 'none';
  hash?: string;
  height?: number;
  withdrawals?: number;
  amount?: number;
  mainchainfee?: number;
  waiting: number;
  lastfailureheight?: number;
}

/** getbmminfo: continuous blind merged mining. */
export interface BmmInfo {
  mining: boolean;
  address?: string;
  always?: boolean;
  amount?: number;
  idle?: boolean;
  lastfees?: number;
  lastoffer?: number;
  requests: number;
  blocks: number;
  outbid: number;
  error?: string;
  candidates: number;
}

/** requestbmmblock: one block asked for by hand. */
export interface BmmRequest {
  hash: string;
  mainchainheight: number;
  transactions: number;
  fees: number;
  amount: number;
  mainchaintxid: string;
}

// --- The mainchain node (peg.*) ---

/** peg.connection: how the node reaches its mainchain node (never its credentials). */
export interface Connection {
  connected: boolean;
  error?: string;
  node: string;
  slot: number;
  walletSet: boolean;
  wallet: string;
  auth: 'password' | 'cookie';
  cookieFile?: string;
}

/** getdrivechaininfo of the mainchain: what a bundle needs. */
export interface MainDrivechainInfo {
  withdrawalperiod: number;
  withdrawalminscore: number;
  height: number;
}

/** listwithdrawalbundles of the mainchain: a bundle of this sidechain being voted on. */
export interface MainBundle {
  slot: number;
  hash: string;
  index: number;
  height: number;
  score: number;
  lastupvote: number;
  blocksleft: number;
  payable: boolean;
  known: boolean;
  vote: string;
}

/** getsidechainevents of the mainchain: what a block did that concerns this sidechain. */
export interface SideEvent {
  height: number;
  hash: string;
  previousblockhash?: string;
  time: number;
  /** The block of this chain it committed to. */
  bmm?: string;
  deposits: { destination: string; amount: number; txid: string; burnindex: number; bundle?: string }[];
  bundles: { hash: string; paid: boolean }[];
  proposed: string[];
  pending: { hash: string; score: number }[];
}

export const peg = {
  connection: () => request<Connection>('host', 'peg.connection', {}),
  /** A method of the mainchain node, from the list the host allows (reading only, and its wallet's balances and addresses). */
  call: <T>(method: string, params: unknown[] = []) => request<T>('host', 'peg.call', { method, params }),
  /** The deposit helper: the wallet of the mainchain node deposits `amount` (coins, text) to `destination`. */
  deposit: (destination: string, amount: string) => request<{ txid: string; total: number }>('host', 'peg.deposit', { destination, amount }),
};

/**
 * Merged mining, through the host: what the wallet of the mainchain node offers is checked there and confirmed in a
 * dialog of the desktop (setbmm and requestbmmblock are not methods the page calls itself). A decline rejects with
 * ErrorCode.CANCELLED.
 */
export const bmmHost = {
  /** On (the address of the fees; with `always`, `amount` offered for every block of the mainchain) or off. */
  set: (mine: boolean, address?: string, always?: boolean, amount?: string) =>
    request<{ mining: boolean }>('host', 'bmm.set', mine ? { mine, address, always: !!always, ...(always ? { amount } : {}) } : { mine }),
  /** One block by hand. */
  request: (address: string, amount: string) => request<BmmRequest>('host', 'bmm.request', { address, amount }),
};

/** Whether an error is the user's "no" (a confirmation of the desktop declined) or a cancelled call. */
export function declined(e: unknown): boolean {
  return !!e && typeof e === 'object' && (e as { code?: number }).code === ErrorCode.CANCELLED;
}

// --- The history of the mainchain blocks that concern this sidechain ---

/** Mainchain blocks kept: getsidechainevents gives at most 2000 at once. */
export const EVENT_WINDOW = 2000;
/** Blocks asked again at each refresh, for a reorganisation of the mainchain. */
const REORG_DEPTH = 6;

/**
 * The events of the last EVENT_WINDOW mainchain blocks, kept up to date: only the new blocks (and the
 * last REORG_DEPTH again) are asked for, and all of them again when the chain they follow changed.
 */
export class EventLog {
  events: SideEvent[] = [];
  constructor(private readonly slot: number) {}

  async update(mainHeight: number, ask: (from: number, count: number) => Promise<SideEvent[]> = (from, count) => peg.call<SideEvent[]>('getsidechainevents', [this.slot, from, count])): Promise<SideEvent[]> {
    if (mainHeight < 0) return (this.events = []);
    const first = Math.max(0, mainHeight - EVENT_WINDOW + 1);
    const last = this.events.length ? this.events[this.events.length - 1].height : -1;
    let from = last < first ? first : Math.max(first, last + 1 - REORG_DEPTH);
    if (from > mainHeight) return this.events;
    let fresh = await ask(from, mainHeight - from + 1);
    if (!Array.isArray(fresh)) fresh = [];
    const kept = this.events.filter((e) => e.height >= first && e.height < from);
    // The first block asked must follow the last one kept, or the chain changed below: everything again.
    if (kept.length && fresh.length && fresh[0].previousblockhash && fresh[0].previousblockhash !== kept[kept.length - 1].hash) {
      from = first;
      fresh = await ask(from, mainHeight - from + 1);
      this.events = Array.isArray(fresh) ? fresh : [];
      return this.events;
    }
    this.events = [...kept, ...fresh];
    return this.events;
  }
}

export interface BundleHistory {
  hash: string;
  /** The mainchain block that proposed it (M3). */
  proposed?: number;
  proposedTime?: number;
  /** The mainchain block that closed it: paid out, or failed. */
  closed?: number;
  closedTime?: number;
  paid?: boolean;
  /** Its score in the last block on record while it was pending. */
  score: number;
}

/** The bundles of this sidechain the events saw, the newest first. */
export function bundleHistory(events: readonly SideEvent[]): BundleHistory[] {
  const byHash = new Map<string, BundleHistory>();
  const get = (hash: string) => {
    let b = byHash.get(hash);
    if (!b) byHash.set(hash, (b = { hash, score: 0 }));
    return b;
  };
  for (const e of events) {
    for (const hash of e.proposed) Object.assign(get(hash), { proposed: e.height, proposedTime: e.time });
    for (const p of e.pending) get(p.hash).score = p.score;
    for (const c of e.bundles) Object.assign(get(c.hash), { closed: e.height, closedTime: e.time, paid: c.paid });
  }
  return [...byHash.values()].sort((a, b) => (b.closed ?? b.proposed ?? Infinity) - (a.closed ?? a.proposed ?? Infinity) || (b.proposed ?? 0) - (a.proposed ?? 0));
}

export interface DepositEntry {
  height: number;
  time: number;
  txid: string;
  burnindex: number;
  destination: string;
  sats: bigint;
  /** The tip of this chain has acted on its block: the coins are on this chain. */
  credited: boolean;
}

/** The deposits to this sidechain in the events (not the change of withdrawals), the newest first. */
export function depositsOf(events: readonly SideEvent[], tipheight: number): DepositEntry[] {
  const out: DepositEntry[] = [];
  for (const e of events) {
    for (const d of e.deposits) {
      if (d.bundle) continue;
      out.push({ height: e.height, time: e.time, txid: d.txid, burnindex: d.burnindex, destination: d.destination, sats: coinsToSats(d.amount), credited: e.height <= tipheight });
    }
  }
  return out.reverse();
}

export interface BmmBlock {
  height: number;
  time: number;
  hash: string;
  /** The block of this chain it committed to, if any. */
  bmm?: string;
}

/** The last `count` mainchain blocks, with the block of this chain each committed to, the newest first. */
export function bmmBlocks(events: readonly SideEvent[], count: number): BmmBlock[] {
  return events
    .slice(-count)
    .map((e) => ({ height: e.height, time: e.time, hash: e.hash, bmm: e.bmm }))
    .reverse();
}

/** Of the last `count` mainchain blocks, how many committed to a block of this chain. */
export function bmmShare(events: readonly SideEvent[], count: number): { committed: number; of: number } {
  const last = events.slice(-count);
  return { committed: last.filter((e) => !!e.bmm).length, of: last.length };
}

// --- Deposit addresses: s<slot>_<address>_<checksum> ---

/** The address of this chain a deposit address credits, if it is one of the slot (the checksum is the host's to check). */
export function depositTarget(destination: string, slot: number): string | null {
  const m = /^s(\d+)_([^_]+)_[^_]{6}$/.exec(destination.trim());
  if (!m || Number(m[1]) !== slot) return null;
  return m[2];
}

// --- Withdrawals ---

/** The withdrawals whose refund address is one of the wallet (only those it can take back). */
export async function ownWithdrawals(list: readonly Withdrawal[], wallet: string, known: Map<string, boolean>): Promise<Set<string>> {
  const mine = new Set<string>();
  for (const w of list) {
    let is = known.get(w.refundaddress);
    if (is === undefined) {
      try {
        const info = await rpc.raw<{ ismine: boolean }>('getaddressinfo', [w.refundaddress], { wallet });
        is = !!info.ismine;
      } catch {
        is = false;
      }
      known.set(w.refundaddress, is);
    }
    if (is) mine.add(`${w.txid}:${w.vout}`);
  }
  return mine;
}

export function withdrawalKey(w: Pick<Withdrawal, 'txid' | 'vout'>): string {
  return `${w.txid}:${w.vout}`;
}

/** Of a bundle being voted on, how far it is: its score out of the score it needs, in percent (0–100). */
export function voteProgress(score: number, minScore: number): number {
  return minScore > 0 ? Math.max(0, Math.min(100, (score / minScore) * 100)) : 0;
}

/** Whether a bundle can still pass: payable, or enough blocks left to reach the score. */
export function bundleCanPass(b: Pick<MainBundle, 'payable' | 'score' | 'blocksleft'>, minScore: number): boolean {
  return b.payable || b.score + b.blocksleft >= minScore;
}
