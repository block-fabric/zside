// The state of the shielded account of the current wallet, kept up to date while the app runs: its
// balance, its transactions, its notes, its melt/cast plan and its default address, and the pool of the
// chain; asked again with every block, every transaction event of the wallet, and every few seconds, one
// round at a time.
//
// And what the chain does not keep, for this viewer only (browser storage, a convenience that may be lost):
// which memos were read, and how long the proofs took (the estimate of the next one).
import { BridgeError } from '../../lib/bridge';
import { every } from '../../lib/poll';
import { rpc } from '../../lib/rpc';
import { node, wallets } from '../../lib/state.svelte';
import { WalletError } from '../../lib/wallet';
import { memosOf, proofEstimate, type NoteRaw, type PlanStep, type PoolInfo, type ShieldedBalanceRaw, type ShieldedTxRaw } from './model';

/** How many transactions of the account the pages show (listshieldedtransactions). */
export const HISTORY_COUNT = 500;

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
    rpc.raw<ShieldedTxRaw[]>('listshieldedtransactions', [HISTORY_COUNT], opts),
    rpc.raw<NoteRaw[]>('listshieldednotes', [true], opts),
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
  readKeys.clear();
  readLoadedFor = null;
}

// --- The memos read (this viewer's convenience) ---

const READ_KEY = 'zside.memos.read';
const readKeys = new Set<string>();
let readLoadedFor: string | null = null;
export const memoState = $state({ version: 0 });

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
    // Not kept: the memos show as unread again next time.
  }
}

function loadRead(wallet: string): void {
  if (readLoadedFor === wallet) return;
  readLoadedFor = wallet;
  readKeys.clear();
  try {
    const list = JSON.parse(storageGet(`${READ_KEY}.${wallet}`) ?? '[]');
    if (Array.isArray(list)) for (const k of list) if (typeof k === 'string') readKeys.add(k);
  } catch {
    // A list that cannot be read: none read.
  }
}

export function isRead(key: string): boolean {
  void memoState.version;
  if (zs.wallet === null) return true;
  loadRead(zs.wallet);
  return readKeys.has(key);
}

export function markRead(keys: string[], read = true): void {
  if (zs.wallet === null) return;
  loadRead(zs.wallet);
  for (const k of keys) {
    if (read) readKeys.add(k);
    else readKeys.delete(k);
  }
  // The newest 4000 kept.
  const list = [...readKeys].slice(-4000);
  storageSet(`${READ_KEY}.${zs.wallet}`, JSON.stringify(list));
  memoState.version++;
}

/** How many memos received are not read yet. */
export function unreadCount(): number {
  void memoState.version;
  return memosOf(zs.txs).filter((m) => !isRead(m.key)).length;
}

// --- How long proofs take (this viewer's convenience) ---

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
