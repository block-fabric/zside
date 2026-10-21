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
  /** host:port of the mainchain node (of its enforcer interface, with -mainchaingrpc). */
  node: string;
  /** How the node reaches it (nodes before 2026-10-18 do not say). */
  via?: MainVia;
  /** The last mainchain block on record (-1: none). */
  height: number;
  bestblockhash?: string;
  /** The last mainchain block the tip of this chain acted on: deposits after it are not credited yet. */
  tipheight: number;
  /** How the link to the mainchain node stands (nodes before 2026-10-20 do not say). */
  state?: 'ok' | 'syncing' | 'down' | 'starting';
  error_kind?: LinkErrorKind;
  /** Since when (unix seconds) the link is in this state. */
  since?: number;
  /** When the mainchain node last answered, last failed (unix seconds). */
  last_ok?: number;
  last_error?: number;
  /** What the mainchain node said of its own chain at the last look. */
  mainchain_height?: number;
  mainchain_headers?: number;
  mainchain_ibd?: boolean;
  mainchain_network?: string;
  followed_height?: number;
  behind_blocks?: number;
}

/** Why the link to the mainchain node is not ok (getmainchaininfo error_kind). */
export type LinkErrorKind = 'refused' | 'unreachable' | 'auth' | 'timeout' | 'wrong-network' | 'not-synced' | 'other';

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

/** How the node reaches its mainchain node: JSON-RPC; JSON-RPC with its requests through the enforcer's interface; that interface alone (-mainchaingrpc). */
export type MainVia = 'json-rpc' | 'json-rpc+enforcer' | 'enforcer';

/** peg.connection: how the node reaches its mainchain node (never its credentials). */
export interface Connection {
  connected: boolean;
  error?: string;
  node: string;
  slot: number;
  /** Nodes before 2026-10-21 do not say: JSON-RPC. */
  via?: MainVia;
  walletSet: boolean;
  wallet: string;
  /** none, token: through the enforcer's interface (no credentials, or -mainchaingrpcauth). */
  auth: 'password' | 'cookie' | 'none' | 'token';
  cookieFile?: string;
}

/** A bundle of this sidechain being voted on (getpeginfo). */
export interface MainBundle {
  hash: string;
  /** The mainchain block that proposed it. */
  height: number;
  /** Its votes now. */
  score: number;
  /** Blocks before it fails; not said by a mainchain that does not report its rules. */
  blocksleft?: number;
  payable?: boolean;
  /** How the mainchain node votes on it (over JSON-RPC only). */
  vote?: string;
}

/**
 * getpeginfo of the node: what the mainchain node says of the peg, the same over JSON-RPC (getdrivechaininfo,
 * getsidechain, listwithdrawalbundles) and through the enforcer's interface (GetChainInfo, GetSidechains, GetCtip,
 * GetWithdrawalBundleProposals...). A field a way does not report is left out.
 */
export interface PegInfo {
  via: MainVia;
  height: number;
  rules: 'chains' | 'enforcer';
  /** Blocks a bundle has to collect its votes. */
  withdrawalperiod?: number;
  /** Votes a bundle needs to be paid out. */
  withdrawalminscore?: number;
  activesidechains?: number;
  proposals?: number;
  sidechain: { slot: number; active: boolean; activationheight?: number; proposalhash?: string; title?: string };
  escrow?: { txid: string; n: number; amount: number };
  bundles: MainBundle[];
  /** Whether the votes of each past block are reported (getsidechainevents' scores): not through the enforcer's interface. */
  votehistory: boolean;
}

/** getdrivechaininfo of a mainchain node over JSON-RPC (a node before getpeginfo: the pages ask it themselves). */
interface LegacyDrivechainInfo {
  withdrawalperiod?: number;
  withdrawalminscore?: number;
  rules?: string;
  bundlethreshold?: number;
  bundlemaxage?: number;
  height?: number;
  activesidechains?: number;
  proposals?: number;
}

