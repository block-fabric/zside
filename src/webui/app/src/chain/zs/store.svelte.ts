// The state of the shielded account of the current wallet, kept up to date while the app runs: its
// balance, its transactions, its notes, its melt/cast plan and its default address, and the pool of the
// chain; asked again with every block, every transaction event of the wallet, and every few seconds, one
// round at a time.
//
// And what the chain does not keep: which memos were read (in the app's preferences, one set per network,
// per wallet: audit port-2 #10), and how long the proofs took (browser storage, a convenience that may be lost).
import { BridgeError } from '../../lib/bridge';
import { every } from '../../lib/poll';
import { host, rpc } from '../../lib/rpc';
import { node, wallets } from '../../lib/state.svelte';
import { WalletError } from '../../lib/wallet';
import { memosOf, proofEstimate, type NoteRaw, type PlanStep, type PoolInfo, type ShieldedBalanceRaw, type ShieldedTxRaw } from './model';

/** How many transactions of the account the pages read at first (listshieldedtransactions). */
export const HISTORY_COUNT = 500;
/** How many the user can choose to read (user decision, 2026-10-15); 0 is all of them. */
export const HISTORY_POINTS = [25, 50, 100, 250, 500, 1000, 10000, 25000, 0] as const;
/** The count asked of the node for "all" (listshieldedtransactions takes a count). */
const ALL_HISTORY = 2147483647;

/**
 * What the pages ask of the account beyond the usual (audit port-2 #10): more of its history ("Show older",
 * HISTORY_COUNT more each time), and its spent notes while a page shows them (listshieldednotes is otherwise
 * asked for the unspent ones only, which do not grow without bound).
 */
export const zsView = $state({ historyCount: HISTORY_COUNT, spent: false });

/** Whether the history read may have left older transactions out (it holds as many as were asked for). */
export function historyCut(): boolean {
  return zsView.historyCount !== 0 && zs.txs.length >= zsView.historyCount;
}

/** Read this many of the newest transactions (0: all of them). */
export function setHistoryCount(count: number): void {
  if (zsView.historyCount === count) return;
  zsView.historyCount = count;
  void refreshZs();
}

/** Read more of the history: the next of HISTORY_POINTS above what is read now (all, at the end). */
export function showOlder(): void {
  setHistoryCount(HISTORY_POINTS.find((n) => n === 0 || n > zsView.historyCount) ?? 0);
}

/** The spent notes too, while a page shows them. */
export function setSpentNotes(spent: boolean): void {
  if (zsView.spent === spent) return;
  zsView.spent = spent;
  void refreshZs();
}

export const zs = $state({
  /** The wallet the rest is of. */
  wallet: null as string | null,
  loaded: false,
  /** Why the shielded account cannot be read (a locked wallet whose account is not set up yet: unlockNeeded). */
  error: '',
  unlockNeeded: false,
  balance: null as ShieldedBalanceRaw | null,
  /** The transparent balance of the wallet: trusted, waiting, immature (getbalances "mine"), in coins. */
  transparent: null as { trusted: number; pending: number; immature: number } | null,
  pool: null as PoolInfo | null,
  txs: [] as ShieldedTxRaw[],
  notes: [] as NoteRaw[],
  plan: [] as PlanStep[],
  /** The default shielded address of the account ('' when it has none: no keys, or not set up). */
  address: '',
  /** Bumped each time the lists change, for the pages that keep a selection. */
  version: 0,
});

let timer: (() => void) | null = null;
let lastTip = '';
let lastWallet: string | null | undefined = undefined;
let lastTx = -1;
let running: Promise<void> | null = null;
let again = false;

function sameJson(a: unknown, b: unknown): boolean {
  return JSON.stringify(a) === JSON.stringify(b);
}

