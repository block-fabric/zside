// What the block explorer shows besides the blocks themselves (pages/Explorer.svelte): an address as the open wallet
// knows it (no record of addresses is kept, no chain scanned: host wallet.address), what a block of this sidechain does
// with the peg (host explorer.peg), and the block and the transactions of the mainchain it names, which are no blocks of
// this chain (shown with a copy button and a word on where to see them, never as links).
import { t } from './i18n.svelte';
import { CHAIN } from '../chain/config';

// --- Addresses (the explorer's address view) ---

/** An address as the wallet knows it (host wallet.address); amounts in sats. Of another wallet's: { mine: false }. */
export interface AddressActivity {
  mine: boolean;
  label?: string;
  received?: number;
  spent?: number;
  balance?: number;
  immature?: number;
  txCount?: number;
  coinCount?: number;
  /** [txid, height (-1: not in a block), time, received, spent], newest first. */
  history?: [string, number, number, number, number][];
  /** [txid, vout, amount, height, immature]. */
  coins?: [string, number, number, number, boolean][];
}

/** What validateaddress says of an address: its script. */
export interface ScriptInfo {
  isvalid?: boolean;
  address?: string;
  isscript?: boolean;
  iswitness?: boolean;
  witness_version?: number;
  scriptPubKey?: string;
}

/** The kind of script an address pays to, in words. */
export function scriptKindText(info: ScriptInfo): string {
  if (info.iswitness) {
    if (info.witness_version === 1) return t('Taproot (P2TR)');
    if (info.witness_version === 0) return info.scriptPubKey?.length === 68 ? t('SegWit script (P2WSH)') : t('SegWit (P2WPKH)');
    return t('SegWit version %1', info.witness_version ?? 0);
  }
  return info.isscript ? t('Script (P2SH)') : t('Legacy (P2PKH)');
}

/** What the search box of the explorer was given, if it may be an address (to ask the node): letters and digits only. */
export function mayBeAddress(text: string): boolean {
  return /^[A-Za-z0-9]{20,100}$/.test(text.trim());
}

/** An address cut to its start and end (a column), whole in its tooltip. */
export function shortAddress(address: string, keep = 8): string {
  return address.length <= keep * 2 + 1 ? address : `${address.slice(0, keep)}…${address.slice(-6)}`;
}

// --- The peg of a block of this sidechain (host explorer.peg) ---

export interface BlockPeg {
  /** The coinbase of the block. */
  coinbase: string;
  /** Its payouts after its first output: [vout, sats, address ('' when none)]: deposits, refunds, the chain's own. */
  payouts: [number, number, string][];
  /** The withdrawal bundle its coinbase commits to. */
  bundle: string | null;
  /** [txid, vout, sats paid on the mainchain, fee for its miners, script there (hex), address a refund pays]. */
  withdrawals: [string, number, number, number, string, string][];
  /** [txid, withdrawal txid, withdrawal vout]. */
  refunds: [string, string, number][];
  /** More of them than shown. */
  more: boolean;
}

/** A deposit the mainchain recorded for this chain, as the app follows it (lib/side.ts DepositEntry). */
export interface MainDeposit {
  /** The transaction of the mainchain that made it. */
  txid: string;
  destination: string;
  sats: bigint;
}

/** The address of this chain a deposit names: "s<slot>_<address>_<checksum>", or the address itself. */
export function depositTarget(destination: string): string {
  const m = /^s\d+_(.+)_[0-9a-f]{6}$/.exec(destination.trim());
  return m ? m[1] : destination.trim();
}

/**
 * Which payouts of a coinbase are deposits the mainchain recorded: the same address and amount, each deposit matched
 * once, oldest first (the coinbase pays them in the order they were recorded). By output: the transaction of the
 * mainchain that made it.
 */
export function matchDeposits(payouts: readonly [number, number, string][], deposits: readonly MainDeposit[]): Map<number, string> {
  const found = new Map<number, string>();
  const used = new Set<number>();
  for (const [vout, sats, address] of payouts) {
    if (!address) continue;
    const i = deposits.findIndex((d, k) => !used.has(k) && d.sats === BigInt(sats) && depositTarget(d.destination) === address);
    if (i < 0) continue;
    used.add(i);
    found.set(vout, deposits[i].txid);
  }
  return found;
}

/** The name of the mainchain, for what is of it. */
export function mainchainName(): string {
  return CHAIN.mainchain?.name ?? 'Chains';
}

/** The tooltip of a block of the mainchain: no block of this chain, seen in the wallet of the mainchain. */
export function mainBlockTip(): string {
  return t('A block of %1: see it in the %1 wallet', mainchainName());
}

/** The tooltip of a transaction of the mainchain. */
export function mainTxTip(): string {
  return t('A transaction of %1: see it in the %1 wallet', mainchainName());
}
