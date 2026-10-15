// The wallet functions of the host (src/webui/walletapi.h), typed, and what the wallet pages of
// chains-qt compute from them: the texts of the transaction table, its filters, the CSV export.
import { BridgeError, request } from './bridge';
import { t } from './i18n.svelte';
import { formatAmount, type Unit } from './units';

/** Error codes of the wallet functions (walletapi.h). */
export const WalletError = {
  UNLOCK_NEEDED: -13,
  PASSPHRASE_INCORRECT: -14,
  NOT_FOUND: -18,
  REFUSED: -6,
} as const;

function call<T>(method: string, params: Record<string, unknown> = {}): Promise<T> {
  return request<T>('host', method, params);
}

const n = (v: unknown): bigint => (typeof v === 'number' ? BigInt(Math.round(v)) : typeof v === 'bigint' ? v : 0n);

// --- Wallets ---

export interface WalletDirEntry {
  name: string;
  format: string;
  loaded: boolean;
  needsMigration: boolean;
}

export interface WalletInfo {
  name: string;
  encrypted: boolean;
  locked: boolean;
  privateKeysDisabled: boolean;
  externalSigner: boolean;
  /** The path of a signer script is set (-signer): a hardware wallet can sign. */
  signerConfigured?: boolean;
  hd: boolean;
  canGetAddresses: boolean;
  taproot: boolean;
  defaultAddressType: string;
  confirmTarget: number;
  requiredFeePerK: number;
  maxTxFee: number;
  multiwallet: boolean;
  balance: number;
  unconfirmed: number;
  immature: number;
}

export interface MigrateResult {
  name: string;
  watchonlyName?: string;
  solvablesName?: string;
  backup: string;
  loaded: boolean;
  warnings?: string[];
}

export const wallets = {
  list: () => call<WalletDirEntry[]>('wallets.list'),
  signers: () => call<{ signers: { name: string }[]; error?: string; supported: boolean }>('wallets.signers'),
  create: (p: { name: string; passphrase?: string; disablePrivateKeys?: boolean; blank?: boolean; externalSigner?: boolean }) =>
    call<{ name: string; warnings: string[] }>('wallets.create', p),
  open: (name: string) => call<{ name: string; warnings: string[] }>('wallets.open', { name }),
  close: (name: string) => call<boolean>('wallets.close', { name }),
  restore: (file: string, name: string) => call<{ name: string; warnings: string[] }>('wallets.restore', { file, name }),
  migrate: (name: string, passphrase: string | undefined, load: boolean) => call<MigrateResult>('wallets.migrate', { name, passphrase, load }),
  restoreMigrate: (file: string, name: string, passphrase?: string) => call<MigrateResult>('wallets.restoreMigrate', { file, name, passphrase }),
  isEncrypted: (name: string) => call<boolean>('wallets.isEncrypted', { name }),
};

// --- One wallet ---

export interface Recipient {
  address: string;
  label?: string;
  amount: bigint;
  subtractFee?: boolean;
  message?: string;
}

export interface Outpoint {
  txid: string;
  vout: number;
}

export interface FeeChoice {
  /** Blocks to confirm in, for the smart fee. */
  confTarget: number;
  /** A custom fee rate, in satoshis per 1000 vbytes; none for the smart fee. */
  feeRate?: number;
}

export interface CoinControl {
  selected?: Outpoint[];
  changeAddress?: string;
}

export interface Prepared {
  token: string;
  fee: bigint;
  vsize: number;
  total: bigint;
  recipients: { address: string; label: string; amount: bigint }[];
  privateKeysDisabled: boolean;
  externalSigner: boolean;
  txid: string;
}

export interface Coin extends Outpoint {
  address: string;
  label: string;
  change: boolean;
  sats: bigint;
  time: number;
  depth: number;
  locked: boolean;
}

export interface CoinGroup {
  address: string;
  label: string;
  coins: Coin[];
}

export interface CoinSummary {
  quantity: number;
  amount: bigint;
  fee: bigint;
  afterFee: bigint;
  bytes: number;
  change: bigint;
  changeEnabled: boolean;
  insufficient: boolean;
  feeVary: number;
  spent: Outpoint[];
}