async function round(): Promise<void> {
  const wallet = wallets.current;
  // The pool is of the chain: read with or without a wallet.
  const pool = rpc.raw<PoolInfo>('getshieldedinfo', []).catch(() => null);
  if (wallet === null) {
    Object.assign(zs, { wallet: null, loaded: true, error: '', unlockNeeded: false, balance: null, transparent: null, txs: [], notes: [], plan: [], address: '' });
    zs.pool = await pool;
    return;
  }
  const opts = { wallet };
  const [balance, txs, notes, plan, address, balances, poolInfo] = await Promise.allSettled([
    rpc.raw<ShieldedBalanceRaw>('getshieldedbalance', [], opts),
    rpc.raw<ShieldedTxRaw[]>('listshieldedtransactions', [zsView.historyCount === 0 ? ALL_HISTORY : zsView.historyCount], opts),
    rpc.raw<NoteRaw[]>('listshieldednotes', [zsView.spent], opts),
    rpc.raw<PlanStep[]>('listmeltcast', [], opts),
    zs.address && zs.wallet === wallet ? Promise.resolve(zs.address) : rpc.raw<string>('getshieldedaddress', [], opts),
    rpc.raw<{ mine: { trusted: number; untrusted_pending: number; immature: number } }>('getbalances', [], opts),
    pool,
  ]);
  // Another wallet chosen meanwhile: this round is of no use.
  if (wallets.current !== wallet) return;
  if (zs.wallet !== wallet) {
    zs.address = '';
    zs.wallet = wallet;
  }
  zs.pool = poolInfo.status === 'fulfilled' ? poolInfo.value : null;
  if (balances.status === 'fulfilled') {
    const m = balances.value.mine;
    zs.transparent = { trusted: m.trusted, pending: m.untrusted_pending, immature: m.immature };
  }
  if (balance.status === 'rejected') {
    const e = BridgeError.from(balance.reason);
    zs.unlockNeeded = e.code === WalletError.UNLOCK_NEEDED;
    zs.error = e.message;
    zs.balance = null;
    zs.txs = [];
    zs.notes = [];
    zs.plan = [];
    zs.loaded = true;
    return;
  }
  zs.error = '';
  zs.unlockNeeded = false;
  if (!sameJson(zs.balance, balance.value)) zs.balance = balance.value;
  let changed = false;
  if (txs.status === 'fulfilled' && !sameJson(zs.txs, txs.value)) {
    zs.txs = txs.value;
    changed = true;
  }
  if (notes.status === 'fulfilled' && !sameJson(zs.notes, notes.value)) {
    zs.notes = notes.value;
    changed = true;
  }
  if (plan.status === 'fulfilled' && !sameJson(zs.plan, plan.value)) {
    zs.plan = plan.value;
    changed = true;
  }
  // No address: a wallet without keys of its own (a viewing key watched), or one not checked against this chain yet.
  zs.address = address.status === 'fulfilled' ? address.value : '';
  if (changed) zs.version++;
  zs.loaded = true;
}

/** Read it all again (after a payment, say); one round at a time, one asked for meanwhile follows. */
export function refreshZs(): Promise<void> {
  if (running) {
    again = true;
    return running;
  }
  running = round()
    .catch(() => {})
    .finally(() => {
      running = null;
      if (again) {
        again = false;
        void refreshZs();
      }
    });
  return running;
}

/** Keep it up to date: with every block, every transaction of the wallet, and every 5 seconds. */
export function startZs(): void {
  const tick = () => {
    const tip = node.tip?.hash ?? '';
    if (tip !== lastTip || wallets.current !== lastWallet || wallets.txVersion !== lastTx) {
      lastTip = tip;
      lastWallet = wallets.current;
      lastTx = wallets.txVersion;
      void refreshZs();
    }
  };
  tick();
  if (timer) return;
  let ticks = 0;
  timer = every(1000, () => {
    tick();
    if (++ticks % 5 === 0) void refreshZs();
  });
}

/** For the tests. */
export function resetZs(): void {
  timer?.();
  timer = null;
  lastTip = '';
  lastWallet = undefined;
  lastTx = -1;
  running = null;
  again = false;
  Object.assign(zs, { wallet: null, loaded: false, error: '', unlockNeeded: false, balance: null, transparent: null, pool: null, txs: [], notes: [], plan: [], address: '', version: 0 });
  Object.assign(zsView, { historyCount: HISTORY_COUNT, spent: false });
  readKeys.clear();
  readLoadedFor = null;
  readStore = null;
  readLoading = null;
}

// --- The memos read ---
//
// In the app's preferences (prefs.get / prefs.set: the host keeps one set per network, so the same wallet
// name on main, test and regtest does not share them), under READ_PREF: per wallet, the newest READ_KEEP
// memos read, each by a short key (16 hex digits of the transaction and the index: a few bytes each, the
// preferences being capped). Marks kept in browser storage by an older version are moved there once.

export const READ_PREF = 'zsideMemosRead';
/** Memos read kept per wallet, and wallets kept (the newest marks of the wallets used last). */
export const READ_KEEP = 1000;
export const READ_WALLETS = 6;
const OLD_READ_KEY = 'zside.memos.read';
const readKeys = new Set<string>();
let readLoadedFor: string | null = null;
let readStore: Record<string, string[]> | null = null;
let readLoading: Promise<void> | null = null;
export const memoState = $state({ version: 0 });

/** The short key a mark is kept by. */
export function readKeyOf(key: string): string {
  const at = key.lastIndexOf(':');
  return at < 0 ? key.slice(0, 16) : `${key.slice(0, Math.min(16, at))}:${key.slice(at + 1)}`;
}

function storageGet(key: string): string | null {
  try {
    return localStorage.getItem(key);
  } catch {
    return null;
  }
}

