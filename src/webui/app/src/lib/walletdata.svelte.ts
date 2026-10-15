// What the wallet pages show of the current wallet, loaded once and shared: its info, its history,
// its address book.
//
// The history is loaded whole once per wallet (as TransactionTableModel does at start), then kept up to
// date in parts, as TransactionTableModel::updateTransaction does: an event names a transaction, and only
// its records are asked for again. With each block the status of every record (its confirmations, its
// maturity) is brought up to date here, from the height of its block; only the transactions that are not
// in a block are asked for again, as whether the wallet trusts them depends on the host. A wallet of a
// hundred thousand transactions is not sent again for each block or each transaction.
import { on } from './bridge';
import { node, wallets } from './state.svelte';
import { parseRecords, withStatus, walletApi, type AddressEntry, type TxRecord, type WalletInfo } from './wallet';

class Current {
  info = $state<WalletInfo | null>(null);
  /** The history. Raw state: a long array of plain records, replaced as a whole when it changes. */
  records = $state.raw<TxRecord[]>([]);
  recordsLoaded = $state(false);
  recordsError = $state('');
  addresses = $state.raw<AddressEntry[]>([]);
  addressesLoaded = $state(false);
}

export const current = new Current();

interface Loaded {
  wallet: string;
  version: string;
}

let infoFor: Loaded | null = null;
let addressesFor: Loaded | null = null;

/** The wallet whose whole history is in current.records, and what the status of its records follows. */
let historyOf: string | null = null;
let walletHeight = 0;
let maturity = 0;
let fullLoad: Promise<void> | null = null;
/** Transactions an event named since the last update, by wallet. */
const changed = new Map<string, Set<string>>();
let updating: Promise<void> | null = null;
let updateAgain = false;
let lastTip = '';
/** More changed transactions than this load the history whole again (a rescan). */
const FULL_RELOAD_AT = 2000;

/** The client of the current wallet; null without one. */
export function api() {
  return wallets.current === null ? null : walletApi(wallets.current);
}

/** The info of the current wallet (locked, keys, address types...). */
export async function refreshInfo(force = false): Promise<WalletInfo | null> {
  const name = wallets.current;
  if (name === null) {
    current.info = null;
    return null;
  }
  const w = wallets.list.find((x) => x.name === name);
  const version = `${w?.locked}:${w?.encrypted}:${wallets.txVersion}`;
  if (!force && infoFor && infoFor.wallet === name && infoFor.version === version && current.info) return current.info;
  infoFor = { wallet: name, version };
  try {
    const info = await walletApi(name).info();
    if (wallets.current === name) current.info = info;
    return info;
  } catch {
    return null;
  }
}

function loadAll(name: string): Promise<void> {
  if (fullLoad && historyOf === name) return fullLoad;
  if (historyOf !== name) {
    current.records = [];
    current.recordsLoaded = false;
  }
  historyOf = name;
  changed.delete(name);
  const run = (async () => {
    try {
      const r = await walletApi(name).transactions();
      if (wallets.current !== name || historyOf !== name) return;
      walletHeight = r.height;
      maturity = r.maturity;
      lastTip = node.tip?.hash ?? '';
      current.records = r.records;
      current.recordsLoaded = true;
      current.recordsError = '';
    } catch (e) {
      if (wallets.current === name) current.recordsError = e instanceof Error ? e.message : String(e);
    }
  })();
  const settled: Promise<void> = run.finally(() => {
    if (fullLoad === settled) fullLoad = null;
  });
  fullLoad = settled;
  return settled;
}

/** The records of these transactions put in place of the ones held, and the status of all brought up to date. */
export function mergeRecords(records: readonly TxRecord[], fresh: readonly TxRecord[], txids: ReadonlySet<string>, height: number, mature: number): TxRecord[] {
  const out: TxRecord[] = [];
  for (const r of records) if (!txids.has(r.txid)) out.push(withStatus(r, height, mature));
  for (const r of fresh) out.push(withStatus(r, height, mature));
  return out;
}

async function update(name: string): Promise<void> {
  const named = changed.get(name) ?? new Set<string>();
  changed.delete(name);
  // Transactions out of a block: whether the wallet trusts them (and spends their change) is the host's to say.
  const ask = new Set(named);
  for (const r of current.records) if (r.state === 'u' && !r.coinbase) ask.add(r.txid);
  if (ask.size > FULL_RELOAD_AT) {
    historyOf = null;
    return loadAll(name);
  }
  try {
    const r = await walletApi(name).transactionsOf([...ask]);
    if (wallets.current !== name || historyOf !== name || fullLoad) return;
    walletHeight = r.height;
    maturity = r.maturity || maturity;
    current.records = mergeRecords(current.records, r.records, ask, walletHeight, maturity);
    current.recordsError = '';
  } catch (e) {
    // Asked again with the next change.
    for (const txid of named) noteChanged(name, txid);
    if (wallets.current === name) current.recordsError = e instanceof Error ? e.message : String(e);
  }
}