export interface AddressEntry {
  address: string;
  label: string;
  purpose: 'send' | 'receive';
  mine: boolean;
}

export interface ReceiveRequest {
  id: number;
  time: number;
  address: string;
  label: string;
  amount: bigint;
  message: string;
}

export interface DetailRow {
  k: string;
  text?: string;
  time?: number;
  sats?: number;
  note?: string;
  plus?: boolean;
  address?: string;
  label?: string;
  own?: boolean;
}

export interface TxDetails {
  txid: string;
  coinbase: boolean;
  rows: DetailRow[];
  notes: DetailRow[];
  ids: DetailRow[];
  messages: string[];
  coinbaseNote?: string;
  debug?: { amounts: DetailRow[]; transaction: string; inputs: { address?: string; label?: string; sats: number; mine: boolean }[] };
}

export interface PsbtState {
  psbt: string;
  complete: boolean;
  canSign: boolean;
  canBroadcast: boolean;
  outputs: { address: string; sats: number; mine: boolean }[];
  total: number;
  fee?: number;
  unsignedInputs: number;
  next: string;
  status: { text: string; level: 'info' | 'warning' | 'error' | 'success' };
  signed?: number;
}

function withPass(params: Record<string, unknown>, passphrase?: string): Record<string, unknown> {
  return passphrase ? { ...params, passphrase } : params;
}

function outpoints(list?: Outpoint[]) {
  return list?.map((o) => ({ txid: o.txid, vout: o.vout }));
}

