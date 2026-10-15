// What the pages of zSide work out from the node's answers, with no state of their own: the shielded
// addresses and viewing keys of each network, memos, the history of the shielded account told in words, the
// bills of a cast, the steps of the melt/cast plan, and the estimate of how long a proof takes.
import { bech32Decode } from '../../lib/address';
import { coinsToSats } from '../../lib/units';

// --- Answers of the node (src/wallet/rpc/shielded.cpp, src/rpc/shielded.cpp) ---

/** getshieldedbalance (amounts in coins). */
export interface ShieldedBalanceRaw {
  available: number;
  pending: number;
  notes: number;
  watchonly?: number;
  rescanning_from?: number;
}

/** getshieldedinfo. */
export interface PoolInfo {
  pool: number;
  notes: number;
  spent: number;
  root: string;
  index?: { notes: number; root: string; synced: boolean };
}

/** listshieldednotes. */
export interface NoteRaw {
  txid: string;
  action: number;
  amount: number;
  memo: string;
  change: boolean;
  confirmations: number;
  spent_by?: string;
  retired?: boolean;
  watchonly?: boolean;
  viewonly?: boolean;
}

/** listshieldedtransactions. */
export interface ShieldedTxRaw {
  txid: string;
  time: number;
  confirmations: number;
  received: number;
  spent: number;
  shielded: number;
  unshielded: number;
  memos: string[];
  payments: { address: string; amount: number; memo: string; to_self: boolean }[];
}

/** A step of the melt/cast plan (listmeltcast, meltcoins, castcoins). */
export interface PlanStep {
  id: number;
  kind: 'melt' | 'cast';
  amount: number;
  coin?: string;
  status: 'planned' | 'waiting' | 'sent';
  height?: number;
  error?: string;
  txid?: string;
  confirmations?: number;
}

// --- Addresses and keys ---

export type NetworkName = string;

/** The human-readable part of shielded addresses (src/wallet/shielded.cpp ShieldedHrp). */
export function shieldedHrp(network: NetworkName): string {
  if (network === 'main') return 'zo';
  if (network === 'regtest') return 'rzo';
  return 'tzo';
}

/** The human-readable part of viewing keys (ShieldedViewingKeyHrp). */
export function viewingKeyHrp(network: NetworkName): string {
  if (network === 'main') return 'zview';
  if (network === 'regtest') return 'rzview';
  return 'tzview';
}

/** An Orchard address is 43 bytes. */
const ADDRESS_BYTES = 43;

/** Whether the text is a shielded address of the network (Bech32m, its prefix, 43 bytes); the node checks the point. */
export function isShieldedAddress(text: string, network: NetworkName): boolean {
  const d = bech32Decode(text.trim());
  if (!d || d.encoding !== 'bech32m' || d.hrp !== shieldedHrp(network)) return false;
  return Math.floor((d.data.length * 5) / 8) === ADDRESS_BYTES;
}

/** Whether the text looks like a shielded address of another network (or a mistyped one of this): a prefix of zSide. */
export function looksShielded(text: string): boolean {
  return /^(zo|tzo|rzo)1/i.test(text.trim());
}

const CHARSET = /^[qpzry9x8gf2tvdw0s3jn54khce6mua7l]+$/;
/** A viewing key is 96 bytes: 154 characters of data and 6 of checksum after the prefix. */
export function isViewingKey(text: string, network: NetworkName): boolean {
  const s = text.trim();
  if (s !== s.toLowerCase()) return false;
  const prefix = `${viewingKeyHrp(network)}1`;
  if (!s.startsWith(prefix)) return false;
  const rest = s.slice(prefix.length);
  return rest.length === 160 && CHARSET.test(rest);
}

// --- Memos ---

export const MEMO_BYTES = 512;

/** The bytes of a memo as the node keeps it (UTF-8). */
export function memoBytes(text: string): number {
  return new TextEncoder().encode(text).length;
}

// --- The history, in words ---

export type TxKind = 'shield' | 'unshield' | 'pay' | 'receive';

export interface ShieldedTx {
  txid: string;
  time: number;
  confirmations: number;
  kind: TxKind;
  /** What the account gained (+) or gave (−), in satoshis; for a shielding, what went into the pool. */
  amount: bigint;
  /** A private payment: what it gave beyond what the recipients got (the fee), satoshis. */
  fee: bigint;
  /** Who was paid, shielded (the account's own addresses left out). */
  to: { address: string; amount: bigint; memo: string }[];
  /** The memos it brought the account. */
  memos: string[];
  /** Whether anything of it is public: a shielding or an unshielding shows the amount on the chain. */
  public: boolean;
}

const sats = (coins: number) => coinsToSats(coins);

