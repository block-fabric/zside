// The tools of chains-qt that work on text: proofs of funds, multisig amounts and signature counts,
// the search of the block explorer, the payload of a file stamp.

import { t } from './i18n.svelte';

import { CHAIN } from '../chain/config';

// --- Proof of funds (proofoffundsdialog.cpp) ---

export const PROOF_BEGIN = '-----BEGIN CHAINS PROOF OF FUNDS-----';
export const PROOF_SIGNATURES = '-----SIGNATURES-----';
/**
 * An extension of the format of chains-qt (user decision, 2026-10-15): the coins proven, one per line
 * ("txid:vout amount"), between the statement and the signatures. They are signed with the statement
 * (the message signed is the statement, a newline, the coin lines joined by newlines), so the proof says
 * exactly which coins. A proof without them is the proof of chains-qt, signed and checked as before.
 */
export const PROOF_COINS = '-----COINS-----';
export const PROOF_END = '-----END CHAINS PROOF OF FUNDS-----';

/** A coin listed in a proof: its outpoint and its amount (satoshis). */
export interface ProofCoin {
  txid: string;
  vout: number;
  sats: bigint;
}

/** The line of a coin in a proof: "txid:vout amount", the amount in coins with 8 decimals. */
export function proofCoinLine(c: ProofCoin): string {
  return `${c.txid}:${c.vout} ${unitsToCoins(c.sats)}`;
}

/** What the addresses sign: the statement alone, or with the coins listed (one line each, in order). */
export function proofMessage(statement: string, coins: readonly ProofCoin[] = []): string {
  return coins.length ? `${statement}\n${coins.map(proofCoinLine).join('\n')}` : statement;
}

/** The text of a proof: the statement, the coins (when chosen), then one line per address with its signature. */
export function formatProof(statement: string, signatures: { address: string; signature: string }[], coins: readonly ProofCoin[] = []): string {
  const lines = signatures.map((s) => `${s.address} ${s.signature}\n`).join('');
  const listed = coins.length ? `${PROOF_COINS}\n${coins.map((c) => `${proofCoinLine(c)}\n`).join('')}` : '';
  return `${PROOF_BEGIN}\n${statement}\n${listed}${PROOF_SIGNATURES}\n${lines}${PROOF_END}\n`;
}

/** A statement as Qt makes it of the text typed: spaces simplified (QString::simplified). */
export function simplified(text: string): string {
  return text.replace(/\s+/g, ' ').trim();
}

export interface ParsedProof {
  statement: string;
  /** The coins listed (the extension of the format); empty for a proof of chains-qt. */
  coins: ProofCoin[];
  /** What the signatures are of (proofMessage). */
  message: string;
  /** The signature lines: address and signature, or null for a line that has not two parts. */
  entries: ({ address: string; signature: string } | { invalid: string })[];
}

/** Take a proof apart; null when the text is not one. */
export function parseProof(text: string): ParsedProof | null {
  const lines = text.split('\n').filter((l) => l.length > 0);
  const begin = lines.indexOf(PROOF_BEGIN);
  const signatures = lines.indexOf(PROOF_SIGNATURES);
  const end = lines.indexOf(PROOF_END);
  const coinsAt = lines.indexOf(PROOF_COINS);
  const withCoins = coinsAt === begin + 2 && begin >= 0;
  if (begin < 0 || end <= signatures || (withCoins ? signatures <= coinsAt + 1 : signatures !== begin + 2)) return null;
  const coins: ProofCoin[] = [];
  if (withCoins) {
    for (let i = coinsAt + 1; i < signatures; i++) {
      const m = /^([0-9a-f]{64}):(\d{1,6}) (\d{1,8}(?:\.\d{1,8})?)$/.exec(simplified(lines[i]));
      const sats = m ? coinsToUnits(m[3]) : null;
      // A coin line that is not one: not a proof (what was signed cannot be known).
      if (!m || sats === null) return null;
      // A coin listed twice would be counted twice: not a proof.
      if (coins.some((c) => c.txid === m[1] && c.vout === Number(m[2]))) return null;
      coins.push({ txid: m[1], vout: Number(m[2]), sats });
    }
  }
  const entries: ParsedProof['entries'] = [];
  for (let i = signatures + 1; i < end; i++) {
    const parts = simplified(lines[i]).split(' ');
    entries.push(parts.length === 2 ? { address: parts[0], signature: parts[1] } : { invalid: parts[0] ?? '' });
  }
  const statement = lines[begin + 1];
  return { statement, coins, message: proofMessage(statement, coins), entries };
}

