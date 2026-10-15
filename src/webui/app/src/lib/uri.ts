// Payment URIs (BIP 21) of this chain: <scheme>:<address>?amount=1.5&label=...&message=... (the scheme of chain.json)
// As GUIUtil::parseBitcoinURI and formatBitcoinURI of chains-qt.
import { parseAmount, satsToCoins } from './units';
import { CHAIN } from '../chain/config';

export const SCHEME = CHAIN.uriScheme;

export interface PaymentRequest {
  address: string;
  /** Satoshis. */
  amount?: bigint;
  label?: string;
  message?: string;
}

export type UriResult = { ok: true; request: PaymentRequest } | { ok: false; error: string };

function decode(text: string): string {
  return decodeURIComponent(text.replace(/\+/g, ' '));
}

export function parsePaymentUri(uri: string): UriResult {
  const text = uri.trim();
  if (!text.toLowerCase().startsWith(SCHEME + ':')) return { ok: false, error: `Not a ${SCHEME}: URI` };
  if (text.toLowerCase().startsWith(SCHEME + '://')) {
    return { ok: false, error: `'${SCHEME}://' is not a valid URI. Use '${SCHEME}:' instead.` };
  }
  const rest = text.slice(SCHEME.length + 1);
  const [path, query = ''] = rest.split('?', 2);
  let address: string;
  try {
    address = decode(path);
  } catch {
    return { ok: false, error: 'URI cannot be parsed' };
  }
  const request: PaymentRequest = { address };
  if (query) {
    for (const pair of query.split('&')) {
      if (!pair) continue;
      const eq = pair.indexOf('=');
      let key: string;
      let value: string;
      try {
        key = decode(eq < 0 ? pair : pair.slice(0, eq));
        value = decode(eq < 0 ? '' : pair.slice(eq + 1));
      } catch {
        return { ok: false, error: 'URI cannot be parsed' };
      }
      // A parameter the wallet must understand, and does not.
      const required = key.startsWith('req-');
      const name = required ? key.slice(4) : key;
      if (name === 'label') request.label = value;
      else if (name === 'message') request.message = value;
      else if (name === 'amount') {
        if (value) {
          const amount = parseAmount(value, 'CHN');
          if (!amount.ok) return { ok: false, error: `URI cannot be parsed: the amount ${value} is not valid` };
          request.amount = amount.sats;
        }
      } else if (required) {
        return { ok: false, error: `URI cannot be parsed: it needs ${name}, which this wallet does not know` };
      }
    }
  }
  if (!request.address) return { ok: false, error: 'URI cannot be parsed: no address' };
  return { ok: true, request };
}

/** The URI of a payment request. */
export function formatPaymentUri(request: PaymentRequest): string {
  const params: string[] = [];
  if (request.amount !== undefined && request.amount > 0n) params.push(`amount=${satsToCoins(request.amount)}`);
  if (request.label) params.push(`label=${encodeURIComponent(request.label)}`);
  if (request.message) params.push(`message=${encodeURIComponent(request.message)}`);
  return `${SCHEME}:${request.address}${params.length ? '?' + params.join('&') : ''}`;
}