export function walletApi(wallet: string) {
  const w = { wallet };
  return {
    info: () => call<WalletInfo>('wallet.info', w),
    encrypt: (passphrase: string) => call<boolean>('wallet.encrypt', { ...w, passphrase }),
    changePassphrase: (old: string, next: string) => call<boolean>('wallet.changePassphrase', { ...w, old, new: next }),
    unlock: (passphrase: string) => call<boolean>('wallet.unlock', { ...w, passphrase }),
    lock: () => call<boolean>('wallet.lock', w),
    backup: (file: string) => call<string>('wallet.backup', { ...w, file }),
    exportWatchOnly: (file: string) => call<string>('wallet.exportWatchOnly', { ...w, file }),
    /** The whole history. */
    transactions: async () => {
      const raw = await call<{ height: number; hash: string; maturity?: number; records: unknown[][] }>('wallet.transactions', w);
      return { height: raw.height, hash: raw.hash, maturity: raw.maturity ?? 0, records: parseRecords(raw.records) };
    },
    /** The records of some transactions, and the ids of those no longer in the wallet. */
    transactionsOf: async (txids: string[]) => {
      const raw = await call<{ height: number; hash: string; maturity?: number; records: unknown[][]; gone?: string[] }>('wallet.transactions', { ...w, txids });
      return { height: raw.height, hash: raw.hash, maturity: raw.maturity ?? 0, records: parseRecords(raw.records), gone: raw.gone ?? [] };
    },
    /** At least `count` of the newest records (the overview), newest first. */
    latest: async (count: number) => {
      const raw = await call<{ height: number; maturity?: number; records: unknown[][] }>('wallet.transactions', { ...w, latest: count });
      return sortNewestFirst(parseRecords(raw.records)).slice(0, count);
    },
    /** The balance after each transaction, oldest first, at most `points` of them (the chart of the overview). */
    balanceHistory: async (points = 400) => (await call<[number, number][]>('wallet.balanceHistory', { ...w, points })).map(([time, balance]) => ({ time, balance: n(balance) })),
    transaction: (txid: string, idx: number, address: string) => call<TxDetails>('wallet.transaction', { ...w, txid, idx, address }),
    txActions: (txid: string) => call<{ canAbandon: boolean; canBump: boolean; hex: string }>('wallet.txActions', { ...w, txid }),
    abandon: (txid: string) => call<boolean>('wallet.abandon', { ...w, txid }),
    bumpPrepare: async (txid: string) => {
      const r = await call<{ token: string; oldFee: number; newFee: number; canSend: boolean }>('wallet.bump.prepare', { ...w, txid });
      return { token: r.token, oldFee: n(r.oldFee), newFee: n(r.newFee), canSend: r.canSend };
    },
    bumpCommit: (token: string, passphrase?: string) => call<{ txid: string }>('wallet.bump.commit', withPass({ ...w, token }, passphrase)),
    bumpPsbt: (token: string) => call<{ psbt: string }>('wallet.bump.psbt', { ...w, token }),
    addresses: () => call<AddressEntry[]>('wallet.addresses', w),
    addSending: (address: string, label: string) => call<string>('wallet.addressBook.add', { ...w, address, label }),
    editAddress: (address: string, label: string, newAddress?: string) =>
      call<{ address: string; changed: boolean }>('wallet.addressBook.edit', { ...w, address, label, ...(newAddress ? { newAddress } : {}) }),
    deleteAddress: (address: string) => call<boolean>('wallet.addressBook.delete', { ...w, address }),
    newAddress: (label: string, type: string, passphrase?: string) => call<string>('wallet.newAddress', withPass({ ...w, label, type }, passphrase)),
    displayAddress: (address: string) => call<boolean>('wallet.displayAddress', { ...w, address }),
    requests: async () => (await call<(Omit<ReceiveRequest, 'amount'> & { amount: number })[]>('wallet.requests', w)).map((r) => ({ ...r, amount: n(r.amount) })),
    addRequest: async (r: { address: string; label: string; amount: bigint; message: string }) => {
      const e = await call<Omit<ReceiveRequest, 'amount'> & { amount: number }>('wallet.requests.add', { ...w, ...r, amount: Number(r.amount) });
      return { ...e, amount: n(e.amount) };
    },
    removeRequests: (items: { address: string; id: number }[]) => call<number>('wallet.requests.remove', { ...w, items }),
    coins: async () => {
      const groups = await call<(Omit<CoinGroup, 'coins'> & { coins: (Omit<Coin, 'sats'> & { sats: number })[] })[]>('wallet.coins', w);
      return groups.map((g) => ({ ...g, coins: g.coins.map((c) => ({ ...c, sats: n(c.sats) })) }));
    },
    lockCoin: (o: Outpoint, lock: boolean) => call<boolean>('wallet.coins.lock', { ...w, txid: o.txid, vout: o.vout, lock }),
    coinSummary: async (cc: CoinControl, amounts: bigint[], subtractFee: boolean, fee: FeeChoice) => {
      const r = await call<Record<string, unknown>>('wallet.coinControl.summary', {
        ...w,
        selected: outpoints(cc.selected),
        amounts: amounts.map(Number),
        subtractFee,
        ...fee,
      });
      return {
        quantity: Number(r.quantity),
        amount: n(r.amount),
        fee: n(r.fee),
        afterFee: n(r.afterFee),
        bytes: Number(r.bytes),
        change: n(r.change),
        changeEnabled: Boolean(r.changeEnabled),
        insufficient: Boolean(r.insufficient),
        feeVary: Number(r.feeVary),
        spent: (r.spent as Outpoint[]) ?? [],
      } satisfies CoinSummary;
    },
    smartFee: (confTarget: number) => call<{ feePerK: number; fallback: boolean; reason: string; target?: number; requiredFeePerK: number }>('wallet.fee', { ...w, confTarget }),
    available: async (selected?: Outpoint[]) => n(await call<number>('wallet.available', { ...w, selected: outpoints(selected) })),
    checkChange: (address: string) => call<{ valid: boolean; mine: boolean; label: string }>('wallet.checkChange', { ...w, address }),
    prepare: async (recipients: Recipient[], cc: CoinControl, fee: FeeChoice, passphrase?: string): Promise<Prepared> => {
      const r = await call<Record<string, unknown>>(
        'wallet.send.prepare',
        withPass(
          {
            ...w,
            recipients: recipients.map((x) => ({ address: x.address, label: x.label ?? '', amount: Number(x.amount), subtractFee: !!x.subtractFee, message: x.message ?? '' })),
            selected: outpoints(cc.selected),
            ...(cc.changeAddress ? { changeAddress: cc.changeAddress } : {}),
            ...fee,
          },
          passphrase,
        ),
      );
      return {
        token: String(r.token),
        fee: n(r.fee),
        vsize: Number(r.vsize),
        total: n(r.total),
        recipients: (r.recipients as { address: string; label: string; amount: number }[]).map((x) => ({ ...x, amount: n(x.amount) })),
        privateKeysDisabled: Boolean(r.privateKeysDisabled),
        externalSigner: Boolean(r.externalSigner),
        txid: String(r.txid),
      };
    },
    commit: (token: string) => call<{ complete: boolean; txid?: string; psbt?: string }>('wallet.send.commit', { ...w, token }),
    createPsbt: (token: string) => call<{ psbt: string }>('wallet.send.psbt', { ...w, token }),
    discard: (token: string) => call<boolean>('wallet.send.discard', { token }),
    signMessage: (address: string, message: string, passphrase?: string) => call<{ signature: string }>('wallet.signMessage', withPass({ ...w, address, message }, passphrase)),
    psbtOpen: (psbt: string) => call<PsbtState>('wallet.psbt.open', { ...w, psbt }),
    psbtSign: (psbt: string, passphrase?: string) => call<PsbtState>('wallet.psbt.sign', withPass({ ...w, psbt }, passphrase)),
  };
}