/** An output as gettxout gives it (null: spent, or never was). */
export interface TxOutNow {
  value: number;
  scriptPubKey: { address?: string };
}

export interface CoinsCheck {
  /** Unspent now, paid to an address that signed, of the amount listed. */
  good: ProofCoin[];
  /** Spent, or not on the chain (yet). */
  spent: ProofCoin[];
  /** Paid to an address that did not sign the proof (or to no address). */
  unsigned: (ProofCoin & { address: string })[];
  /** Not of the amount listed. */
  wrongAmount: (ProofCoin & { now: bigint })[];
  /** What the good coins hold. */
  total: bigint;
}

/** The coins of a proof against the chain now (one gettxout answer per coin, in order) and the addresses that signed. */
export function checkProofCoins(coins: readonly ProofCoin[], now: readonly (TxOutNow | null)[], signed: ReadonlySet<string>): CoinsCheck {
  const out: CoinsCheck = { good: [], spent: [], unsigned: [], wrongAmount: [], total: 0n };
  // Each coin counts once, whatever lists it again (parseProof refuses such a proof already).
  const seen = new Set<string>();
  coins.forEach((c, i) => {
    const key = `${c.txid}:${c.vout}`;
    if (seen.has(key)) return;
    seen.add(key);
    const o = now[i];
    if (!o) return void out.spent.push(c);
    const address = o.scriptPubKey.address ?? '';
    if (!signed.has(address)) return void out.unsigned.push({ ...c, address });
    const sats = jsonCoinsToUnits(o.value);
    if (sats !== c.sats) return void out.wrongAmount.push({ ...c, now: sats });
    out.good.push(c);
    out.total += c.sats;
  });
  return out;
}

/** The verdict of a check: valid, partly valid, or not valid, with the amount on the addresses now. */
export function proofVerdict(valid: number, invalid: number, total: string): string {
  if (invalid === 0 && valid > 0) return `VALID. The holder of ${valid} address(es), with ${total} ${CHAIN.coin.ticker} on them now, signed this statement:`;
  if (valid === 0) return 'NOT VALID. No signature of the proof is good. The statement was:';
  return `PARTLY VALID. ${valid} signature(s) are good, covering ${total} ${CHAIN.coin.ticker} now, and ${invalid} are not. The statement was:`;
}

// --- Multisig lounge (multisigdialog.cpp) ---

const COIN = 100_000_000n;

/** An amount typed as coins ("1.5"), as satoshis; null if it is not one (at most 8 decimals). */
export function coinsToUnits(text: string): bigint | null {
  if (!/^\d{1,8}(\.\d{1,8})?$/.test(text.trim())) return null;
  const [whole, frac = ''] = text.trim().split('.');
  return BigInt(whole) * COIN + BigInt(frac.padEnd(8, '0'));
}

/** Satoshis as coins with 8 decimals. */
export function unitsToCoins(value: bigint): string {
  const sign = value < 0n ? '-' : '';
  const v = value < 0n ? -value : value;
  return `${sign}${v / COIN}.${String(v % COIN).padStart(8, '0')}`;
}

/** An amount of an RPC answer (a JSON number of coins) as satoshis, exactly. */
export function jsonCoinsToUnits(amount: number): bigint {
  return coinsToUnits(amount.toFixed(8)) ?? 0n;
}

/** The descriptor of a shared address: the keys sorted by the descriptor, so all partners get the same. */
export function sharedDescriptor(required: number, keys: string[]): string {
  return `wsh(sortedmulti(${required},${keys.join(',')}))`;
}

