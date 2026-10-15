// Test helpers: addresses of this chain (chain/chain.json) made with the bech32 reference algorithm.
import { CHAIN } from '../chain/config';
// Made with the bech32 reference algorithm for the human-readable parts of Chains.
export function encode(hrp: string, data: number[], bech32m: boolean): string {
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

export function program(bytes: number): number[] {
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

/** A valid P2WPKH and a P2TR address of a network. */
export const regtestP2wpkh = encode(CHAIN.networks.regtest.hrp, [0, ...program(20)], false);
export const regtestP2tr = encode(CHAIN.networks.regtest.hrp, [1, ...program(32)], true);