export type WalletClient = ReturnType<typeof walletApi>;

export const messages = {
  verify: (address: string, signature: string, message: string) => call<{ ok: boolean; message: string; field?: 'address' | 'signature' }>('message.verify', { address, signature, message }),
};

/** GUIUtil::isDust: an output of this amount to this address would be dust. */
export function isDust(address: string, amount: bigint): Promise<boolean> {
  return call<boolean>('send.isDust', { address, amount: Number(amount) });
}

export const psbt = {
  fromClipboardText: (base64: string) => call<{ psbt: string }>('psbt.decode', { base64 }),
  fromFile: (fileBase64: string) => call<{ psbt: string }>('psbt.decode', { fileBase64 }),
  open: (psbtText: string) => call<PsbtState>('wallet.psbt.open', { psbt: psbtText }),
  broadcast: (psbtText: string) => call<{ txid: string }>('psbt.broadcast', { psbt: psbtText }),
};

/** Whether an error asks for the passphrase of the wallet. */
export function needsUnlock(e: unknown): boolean {
  return e instanceof BridgeError && e.code === WalletError.UNLOCK_NEEDED;
}

// --- The transaction history (TransactionRecord, TransactionTableModel) ---

export type RecordType = 'mined' | 'recv' | 'recvOther' | 'send' | 'sendOther' | 'other';
export type TxStatus = 'confirmed' | 'unconfirmed' | 'confirming' | 'conflicted' | 'abandoned' | 'immature' | 'notaccepted';

export interface TxRecord {
  /** Unique: txid and output index. */
  key: string;
  txid: string;
  idx: number;
  type: RecordType;
  address: string;
  label: string;
  time: number;
  credit: bigint;
  debit: bigint;
  /** credit + debit. */
  amount: bigint;
  status: TxStatus;
  depth: number;
  maturesIn: number;
  countsForBalance: boolean;
  sortKey: string;
  /** Height of the block of the transaction (of the conflict, for a conflicted one); -1 out of a block. */
  blockHeight: number;
  /** In a block (c), conflicted by one (x), or neither (u); null when the host did not say. */
  state: 'c' | 'x' | 'u' | null;
  coinbase: boolean;
  abandoned: boolean;
  /** Whether the wallet trusts it, as the host said (what matters for a transaction out of a block). */
  trusted: boolean;
}

/** The records as the host sends them: one array each (RecordJson in walletapi.cpp). */
export function parseRecords(raw: unknown[][]): TxRecord[] {
  return raw.map((r) => {
    const credit = n(r[6]);
    const debit = n(r[7]);
    return {
      key: `${r[0]}:${r[1]}:${r[2]}`,
      txid: String(r[0]),
      idx: Number(r[1]),
      type: r[2] as RecordType,
      address: String(r[3]),
      label: String(r[4]),
      time: Number(r[5]),
      credit,
      debit,
      amount: credit + debit,
      status: r[8] as TxStatus,
      depth: Number(r[9]),
      maturesIn: Number(r[10]),
      countsForBalance: Boolean(r[11]),
      sortKey: String(r[12]),
      blockHeight: r.length > 13 ? Number(r[13]) : -1,
      state: r.length > 14 && (r[14] === 'c' || r[14] === 'x' || r[14] === 'u') ? r[14] : null,
      coinbase: Boolean(r[15]),
      abandoned: Boolean(r[16]),
      trusted: r.length > 17 ? Boolean(r[17]) : Boolean(r[11]),
    };
  });
}