function scheduleUpdate(name: string): Promise<void> {
  if (updating) {
    updateAgain = true;
    return updating;
  }
  const run = (async () => {
    do {
      updateAgain = false;
      await update(name);
    } while (updateAgain && wallets.current === name);
  })();
  updating = run.finally(() => {
    updating = null;
  });
  return updating;
}

function noteChanged(name: string, txid: string): void {
  let set = changed.get(name);
  if (!set) changed.set(name, (set = new Set()));
  set.add(txid);
}

/**
 * The history of the current wallet: loaded whole the first time (or with `force`), then brought up to
 * date with what changed since (the transactions events named, the status after new blocks).
 */
export function refreshRecords(force = false): Promise<void> {
  const name = wallets.current;
  if (name === null) {
    current.records = [];
    current.recordsLoaded = false;
    historyOf = null;
    return Promise.resolve();
  }
  if (force && historyOf === name && current.recordsLoaded && !fullLoad) {
    // A transaction of this wallet changed (abandoned, bumped): the part that changed is enough.
    return scheduleUpdate(name);
  }
  if (historyOf !== name || (!current.recordsLoaded && !fullLoad)) return loadAll(name);
  if (fullLoad) return fullLoad;
  const tip = node.tip?.hash ?? '';
  if ((changed.get(name)?.size ?? 0) === 0 && tip === lastTip) return updating ?? Promise.resolve();
  lastTip = tip;
  return scheduleUpdate(name);
}

/** The records of some transactions of the current wallet, asked for now (a notice of a new one). */
export async function recordsOf(txids: string[]): Promise<TxRecord[]> {
  const name = wallets.current;
  if (name === null || txids.length === 0) return [];
  const r = await walletApi(name).transactionsOf(txids);
  return r.records.map((x) => withStatus(x, r.height, r.maturity));
}

/** The address book of the current wallet. */
export async function refreshAddresses(force = false): Promise<void> {
  const name = wallets.current;
  if (name === null) {
    current.addresses = [];
    return;
  }
  const version = `${wallets.addressVersion}:${wallets.txVersion}`;
  if (!force && addressesFor && addressesFor.wallet === name && addressesFor.version === version) return;
  addressesFor = { wallet: name, version };
  try {
    const list = await walletApi(name).addresses();
    if (wallets.current === name) {
      current.addresses = list;
      current.addressesLoaded = true;
    }
  } catch {
    // The wallet went away; the next event says so.
  }
}

/** The label of an address in the book, or ''. */
export function labelOf(address: string): string {
  return current.addresses.find((a) => a.address === address)?.label ?? '';
}

/** A label changed in the address book: the records of that address show it at once. */
export function relabel(records: readonly TxRecord[], address: string, label: string): TxRecord[] {
  let touched = false;
  const out = records.map((r) => {
    if (r.address !== address || r.label === label || (r.type !== 'recv' && r.type !== 'send' && r.type !== 'mined')) return r;
    touched = true;
    return { ...r, label };
  });
  return touched ? out : (records as TxRecord[]);
}

// Events of the wallets: what changed is noted, and asked for when a page shows the history.
on<{ name: string; txid: string }>('wallet.tx', ({ name, txid }) => {
  if (name === historyOf) noteChanged(name, txid);
});
on<{ name: string; address: string; label: string; change: string }>('wallet.addressbook', ({ name, address, label, change }) => {
  if (name !== historyOf || !current.recordsLoaded) return;
  current.records = relabel(current.records, address, change === 'deleted' ? '' : label);
});
on<{ name: string }>('wallet.unloaded', ({ name }) => {
  changed.delete(name);
  if (name === historyOf) {
    historyOf = null;
    current.records = [];
    current.recordsLoaded = false;
  }
});

/** Forget what was loaded (tests, a wallet closed). */
export function resetWalletData(): void {
  infoFor = addressesFor = null;
  historyOf = null;
  fullLoad = null;
  updating = null;
  changed.clear();
  lastTip = '';
  current.info = null;
  current.records = [];
  current.recordsLoaded = false;
  current.recordsError = '';
  current.addresses = [];
  current.addressesLoaded = false;
}

/** For the tests: the height the status of the records follows. */
export function historyState(): { wallet: string | null; height: number; maturity: number } {
  return { wallet: historyOf, height: walletHeight, maturity };
}
