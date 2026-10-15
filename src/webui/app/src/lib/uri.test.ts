import { describe, expect, it } from 'vitest';
import { formatPaymentUri, parsePaymentUri } from './uri';
import { CHAIN } from '../chain/config';

// The scheme of this chain (chain/chain.json).
const S = CHAIN.uriScheme;

describe('payment URIs', () => {
  it('parses BIP 21', () => {
    expect(parsePaymentUri(`${S}:rchn1qabc?amount=1.5&label=Rent%20May&message=thanks`)).toEqual({
      ok: true,
      request: { address: 'rchn1qabc', amount: 150000000n, label: 'Rent May', message: 'thanks' },
    });
    expect(parsePaymentUri(`${S.toUpperCase()}:rchn1qabc`)).toEqual({ ok: true, request: { address: 'rchn1qabc' } });
    expect(parsePaymentUri(`${S}:rchn1qabc?label=a+b`)).toMatchObject({ ok: true, request: { label: 'a b' } });
  });
  it('refuses what it cannot honour', () => {
    expect(parsePaymentUri(`${S}://rchn1qabc`)).toMatchObject({ ok: false });
    expect(parsePaymentUri('bitcoin:bc1qabc')).toMatchObject({ ok: false });
    expect(parsePaymentUri(`${S}:rchn1qabc?amount=abc`)).toMatchObject({ ok: false });
    expect(parsePaymentUri(`${S}:rchn1qabc?req-somethingnew=1`)).toMatchObject({ ok: false });
    expect(parsePaymentUri(`${S}:?amount=1`)).toMatchObject({ ok: false });
    expect(parsePaymentUri(`${S}:rchn1qabc?somethingnew=1`)).toMatchObject({ ok: true });
  });
  it('formats and parses back', () => {
    const uri = formatPaymentUri({ address: 'rchn1qabc', amount: 1n, label: 'A & B', message: 'é' });
    expect(uri).toBe(`${S}:rchn1qabc?amount=0.00000001&label=A%20%26%20B&message=%C3%A9`);
    expect(parsePaymentUri(uri)).toEqual({ ok: true, request: { address: 'rchn1qabc', amount: 1n, label: 'A & B', message: 'é' } });
  });
});