/** Newest first, as the overview and the table sort by date (time, then the sort key of TransactionRecord). */
/**
 * The order of two records by time, then by their sort key (fixed-width digits from the host: compared
 * as they are, not by the locale, which is ten times slower on a long history).
 */
export function compareRecords(a: Pick<TxRecord, 'time' | 'sortKey'>, b: Pick<TxRecord, 'time' | 'sortKey'>): number {
  return a.time - b.time || (a.sortKey < b.sortKey ? -1 : a.sortKey > b.sortKey ? 1 : 0);
}

export function sortNewestFirst<T extends Pick<TxRecord, 'time' | 'sortKey'>>(records: readonly T[]): T[] {
  return [...records].sort((a, b) => compareRecords(b, a));
}

/** The newest `count` records, newest first, in one pass over the history (no sort of all of it). */
export function newest<T extends Pick<TxRecord, 'time' | 'sortKey'>>(records: readonly T[], count: number): T[] {
  const best: T[] = [];
  for (const r of records) {
    if (best.length === count && compareRecords(r, best[best.length - 1]) <= 0) continue;
    let i = best.length;
    while (i > 0 && compareRecords(r, best[i - 1]) > 0) i--;
    best.splice(i, 0, r);
    if (best.length > count) best.pop();
  }
  return best;
}

/**
 * TransactionRecord::updateStatus, from the height of the wallet: the confirmations, the maturity and
 * whether the record counts for the balance (`maturity`: blocks a coinbase needs, its own included).
 * The same record comes back when nothing changed; one the host gave no state for is kept as it is.
 */
export function withStatus(r: TxRecord, height: number, maturity: number): TxRecord {
  if (r.state === null || maturity <= 0) return r;
  const depth = r.state === 'c' ? height - r.blockHeight + 1 : r.state === 'x' ? -(height - r.blockHeight + 1) : 0;
  const toMaturity = r.coinbase ? Math.max(0, maturity - depth) : 0;
  const trusted = depth >= 1 ? true : depth < 0 ? false : r.trusted;
  const countsForBalance = trusted && !(toMaturity > 0);
  let status: TxStatus;
  let maturesIn = 0;
  if (r.type === 'mined') {
    if (toMaturity > 0) {
      if (r.state === 'c') {
        status = 'immature';
        maturesIn = toMaturity;
      } else {
        status = 'notaccepted';
      }
    } else {
      status = 'confirmed';
    }
  } else if (depth < 0) {
    status = 'conflicted';
  } else if (depth === 0) {
    status = r.abandoned ? 'abandoned' : 'unconfirmed';
  } else if (depth < RECOMMENDED_CONFIRMATIONS) {
    status = 'confirming';
  } else {
    status = 'confirmed';
  }
  if (status === r.status && depth === r.depth && maturesIn === r.maturesIn && countsForBalance === r.countsForBalance) return r;
  return { ...r, status, depth, maturesIn, countsForBalance };
}

export const RECOMMENDED_CONFIRMATIONS = 6;

/** TransactionTableModel::formatTxType. */
export function typeText(type: RecordType): string {
  switch (type) {
    case 'recv':
      return t('Received with');
    case 'recvOther':
      return t('Received from');
    case 'send':
    case 'sendOther':
      return t('Sent to');
    case 'mined':
      return t('Mined');
    default:
      return '';
  }
}

