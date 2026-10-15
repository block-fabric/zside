// Amounts: satoshis as bigint, shown and typed in the unit the user chose
// (the units of chains-qt: CHN, mCHN, bits, sat). The ids are the kit's (kept in the preferences); the
// names shown are those of the coin of this chain (chain.json: coin.ticker, coin.name).

import { CHAIN } from '../chain/config';

export type Unit = 'CHN' | 'mCHN' | 'bits' | 'sat';

const TICKER = CHAIN.coin.ticker;
const COIN_NAME = CHAIN.coin.name;

export interface UnitInfo {
  id: Unit;
  /** Short name, shown after amounts. */
  short: string;
  long: string;
  /** Digits after the decimal point: down to one satoshi (10^decimals satoshis make one unit). */
  decimals: number;
}

export const UNITS: readonly UnitInfo[] = [
  { id: 'CHN', short: TICKER, long: COIN_NAME, decimals: 8 },
  { id: 'mCHN', short: `m${TICKER}`, long: `Milli-${COIN_NAME} (1 / 1 000)`, decimals: 5 },
  { id: 'bits', short: 'bits', long: `Micro-${COIN_NAME} (bits) (1 / 1 000 000)`, decimals: 2 },
  { id: 'sat', short: 'sat', long: 'Satoshi (sat) (1 / 100 000 000)', decimals: 0 },
];

export const COIN = 100_000_000n;
export const MAX_MONEY = 21_000_000n * COIN;

export function unitInfo(unit: Unit): UnitInfo {
  return UNITS.find((u) => u.id === unit) ?? UNITS[0];
}

export function isUnit(value: unknown): value is Unit {
  return typeof value === 'string' && UNITS.some((u) => u.id === value);
}

const THIN_SPACE = ' ';

function groupThousands(digits: string): string {
  return digits.replace(/\B(?=(\d{3})+(?!\d))/g, THIN_SPACE);
}

export interface FormatOptions {
  /** A + before positive amounts. */
  plus?: boolean;
  /** Thin spaces between groups of three digits of the whole part. */
  separators?: boolean;
  /** Add the short name of the unit. */
  withUnit?: boolean;
  /** Drop zeros at the end of the decimals (keeping at least `minDecimals`). */
  trim?: boolean;
  minDecimals?: number;
}

/** An amount in satoshis, in a unit: "1 234.56780000 CHN". */
export function formatAmount(sats: bigint | number, unit: Unit, options: FormatOptions = {}): string {
  const value = typeof sats === 'bigint' ? sats : BigInt(Math.round(sats));
  const { decimals, short } = unitInfo(unit);
  const negative = value < 0n;
  const abs = negative ? -value : value;
  // The decimals of a unit go down to one satoshi: the satoshis are the digits.
  const scaled = abs;
  const factor = 10n ** BigInt(decimals);
  let whole = (scaled / factor).toString();
  let fraction = decimals > 0 ? (scaled % factor).toString().padStart(decimals, '0') : '';
  if (options.trim && fraction) {
    const keep = Math.max(options.minDecimals ?? 0, fraction.replace(/0+$/, '').length);
    fraction = fraction.slice(0, keep);
  }
  if (options.separators !== false) whole = groupThousands(whole);
  let text = fraction ? `${whole}.${fraction}` : whole;
  if (negative) text = `-${text}`;
  else if (options.plus && value > 0n) text = `+${text}`;
  return options.withUnit ? `${text} ${short}` : text;
}

/** Satoshis from a JSON number of coins (RPC results): exact for every amount the node can give. */
export function coinsToSats(coins: number | string): bigint {
  const text = typeof coins === 'number' ? coins.toFixed(8) : coins;
  const parsed = parseAmount(text.replace(/^-/, ''), 'CHN');
  if (!parsed.ok) throw new Error(`not an amount: ${coins}`);
  return text.startsWith('-') ? -parsed.sats : parsed.sats;
}

/** Coins as the text RPC methods take: "1.23456789". */
export function satsToCoins(sats: bigint): string {
  return formatAmount(sats, 'CHN', { separators: false, trim: true });
}

export type ParseResult = { ok: true; sats: bigint } | { ok: false; error: string };

/** Read what the user typed, in a unit. Spaces between digit groups are fine, a comma is a decimal point. */
export function parseAmount(text: string, unit: Unit): ParseResult {
  const cleaned = text.trim().replace(/[\s  _]/g, '').replace(',', '.');
  if (cleaned === '') return { ok: false, error: 'Enter an amount' };
  if (!/^\d*\.?\d*$/.test(cleaned) || cleaned === '.') return { ok: false, error: 'Use digits and one decimal point' };
  const { decimals } = unitInfo(unit);
  const [whole = '', fraction = ''] = cleaned.split('.');
  if (fraction.length > decimals) {
    return { ok: false, error: decimals === 0 ? `${unitInfo(unit).short} has no decimals` : `At most ${decimals} decimals in ${unitInfo(unit).short}` };
  }
  if (whole.length > 16) return { ok: false, error: 'This amount is too large' };
  const scaled = BigInt(whole || '0') * 10n ** BigInt(decimals) + BigInt(fraction.padEnd(decimals, '0') || '0');
  const sats = scaled;
  if (sats > MAX_MONEY) return { ok: false, error: 'More than can ever exist' };
  return { ok: true, sats };
}