/** A descriptor without its checksum. */
export function bareDescriptor(descriptor: string): string {
  return descriptor.split('#')[0];
}

/** "2 of 3" for the descriptor of a shared address. */
export function signersText(descriptor: string): string {
  const m = /multi\((\d+),/.exec(descriptor);
  const keys = (descriptor.match(/,/g) ?? []).length;
  return t('%1 of %2', m ? m[1] : '?', keys);
}

/** The key of a wallet address, with its derivation path, from the descriptor of getaddressinfo ("wpkh(KEY)#..."). */
export function keyOfDescriptor(desc: string): string {
  const m = /^wpkh\((.+)\)#/.exec(desc);
  return m ? m[1] : '';
}

interface DecodedPsbt {
  tx?: { vout: { value: number; scriptPubKey: { address?: string } }[] };
  outputs?: { amount?: number; script?: { address?: string } }[];
  inputs: { final_scriptwitness?: unknown; partial_signatures?: Record<string, string>; witness_script?: { asm: string } }[];
  fee?: number;
}

/** How far a shared spend is (MultisigDialog::updateSignatureCount): signatures, what it pays, the fee. */
export function psbtStatus(decoded: DecodedPsbt): string {
  // The input with the fewest signatures tells how far the transaction is.
  let signatures = -1;
  let needed = '?';
  let final = true;
  for (const input of decoded.inputs) {
    if (input.final_scriptwitness) continue;
    final = false;
    const count = input.partial_signatures ? Object.keys(input.partial_signatures).length : 0;
    if (signatures < 0 || count < signatures) signatures = count;
    if (input.witness_script) needed = /^(\d+) /.exec(input.witness_script.asm)?.[1] ?? needed;
  }
  // A PSBT of version 2 lists its outputs itself; an older one carries a transaction.
  const outputs = decoded.tx ? decoded.tx.vout.map((o) => ({ amount: o.value, address: o.scriptPubKey.address })) : (decoded.outputs ?? []).map((o) => ({ amount: o.amount ?? 0, address: o.script?.address }));
  const payments = outputs.map((o) => `${o.amount.toFixed(8)} ${CHAIN.coin.ticker} to ${o.address ?? '(no address)'}`);
  const fee = decoded.fee !== undefined ? decoded.fee.toFixed(8) : '?';
  return `${final ? 'Fully signed, ready to send.' : `Signatures: ${signatures} of the ${needed} needed.`} Pays ${payments.join(', and ')}. Fee: ${fee} ${CHAIN.coin.ticker}.`;
}

// --- Block explorer (blockexplorer.cpp) ---

export type ExplorerQuery = { kind: 'height'; height: number } | { kind: 'hash'; hash: string } | { kind: 'invalid' };

/** What a search of the explorer is: a height, a hash (block or transaction), or neither. */
export function explorerQuery(input: string): ExplorerQuery {
  const q = input.trim();
  if (/^[0-9]{1,9}$/.test(q)) return { kind: 'height', height: Number(q) };
  if (/^[0-9a-fA-F]{64}$/.test(q)) return { kind: 'hash', hash: q.toLowerCase() };
  return { kind: 'invalid' };
}

// --- Timestamps (timestampdialog.cpp) ---

/** What a stamp publishes: "STAMP" and the SHA-256 of the file, in hexadecimal. */
export function stampPayload(sha256: string): string {
  return '5354414d50' + sha256.toLowerCase();
}

// --- Third-party transaction URLs (TransactionView: strThirdPartyTxUrls) ---

/** The menu items for a transaction: one per web address with a host, its %s replaced by the id. */
export function thirdPartyLinks(setting: string, txid: string): { label: string; url: string }[] {
  const out: { label: string; url: string }[] = [];
  for (const raw of setting.split('|')) {
    const text = raw.trim();
    if (!text) continue;
    let url: URL;
    try {
      url = new URL(text.replace(/%s/g, 'TXID'));
    } catch {
      continue;
    }
    if (!url.host || (url.protocol !== 'https:' && url.protocol !== 'http:')) continue;
    out.push({ label: url.host, url: text.replace(/%s/g, txid) });
  }
  return out;
}