/** TransactionTableModel::formatTxStatus. */
export function statusText(r: Pick<TxRecord, 'status' | 'depth' | 'maturesIn'>): string {
  switch (r.status) {
    case 'unconfirmed':
      return t('Unconfirmed');
    case 'abandoned':
      return t('Abandoned');
    case 'confirming':
      return t('Confirming (%1 of %2 recommended confirmations)', r.depth, RECOMMENDED_CONFIRMATIONS);
    case 'confirmed':
      return t('Confirmed (%1 confirmations)', r.depth);
    case 'conflicted':
      return t('Conflicted');
    case 'immature':
      return t('Immature (%1 confirmations, will be available after %2)', r.depth, r.depth + r.maturesIn);
    case 'notaccepted':
      return t('Generated but not accepted');
  }
}

/** How far a transaction is from being confirmed, 0 to 1 (the status icon of the table). */
export function confirmationProgress(r: Pick<TxRecord, 'status' | 'depth' | 'maturesIn'>): number {
  switch (r.status) {
    case 'confirmed':
      return 1;
    case 'confirming':
      return Math.min(1, r.depth / RECOMMENDED_CONFIRMATIONS);
    case 'immature':
      return r.depth / Math.max(1, r.depth + r.maturesIn);
    default:
      return 0;
  }
}

/** TransactionTableModel::formatTxToAddress: the label, or the address, or "(n/a)". */
export function addressText(r: Pick<TxRecord, 'type' | 'address' | 'label'>, withAddress = false): string {
  switch (r.type) {
    case 'recvOther':
    case 'sendOther':
      return r.address;
    case 'recv':
    case 'send':
    case 'mined':
      if (!r.label) return r.address;
      return withAddress ? `${r.label} (${r.address})` : r.label;
    default:
      return '(n/a)';
  }
}

/** TransactionTableModel::formatTxAmount: amounts that do not count yet in brackets. */
export function amountText(r: Pick<TxRecord, 'amount' | 'countsForBalance'>, unit: Unit, brackets = true, separators = true): string {
  const text = formatAmount(r.amount, unit, { separators });
  return brackets && !r.countsForBalance ? `[${text}]` : text;
}

// --- Filters (TransactionView, TransactionFilterProxy) ---

export type DateFilter = 'all' | 'today' | 'week' | 'month' | 'lastMonth' | 'year' | 'range';
export type TypeFilter = 'all' | 'received' | 'sent' | 'mined' | 'other';

export const DATE_FILTERS: { id: DateFilter; label: string }[] = [
  { id: 'all', label: 'All' },
  { id: 'today', label: 'Today' },
  { id: 'week', label: 'This week' },
  { id: 'month', label: 'This month' },
  { id: 'lastMonth', label: 'Last month' },
  { id: 'year', label: 'This year' },
  { id: 'range', label: 'Range…' },
];

export const TYPE_FILTERS: { id: TypeFilter; label: string; types: RecordType[] }[] = [
  { id: 'all', label: 'All', types: ['mined', 'recv', 'recvOther', 'send', 'sendOther', 'other'] },
  { id: 'received', label: 'Received with', types: ['recv', 'recvOther'] },
  { id: 'sent', label: 'Sent to', types: ['send', 'sendOther'] },
  { id: 'mined', label: 'Mined', types: ['mined'] },
  { id: 'other', label: 'Other', types: ['other'] },
];

function startOfDay(d: Date): Date {
  return new Date(d.getFullYear(), d.getMonth(), d.getDate());
}

/** The time range of a date filter, in seconds (TransactionView::chooseDate). `to` is excluded. */
export function dateRange(filter: DateFilter, now: Date, from?: Date, to?: Date): { from?: number; to?: number } {
  const secs = (d: Date) => Math.floor(d.getTime() / 1000);
  const today = startOfDay(now);
  switch (filter) {
    case 'all':
      return {};
    case 'today':
      return { from: secs(today) };
    case 'week': {
      // From the last Monday.
      const day = (today.getDay() + 6) % 7;
      return { from: secs(new Date(today.getFullYear(), today.getMonth(), today.getDate() - day)) };
    }
    case 'month':
      return { from: secs(new Date(today.getFullYear(), today.getMonth(), 1)) };
    case 'lastMonth':
      return { from: secs(new Date(today.getFullYear(), today.getMonth() - 1, 1)), to: secs(new Date(today.getFullYear(), today.getMonth(), 1)) };
    case 'year':
      return { from: secs(new Date(today.getFullYear(), 0, 1)) };
    case 'range': {
      const out: { from?: number; to?: number } = {};
      if (from) out.from = secs(startOfDay(from));
      if (to) out.to = secs(new Date(to.getFullYear(), to.getMonth(), to.getDate() + 1));
      return out;
    }
  }
}

