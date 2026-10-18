// Exact numbers from the node (lib/exactjson.ts, audit 7 E-BA6): what a double holds stays a number, what it cannot
// becomes the text of its digits; a request asked exact gets its result so, others as always.
import { describe, expect, it } from 'vitest';
import { exactResult, holdsExactly, parseExact } from '../lib/exactjson';
import { request, setExactMethods, setTransport, type BridgeRequest } from '../lib/bridge';

const BIG = 2n ** 60n + 1n; // 1152921504606846977 units

describe('exact numbers of JSON', () => {
  it('a number a double holds stays a number; one it cannot is its text', () => {
    for (const ok of ['0', '-0', '1', '0.1', '0.10000000', '20999999.99999999', '9007199254740991', '1152921504606846976', '0.30000000000000004', '1e+21', '4.6565423739069247e-10', '123.456', '-5.25']) {
      expect(holdsExactly(ok), ok).toBe(true);
    }
    for (const lossy of ['9007199254740993', String(BIG), '11529215046.06846977', '-11529215046.06846977', '0.1000000000000000000001', '1'.repeat(120)]) {
      expect(holdsExactly(lossy), lossy).toBe(false);
    }
  });

  it('parses a reply: strings untouched (digits and escapes in them too), lossy numbers as text', () => {
    const text = `{"balance":11529215046.06846977,"supply":${BIG},"decimals":8,"price":0.5,"label":"9007199254740993 \\"q\\" 1e400","list":[1,${BIG},-2.5e-3],"ok":true,"none":null}`;
    expect(parseExact(text)).toEqual({ balance: '11529215046.06846977', supply: String(BIG), decimals: 8, price: 0.5, label: '9007199254740993 "q" 1e400', list: [1, String(BIG), -0.0025], ok: true, none: null });
  });

  it('a result the host gave as text is read exactly; anything else as it is', () => {
    expect(exactResult({ $rawjson: `[${BIG}]` })).toEqual([String(BIG)]);
    expect(exactResult({ $rawjson: '"txid"' })).toBe('txid');
    expect(exactResult({ $rawjson: 'x', other: 1 })).toEqual({ $rawjson: 'x', other: 1 });
    expect(exactResult('abc')).toBe('abc');
    expect(exactResult([1])).toEqual([1]);
  });

  it('the bridge asks exact for the methods a chain names, or when told; not for others', async () => {
    const sent: BridgeRequest[] = [];
    setTransport(async (r) => {
      sent.push(r);
      return r.exact ? { $rawjson: `{"balance":${BIG}}` } : { balance: Number(BIG) };
    });
    try {
      setExactMethods(['listmyassets']);
      expect(await request('rpc', 'listmyassets', [])).toEqual({ balance: String(BIG) });
      expect(await request('rpc', 'getbalances', [])).toEqual({ balance: Number(BIG) });
      expect(await request('rpc', 'getbalances', [], { exact: true })).toEqual({ balance: String(BIG) });
      // What the host confirmed and ran (rpc.confirmed) answers exactly too, for such a method.
      expect(await request('host', 'rpc.confirmed', { method: 'listmyassets', params: [] })).toEqual({ balance: String(BIG) });
      expect(await request('host', 'rpc.confirmed', { method: 'sendtoaddress', params: [] })).toEqual({ balance: Number(BIG) });
      expect(sent.map((r) => r.exact)).toEqual([true, undefined, true, true, undefined]);
    } finally {
      setTransport(null);
    }
  });
});