/**
 * getpeginfo made from getdrivechaininfo and listwithdrawalbundles, for a node that does not have it, over JSON-RPC
 * (the enforcer's interface has neither: such a node is not asked). As the node makes it (PegInfoFromRpc).
 */
export function pegInfoFromLegacy(info: LegacyDrivechainInfo | null, bundles: unknown, slot: number, via: MainVia = 'json-rpc'): PegInfo {
  const d = info && typeof info === 'object' ? info : {};
  const enforcerRules = d.rules === 'enforcer';
  const num = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
  const threshold = num(d.bundlethreshold);
  const list = Array.isArray(bundles) ? (bundles as Record<string, unknown>[]) : [];
  return {
    via,
    height: num(d.height) ?? -1,
    rules: enforcerRules ? 'enforcer' : 'chains',
    withdrawalperiod: enforcerRules ? num(d.bundlemaxage) : num(d.withdrawalperiod),
    withdrawalminscore: enforcerRules ? (threshold === undefined ? undefined : threshold + 1) : num(d.withdrawalminscore),
    activesidechains: num(d.activesidechains),
    proposals: num(d.proposals),
    sidechain: { slot, active: true },
    bundles: list
      .filter((b) => b && typeof b.hash === 'string' && (b.slot === undefined || b.slot === slot))
      .map((b) => ({
        hash: b.hash as string,
        height: num(b.height) ?? -1,
        score: num(b.score) ?? 0,
        blocksleft: num(b.blocksleft) === undefined ? undefined : Math.max(0, b.blocksleft as number),
        payable: typeof b.payable === 'boolean' ? b.payable : undefined,
        vote: typeof b.vote === 'string' ? b.vote : undefined,
      })),
    votehistory: true,
  };
}

/** Whether an error is "no such method" (a node of before a method). */
export function methodMissing(e: unknown): boolean {
  return !!e && typeof e === 'object' && ((e as { code?: number }).code === -32601 || /method not found/i.test(String((e as { message?: string }).message ?? '')));
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
  /**
   * Its score in the last block on record while it was pending (its votes now, if it still is); not known for a
   * bundle closed when the mainchain node does not report the votes of past blocks (the enforcer's interface).
   */
  score?: number;
}

/**
 * The bundles of this sidechain the events saw, the newest first. `current`: the bundles being voted on now
 * (getpeginfo), whose votes now are their score; `votehistory` false: the events have no scores (the enforcer's
 * interface), so a closed bundle's is not known.
 */
export function bundleHistory(events: readonly SideEvent[], current: readonly Pick<MainBundle, 'hash' | 'score'>[] = [], votehistory = true): BundleHistory[] {
  const byHash = new Map<string, BundleHistory>();
  const get = (hash: string) => {
    let b = byHash.get(hash);
    if (!b) byHash.set(hash, (b = votehistory ? { hash, score: 0 } : { hash }));
    return b;
  };
  for (const e of events) {
    for (const hash of e.proposed) Object.assign(get(hash), { proposed: e.height, proposedTime: e.time });
    for (const p of e.pending) get(p.hash).score = p.score;
    for (const c of e.bundles) Object.assign(get(c.hash), { closed: e.height, closedTime: e.time, paid: c.paid });
  }
  for (const b of current) {
    const h = byHash.get(b.hash);
    if (h && h.closed === undefined) h.score = b.score;
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

/** Whether a bundle can still pass: payable, or enough blocks left to reach the score (not known: it can). */
export function bundleCanPass(b: Pick<MainBundle, 'payable' | 'score' | 'blocksleft'>, minScore: number): boolean {
  if (b.payable ?? (minScore > 0 && b.score >= minScore)) return true;
  return b.blocksleft === undefined || b.score + b.blocksleft >= minScore;
}

/** Whether a bundle has the votes it needs (as the mainchain node says, or from the votes it needs). */
export function bundlePayable(b: Pick<MainBundle, 'payable' | 'score'>, minScore: number): boolean {
  return b.payable ?? (minScore > 0 && b.score >= minScore);
}