export interface TxFilter {
  from?: number;
  to?: number;
  type: TypeFilter;
  search: string;
  /** Smallest absolute amount, in satoshis. */
  minAmount: bigint;
  /** Show conflicted transactions. */
  showInactive: boolean;
}

/** TransactionFilterProxy::filterAcceptsRow. The search looks in the address, the label and the txid. */
// What the search looks in, lower case, made once per record: kept by the key of the record, as a new
// block makes new record objects (their status) but not new text.
const haystacks = new Map<string, { label: string; text: string }>();

function haystack(r: TxRecord): string {
  const kept = haystacks.get(r.key);
  if (kept && kept.label === r.label) return kept.text;
  const text = `${r.address}\n${r.label}\n${r.txid}`.toLowerCase();
  if (haystacks.size > 500_000) haystacks.clear();
  haystacks.set(r.key, { label: r.label, text });
  return text;
}

export function filterRecords(records: readonly TxRecord[], f: TxFilter): TxRecord[] {
  const types = new Set(TYPE_FILTERS.find((t) => t.id === f.type)?.types ?? []);
  const search = f.search.trim().toLowerCase();
  return records.filter((r) => {
    if (!f.showInactive && r.status === 'conflicted') return false;
    if (!types.has(r.type)) return false;
    if (f.from !== undefined && r.time < f.from) return false;
    if (f.to !== undefined && r.time >= f.to) return false;
    if (search && !haystack(r).includes(search)) return false;
    const abs = r.amount < 0n ? -r.amount : r.amount;
    return abs >= f.minAmount;
  });
}

// --- CSV (CSVModelWriter: every value quoted, quotes doubled) ---

export function toCsv(header: string[], rows: string[][]): string {
  const value = (v: string) => `"${v.replace(/"/g, '""')}"`;
  return [header, ...rows].map((r) => r.map(value).join(',')).join('\n') + '\n';
}

