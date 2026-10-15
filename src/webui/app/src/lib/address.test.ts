import { describe, expect, it } from 'vitest';
import { bech32Decode, checkAddress } from './address';
import { CHAIN } from '../chain/config';

// The regtest addresses of this chain (chain/chain.json).
const R = CHAIN.networks.regtest.hrp;

// Made with the bech32 reference algorithm for the human-readable parts of Chains.
function encode(hrp: string, data: number[], bech32m: boolean): string {
  const CHARSET = 'qpzry9x8gf2tvdw0s3jn54khce6mua7l';
  const GEN = [0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3];
  const polymod = (values: number[]) => {
    let chk = 1;
    for (const v of values) {
      const top = chk >>> 25;
      chk = ((chk & 0x1ffffff) << 5) ^ v;
      for (let i = 0; i < 5; i++) if ((top >>> i) & 1) chk ^= GEN[i];
    }
    return chk >>> 0;
  };
  const expand = [...hrp].map((c) => c.charCodeAt(0) >> 5).concat([0], [...hrp].map((c) => c.charCodeAt(0) & 31));
  const mod = polymod([...expand, ...data, 0, 0, 0, 0, 0, 0]) ^ (bech32m ? 0x2bc830a3 : 1);
  const checksum = Array.from({ length: 6 }, (_, i) => (mod >>> (5 * (5 - i))) & 31);
  return hrp + '1' + [...data, ...checksum].map((d) => CHARSET[d]).join('');
}

function program(bytes: number): number[] {
  // 8-bit bytes 0,1,2,... as 5-bit groups.
  const out: number[] = [];
  let acc = 0;
  let bits = 0;
  for (let i = 0; i < bytes; i++) {
    acc = (acc << 8) | i;
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      out.push((acc >> bits) & 31);
    }
  }
  if (bits) out.push((acc << (5 - bits)) & 31);
  return out;
}

describe('addresses', () => {
  const v0 = encode(R, [0, ...program(20)], false);
  const v1 = encode(R, [1, ...program(32)], true);

  it('decodes bech32 and bech32m', () => {
    expect(bech32Decode(v0)?.encoding).toBe('bech32');
    expect(bech32Decode(v1)?.encoding).toBe('bech32m');
  });

  it('takes the addresses of the network', async () => {
    expect(await checkAddress(v0, 'regtest')).toEqual({ valid: true, kind: 'bech32', type: 'witness_v0_keyhash' });
    expect(await checkAddress(v1, 'regtest')).toEqual({ valid: true, kind: 'bech32', type: 'witness_v1_taproot' });
    expect(await checkAddress(v0.toUpperCase(), 'regtest')).toMatchObject({ valid: true });
  });

  it('refuses typos, other networks and mixed encodings', async () => {
    const typo = v0.slice(0, -1) + (v0.endsWith('q') ? 'p' : 'q');
    expect(await checkAddress(typo, 'regtest')).toMatchObject({ valid: false, error: 'This address has a typing error in it' });
    expect(await checkAddress(v0, 'main')).toMatchObject({ valid: false, error: 'This address is for another network' });
    expect(await checkAddress(encode(R, [0, ...program(20)], true), 'regtest')).toMatchObject({ valid: false });
    expect(await checkAddress('bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4', 'main')).toMatchObject({ valid: false });
    expect(await checkAddress('', 'main')).toMatchObject({ valid: false, error: 'Enter an address' });
    expect(await checkAddress('hello', 'main')).toMatchObject({ valid: false });
  });

  it('checks base58 addresses', async () => {
    // Version 111, payload 20 zero bytes: the regtest/testnet form of the well-known address.
    expect(await checkAddress('mfWxJ45yp2SFn7UciZyNpvDKrzbhyfKrY8', 'regtest')).toMatchObject({ valid: true, type: 'pubkeyhash' });
    expect(await checkAddress('mfWxJ45yp2SFn7UciZyNpvDKrzbhyfKrY9', 'regtest')).toMatchObject({ valid: false });
    expect(await checkAddress('mfWxJ45yp2SFn7UciZyNpvDKrzbhyfKrY8', 'main')).toMatchObject({ valid: false, error: 'This address is for another network' });
  });
});