function storageSet(key: string, value: string): void {
  try {
    localStorage.setItem(key, value);
  } catch {
    // Not kept: a convenience.
  }
}

function storageRemove(key: string): void {
  try {
    localStorage.removeItem(key);
  } catch {
    // Nothing to do.
  }
}

function loadStore(): Promise<void> {
  readLoading ??= host
    .prefs()
    .then((p) => {
      const raw = p[READ_PREF];
      const store: Record<string, string[]> = {};
      if (raw && typeof raw === 'object' && !Array.isArray(raw)) {
        for (const [w, list] of Object.entries(raw as Record<string, unknown>)) {
          if (Array.isArray(list)) store[w] = list.filter((k): k is string => typeof k === 'string').slice(-READ_KEEP);
        }
      }
      readStore = store;
    })
    .catch(() => {
      readStore = {};
    })
    .then(() => {
      readLoadedFor = null;
      memoState.version++;
    });
  return readLoading;
}

function loadRead(wallet: string): void {
  if (readStore === null) {
    void loadStore();
    return;
  }
  if (readLoadedFor === wallet) return;
  readLoadedFor = wallet;
  readKeys.clear();
  for (const k of readStore[wallet] ?? []) readKeys.add(k);
  // Marks of an older version, in browser storage: moved to the preferences.
  const old = storageGet(`${OLD_READ_KEY}.${wallet}`);
  if (old !== null) {
    try {
      const list = JSON.parse(old);
      if (Array.isArray(list)) for (const k of list) if (typeof k === 'string') readKeys.add(readKeyOf(k));
    } catch {
      // A list that cannot be read: none read.
    }
    storageRemove(`${OLD_READ_KEY}.${wallet}`);
    saveRead(wallet);
  }
}

function saveRead(wallet: string): void {
  if (readStore === null) return;
  const store = { ...readStore };
  delete store[wallet];
  // The wallet saved last goes last: the oldest wallets are let go first.
  store[wallet] = [...readKeys].slice(-READ_KEEP);
  const wallets = Object.keys(store);
  for (const w of wallets.slice(0, Math.max(0, wallets.length - READ_WALLETS))) delete store[w];
  readStore = store;
  host.setPrefs({ [READ_PREF]: store }).catch(() => undefined);
}

export function isRead(key: string): boolean {
  void memoState.version;
  if (zs.wallet === null) return true;
  // Until the preferences are read, nothing shows as unread.
  if (readStore === null) {
    void loadStore();
    return true;
  }
  loadRead(zs.wallet);
  return readKeys.has(readKeyOf(key));
}

export function markRead(keys: string[], read = true): void {
  if (zs.wallet === null) return;
  if (readStore === null) {
    void loadStore().then(() => markRead(keys, read));
    return;
  }
  loadRead(zs.wallet);
  for (const k of keys) {
    const short = readKeyOf(k);
    readKeys.delete(short);
    if (read) readKeys.add(short);
  }
  saveRead(zs.wallet);
  memoState.version++;
}

/** How many memos received are not read yet. */
export function unreadCount(): number {
  void memoState.version;
  return memosOf(zs.txs).filter((m) => !isRead(m.key)).length;
}

// --- How long proofs take (this viewer's convenience, in browser storage) ---

const PROOF_KEY = 'zside.proofs';

function proofTimes(): Record<string, number[]> {
  try {
    const v = JSON.parse(storageGet(PROOF_KEY) ?? '{}');
    return v && typeof v === 'object' ? v : {};
  } catch {
    return {};
  }
}

export function estimateFor(kind: string): number {
  return proofEstimate(proofTimes()[kind] ?? []);
}

export function recordProof(kind: string, ms: number): void {
  const all = proofTimes();
  all[kind] = [...(all[kind] ?? []), Math.round(ms)].slice(-8);
  storageSet(PROOF_KEY, JSON.stringify(all));
}

// --- A proof under way ---

export interface Proving {
  kind: string;
  /** What it is doing, in the app's words (translated where it is shown). */
  label: string;
  started: number;
  estimate: number;
}

export const proving = $state({ now: null as Proving | null });

/**
 * Run a call that proves (a payment, a shielding, an unshielding, a fee bump): the node proves it on a
 * lane of its own, off the UI thread, and the page shows how far along it seems meanwhile.
 */
export async function prove<T>(kind: string, label: string, run: () => Promise<T>): Promise<T> {
  const started = Date.now();
  proving.now = { kind, label, started, estimate: estimateFor(kind) };
  try {
    const result = await run();
    // A cancelled call (null: the passphrase not given) proved nothing.
    if (result !== null) recordProof(kind, Date.now() - started);
    return result;
  } finally {
    proving.now = null;
    void refreshZs();
  }
}
