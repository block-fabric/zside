import { describe, expect, it } from 'vitest';
import { coinsToSats, formatAmount, MAX_MONEY, parseAmount, satsToCoins } from './units';

describe('formatAmount', () => {
  it('formats in every unit', () => {
    expect(formatAmount(123456789n, 'CHN')).toBe('1.23456789');
    expect(formatAmount(123456789n, 'mCHN')).toBe('1 234.56789');
    expect(formatAmount(123456789n, 'bits')).toBe('1 234 567.89');
    expect(formatAmount(123456789n, 'sat')).toBe('123 456 789');
  });
  it('signs, units, trimming', () => {
    expect(formatAmount(-5000n, 'CHN')).toBe('-0.00005000');
    expect(formatAmount(5000n, 'CHN', { plus: true, withUnit: true })).toBe('+0.00005000 CHN');
    expect(formatAmount(150000000n, 'CHN', { trim: true })).toBe('1.5');
    expect(formatAmount(100000000n, 'CHN', { trim: true })).toBe('1');
    expect(formatAmount(2100000000000000n, 'CHN', { separators: false })).toBe('21000000.00000000');
  });
});

describe('parseAmount', () => {
  it('reads what users type', () => {
    expect(parseAmount('1.5', 'CHN')).toEqual({ ok: true, sats: 150000000n });
    expect(parseAmount(' 1 000,25 ', 'mCHN')).toEqual({ ok: true, sats: 100025000n });
    expect(parseAmount('.5', 'CHN')).toEqual({ ok: true, sats: 50000000n });
    expect(parseAmount('42', 'sat')).toEqual({ ok: true, sats: 42n });
    expect(parseAmount('0.00000001', 'CHN')).toEqual({ ok: true, sats: 1n });
  });
  it('refuses what is not an amount', () => {
    expect(parseAmount('', 'CHN').ok).toBe(false);
    expect(parseAmount('1.2.3', 'CHN').ok).toBe(false);
    expect(parseAmount('-1', 'CHN').ok).toBe(false);
    expect(parseAmount('1e5', 'CHN').ok).toBe(false);
    expect(parseAmount('0.000000001', 'CHN').ok).toBe(false);
    expect(parseAmount('1.5', 'sat').ok).toBe(false);
    expect(parseAmount('21000000.00000001', 'CHN').ok).toBe(false);
    expect(parseAmount('21000000', 'CHN')).toEqual({ ok: true, sats: MAX_MONEY });
  });
  it('round-trips through the RPC form', () => {
    for (const sats of [0n, 1n, 99n, 100000000n, 123456789012345n, MAX_MONEY]) {
      expect(coinsToSats(satsToCoins(sats))).toBe(sats);
    }
    expect(coinsToSats(0.1)).toBe(10000000n);
    expect(coinsToSats(-2.5)).toBe(-250000000n);
  });
});