/** A transaction of listshieldedtransactions, told as the user thinks of it. */
export function describeTx(raw: ShieldedTxRaw): ShieldedTx {
  const received = sats(raw.received);
  const spent = sats(raw.spent);
  const shielded = sats(raw.shielded);
  const unshielded = sats(raw.unshielded);
  const to = raw.payments.filter((p) => !p.to_self).map((p) => ({ address: p.address, amount: sats(p.amount), memo: p.memo }));
  const base = { txid: raw.txid, time: raw.time, confirmations: raw.confirmations, memos: raw.memos, to };
  if (shielded > 0n) return { ...base, kind: 'shield', amount: shielded, fee: 0n, public: true };
  if (spent > 0n) {
    // What the account gave: the payments, the transparent outputs (an unshielding) and the fee. The fee
    // comes out of the pool too, so `unshielded` is never zero here: an unshielding is told by its payments
    // to shielded addresses, none.
    const given = spent - received;
    if (to.length === 0) return { ...base, kind: 'unshield', amount: -given, fee: 0n, public: true };
    const paid = to.reduce((n, p) => n + p.amount, 0n);
    const rest = given - paid;
    return { ...base, kind: 'pay', amount: -given, fee: rest > 0n ? rest : 0n, public: false };
  }
  return { ...base, kind: 'receive', amount: received, fee: 0n, public: false };
}

/** The memos received, newest first: one entry per memo, with the transaction that brought it. */
export interface MemoEntry {
  key: string;
  txid: string;
  time: number;
  confirmations: number;
  /** What the transaction brought the account (change included), satoshis. */
  amount: bigint;
  text: string;
}

export function memosOf(txs: readonly ShieldedTxRaw[]): MemoEntry[] {
  const out: MemoEntry[] = [];
  for (const tx of txs) {
    tx.memos.forEach((text, i) => {
      if (text.trim() === '') return;
      out.push({ key: `${tx.txid}:${i}`, txid: tx.txid, time: tx.time, confirmations: tx.confirmations, amount: sats(tx.received), text });
    });
  }
  return out.sort((a, b) => b.time - a.time || a.key.localeCompare(b.key));
}

// --- Melt & cast (src/wallet/meltcast.h) ---

/** The smallest bill: 0.01 coin. Bill k is BILL_UNIT << k. */
export const BILL_UNIT = 1_000_000n;
export const NUM_BILLS = 21;
/** The most a cast takes at once: every bill once. */
export const MAX_CAST = ((1n << BigInt(NUM_BILLS)) - 1n) * BILL_UNIT;

/** The bills an amount comes out as (largest first), and what stays in the pool. */
export function billsOf(amount: bigint): { bills: bigint[]; rest: bigint; tooMuch: boolean } {
  if (amount <= 0n) return { bills: [], rest: 0n, tooMuch: false };
  const units = amount / BILL_UNIT;
  if (units >= 1n << BigInt(NUM_BILLS)) return { bills: [], rest: amount, tooMuch: true };
  const bills: bigint[] = [];
  for (let k = NUM_BILLS - 1; k >= 0; k--) if (units & (1n << BigInt(k))) bills.push(BILL_UNIT << BigInt(k));
  return { bills, rest: amount - units * BILL_UNIT, tooMuch: false };
}

/** The steps of the plan: what is still to go first (the soonest first), then what went (the latest first). */
export function sortPlan(steps: readonly PlanStep[]): PlanStep[] {
  return steps.slice().sort((a, b) => {
    const sa = a.status === 'sent';
    const sb = b.status === 'sent';
    if (sa !== sb) return sa ? 1 : -1;
    return sa ? b.id - a.id : (a.height ?? 0) - (b.height ?? 0) || a.id - b.id;
  });
}

// --- Proofs ---

/** What a proof of each kind is expected to take, ms, before this app measured any (a bundle of two actions). */
export const PROOF_GUESS_MS = 6000;

/** The estimate from the durations measured of this kind (the latest few), or the guess. */
export function proofEstimate(measured: readonly number[]): number {
  const recent = measured.slice(-5).filter((x) => Number.isFinite(x) && x > 0);
  if (recent.length === 0) return PROOF_GUESS_MS;
  const sorted = recent.slice().sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

/**
 * How far along a proof seems, 0 to 0.97, from the time it has taken and the estimate: the line slows as
 * it nears the end, so that it never says done before the node does.
 */
export function proofProgress(elapsedMs: number, estimateMs: number): number {
  if (elapsedMs <= 0 || estimateMs <= 0) return 0;
  const x = elapsedMs / estimateMs;
  return Math.min(0.97, 1 - Math.exp(-1.6 * x));
}