/** ISO date and time, local (as QDateTime::toString(Qt::ISODate) in the export of chains-qt). */
export function isoLocal(secs: number): string {
  const d = new Date(secs * 1000);
  const pad = (x: number) => String(x).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

/** The transaction history as chains-qt exports it: Confirmed, Date, Type, Label, Address, Amount, ID. */
export function transactionsCsv(records: readonly TxRecord[], unit: Unit): string {
  const rows = records.map((r) => [
    String(r.status === 'confirmed' || r.status === 'confirming'),
    isoLocal(r.time),
    typeText(r.type),
    r.label,
    r.address,
    formatAmount(r.amount, unit, { separators: false }),
    r.txid,
  ]);
  return toCsv(['Confirmed', 'Date', 'Type', 'Label', 'Address', `Amount (${unit})`, 'ID'], rows);
}

/** The address list as chains-qt exports it: Label, Address. */
export function addressesCsv(entries: readonly AddressEntry[]): string {
  return toCsv(['Label', 'Address'], entries.map((e) => [e.label, e.address]));
}

/** A receiving address of the wallet as the Addresses tab of the Receive page lists it. */
export interface ReceivingAddress {
  address: string;
  label: string;
  /** The id of ADDRESS_TYPES ('' when the form is none of them). */
  type: string;
  /** Some coins came to it (in the history). */
  used: boolean;
  /** Everything it received, in the history (not conflicted or abandoned). */
  received: bigint;
  /** The time of the last payment to it; 0 when none. */
  lastUsed: number;
}

/**
 * The receiving addresses of the address book, with what the history says of each: received, how
 * much, and when last (the records "Received with" and "Mined" name the address; a payment to one's
 * own address is such a record too).
 */
export function receivingAddresses(entries: readonly AddressEntry[], records: readonly TxRecord[], typeOf: (address: string) => string): ReceivingAddress[] {
  const seen = new Map<string, { received: bigint; last: number }>();
  for (const r of records) {
    if ((r.type !== 'recv' && r.type !== 'mined') || r.credit <= 0n || !r.address) continue;
    if (r.status === 'conflicted' || r.status === 'abandoned' || r.abandoned) continue;
    const s = seen.get(r.address) ?? { received: 0n, last: 0 };
    s.received += r.credit;
    s.last = Math.max(s.last, r.time);
    seen.set(r.address, s);
  }
  return entries
    .filter((e) => e.purpose === 'receive')
    .map((e) => {
      const s = seen.get(e.address);
      return { address: e.address, label: e.label, type: typeOf(e.address), used: !!s, received: s?.received ?? 0n, lastUsed: s?.last ?? 0 };
    });
}

/** The name of an address type (ADDRESS_TYPES), in the language shown. */
export function addressTypeText(type: string): string {
  const known = ADDRESS_TYPES.find((a) => a.id === type);
  return known ? t(known.label) : t('Other');
}

/** TransactionTableModel TxPlainTextRole: the line "Copy full transaction details" copies. */
export function plainText(r: TxRecord, unit: Unit): string {
  const d = new Date(r.time * 1000);
  const pad = (x: number) => String(x).padStart(2, '0');
  let out = `${d.getMonth() + 1}/${d.getDate()}/${String(d.getFullYear()).slice(-2)} ${pad(d.getHours())}:${pad(d.getMinutes())} ${statusText(r)}. `;
  const type = typeText(r.type);
  if (type) out += `${type} `;
  if (r.address) out += `${r.label ? `(${r.label})` : '(no label)'} ${r.address} `;
  return out + amountText(r, unit, false, false);
}

/** The running balance after each transaction, oldest first: for the balance chart of the overview. */
export function balanceHistory(records: readonly TxRecord[]): { time: number; balance: bigint }[] {
  const counted = records.filter((r) => r.status !== 'conflicted' && r.status !== 'abandoned' && r.status !== 'notaccepted');
  const byTime = counted.sort(compareRecords);
  const out: { time: number; balance: bigint }[] = [];
  let balance = 0n;
  for (const r of byTime) {
    balance += r.amount;
    const last = out[out.length - 1];
    if (last && last.time === r.time) last.balance = balance;
    else out.push({ time: r.time, balance });
  }
  return out;
}

// --- The confirmation targets of the send page (sendcoinsdialog.cpp) ---

export const CONF_TARGETS = [2, 4, 6, 12, 24, 48, 144, 504, 1008] as const;

/** getIndexForConfTarget: the first target at least as long, or the last. */
export function nearestConfTarget(target: number): number {
  return CONF_TARGETS.find((t) => t >= target) ?? CONF_TARGETS[CONF_TARGETS.length - 1];
}

/** Address types of the receive page (ReceiveCoinsDialog::setModel). */
export const ADDRESS_TYPES: { id: string; label: string; tip: string; taproot?: boolean }[] = [
  { id: 'legacy', label: 'Base58 (Legacy)', tip: 'Not recommended due to higher fees and less protection against typos.' },
  { id: 'p2sh-segwit', label: 'Base58 (P2SH-SegWit)', tip: 'Generates an address compatible with older wallets.' },
  { id: 'bech32', label: 'Bech32 (SegWit)', tip: "Generates a native segwit address (BIP-173). Some old wallets don't support it." },
  { id: 'bech32m', label: 'Bech32m (Taproot)', tip: 'Bech32m (BIP-350) is an upgrade to Bech32, wallet support is still limited.', taproot: true },
];

/** The name of a file for a PSBT (SendCoinsDialog::presentPSBT): label or address, and amount, of each recipient. */
export function psbtFileName(recipients: { address: string; label?: string; amount: bigint }[], unit: Unit): string {
  return recipients.map((r) => `${r.label || r.address}-${formatAmount(r.amount, unit, { separators: false, withUnit: true })}`).join(' - ') + '.psbt';
}

/** "default wallet" for the wallet without a name (GUIUtil::WalletDisplayName). */
export function walletDisplayName(name: string): string {
  return name === '' ? '[default wallet]' : name;
}
