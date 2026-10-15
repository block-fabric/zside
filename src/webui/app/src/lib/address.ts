// Addresses of this chain (chain/chain.json), checked as they are typed (the node checks them again
// with validateaddress before anything is sent); those of the mainchain of a sidechain too (withdrawals).
import { CHAIN, type NetworkInfo } from '../chain/config';

export interface NetworkParams {
  /** Human-readable part of bech32 addresses. */
  hrp: string;
  /** Version bytes of base58 addresses: pay to public key hash, pay to script hash. */
  pubkey: number;
  script: number;
}

function paramsOf(networks: Record<string, NetworkInfo>): Record<string, NetworkParams> {
  const out: Record<string, NetworkParams> = {};
  for (const [id, n] of Object.entries(networks)) out[id] = { hrp: n.hrp, pubkey: n.pubkey, script: n.script };
  out.testnet4 = out.test;
  return out;
}

export const NETWORKS: Record<string, NetworkParams> = paramsOf(CHAIN.networks);
/** The networks of the mainchain (a sidechain's withdrawals go to its addresses). */
export const MAINCHAIN_NETWORKS: Record<string, NetworkParams> = CHAIN.mainchain ? paramsOf(CHAIN.mainchain.networks) : NETWORKS;

export function networkParams(network: string, chain: 'own' | 'mainchain' = 'own'): NetworkParams {
  const networks = chain === 'mainchain' ? MAINCHAIN_NETWORKS : NETWORKS;
  return networks[network] ?? networks.main;
}

// --- bech32 / bech32m (BIP 173, BIP 350) ---

const CHARSET = 'qpzry9x8gf2tvdw0s3jn54khce6mua7l';
const BECH32_CONST = 1;
const BECH32M_CONST = 0x2bc830a3;

function polymod(values: number[]): number {
  const GEN = [0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3];
  let chk = 1;
  for (const v of values) {
    const top = chk >>> 25;
    chk = ((chk & 0x1ffffff) << 5) ^ v;
    for (let i = 0; i < 5; i++) if ((top >>> i) & 1) chk ^= GEN[i];
  }
  return chk >>> 0;
}

function hrpExpand(hrp: string): number[] {
  const out: number[] = [];
  for (const c of hrp) out.push(c.charCodeAt(0) >> 5);
  out.push(0);
  for (const c of hrp) out.push(c.charCodeAt(0) & 31);
  return out;
}

export interface Bech32Decoded {
  hrp: string;
  data: number[];
  encoding: 'bech32' | 'bech32m';
}

export function bech32Decode(text: string): Bech32Decoded | null {
  if (text.length > 90 || text.length < 8) return null;
  if (text !== text.toLowerCase() && text !== text.toUpperCase()) return null;
  const s = text.toLowerCase();
  const sep = s.lastIndexOf('1');
  if (sep < 1 || sep + 7 > s.length) return null;
  const hrp = s.slice(0, sep);
  const data: number[] = [];
  for (const c of s.slice(sep + 1)) {
    const v = CHARSET.indexOf(c);
    if (v < 0) return null;
    data.push(v);
  }
  const check = polymod([...hrpExpand(hrp), ...data]);
  const encoding = check === BECH32_CONST ? 'bech32' : check === BECH32M_CONST ? 'bech32m' : null;
  if (!encoding) return null;
  return { hrp, data: data.slice(0, -6), encoding };
}

function convertBits(data: number[], from: number, to: number, pad: boolean): number[] | null {
  let acc = 0;
  let bits = 0;
  const out: number[] = [];
  const max = (1 << to) - 1;
  for (const v of data) {
    if (v >> from) return null;
    acc = (acc << from) | v;
    bits += from;
    while (bits >= to) {
      bits -= to;
      out.push((acc >> bits) & max);
    }
  }
  if (pad) {
    if (bits) out.push((acc << (to - bits)) & max);
  } else if (bits >= from || ((acc << (to - bits)) & max)) {
    return null;
  }
  return out;
}

// --- base58check ---

const B58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';

export function base58Decode(text: string): Uint8Array | null {
  let num = 0n;
  for (const c of text) {
    const v = B58.indexOf(c);
    if (v < 0) return null;
    num = num * 58n + BigInt(v);
  }
  const bytes: number[] = [];
  while (num > 0n) {
    bytes.unshift(Number(num & 0xffn));
    num >>= 8n;
  }
  for (const c of text) {
    if (c !== '1') break;
    bytes.unshift(0);
  }
  return new Uint8Array(bytes);
}

async function sha256(data: Uint8Array): Promise<Uint8Array> {
  return new Uint8Array(await crypto.subtle.digest('SHA-256', data as BufferSource));
}

export type AddressCheck =
  | { valid: true; kind: 'bech32' | 'base58'; type: string }
  | { valid: false; error: string };

/** Whether text is an address of this network (or of the mainchain of a sidechain), checked offline. */
export async function checkAddress(text: string, network: string, chain: 'own' | 'mainchain' = 'own'): Promise<AddressCheck> {
  const address = text.trim();
  if (!address) return { valid: false, error: 'Enter an address' };
  const params = networkParams(network, chain);
  const networks = chain === 'mainchain' ? MAINCHAIN_NETWORKS : NETWORKS;

  if (address.toLowerCase().startsWith(params.hrp + '1')) {
    const decoded = bech32Decode(address);
    if (!decoded) return { valid: false, error: 'This address has a typing error in it' };
    if (decoded.hrp !== params.hrp) return { valid: false, error: 'This address is for another network' };
    const [version, ...rest] = decoded.data;
    const program = convertBits(rest, 5, 8, false);
    if (version === undefined || !program || program.length < 2 || program.length > 40 || version > 16) {
      return { valid: false, error: 'This is not a valid address' };
    }
    if ((version === 0) !== (decoded.encoding === 'bech32')) return { valid: false, error: 'This address has a typing error in it' };
    if (version === 0 && program.length !== 20 && program.length !== 32) return { valid: false, error: 'This is not a valid address' };
    const type = version === 0 ? (program.length === 20 ? 'witness_v0_keyhash' : 'witness_v0_scripthash') : version === 1 && program.length === 32 ? 'witness_v1_taproot' : 'witness_unknown';
    return { valid: true, kind: 'bech32', type };
  }

  if (/^[1-9A-HJ-NP-Za-km-z]{25,40}$/.test(address)) {
    const bytes = base58Decode(address);
    if (!bytes || bytes.length !== 25) return { valid: false, error: 'This is not a valid address' };
    const payload = bytes.slice(0, 21);
    const checksum = bytes.slice(21);
    const hash = await sha256(await sha256(payload));
    if (!checksum.every((b, i) => b === hash[i])) return { valid: false, error: 'This address has a typing error in it' };
    if (payload[0] === params.pubkey) return { valid: true, kind: 'base58', type: 'pubkeyhash' };
    if (payload[0] === params.script) return { valid: true, kind: 'base58', type: 'scripthash' };
    return { valid: false, error: 'This address is for another network' };
  }
  // A bech32 address of another network.
  const other = Object.values(networks).find((p) => address.toLowerCase().startsWith(p.hrp + '1'));
  if (other) return { valid: false, error: 'This address is for another network' };
  // An address of the other chain of the peg (the mainchain, or this sidechain): said apart, it is an easy slip.
  const across = chain === 'mainchain' ? NETWORKS : CHAIN.mainchain ? MAINCHAIN_NETWORKS : {};
  if (Object.values(across).some((p) => address.toLowerCase().startsWith(p.hrp + '1'))) {
    return { valid: false, error: chain === 'mainchain' ? `This is an address of ${CHAIN.name}, not of ${CHAIN.mainchain?.name ?? ''}` : `This is an address of ${CHAIN.mainchain?.name ?? ''}, not of ${CHAIN.name}` };
  }
  return { valid: false, error: `This is not an address of ${chain === 'mainchain' ? (CHAIN.mainchain?.name ?? CHAIN.name) : CHAIN.name}` };
}

/** An example address of a network, for placeholders. */
export function exampleAddress(network: string, chain: 'own' | 'mainchain' = 'own'): string {
  return `${networkParams(network, chain).hrp}1q…`;
}

/**
 * Whether messages can be signed with an address: one key, P2PKH or P2WPKH (the address book of the
 * sign page lists only those). Checked on the form of the address; the node checks again.
 */
export function isSingleKeyAddress(address: string, network: string): boolean {
  const params = networkParams(network);
  if (address.toLowerCase().startsWith(params.hrp + '1')) {
    const decoded = bech32Decode(address);
    if (!decoded || decoded.hrp !== params.hrp) return false;
    const [version, ...rest] = decoded.data;
    const program = convertBits(rest, 5, 8, false);
    return version === 0 && program !== null && program.length === 20;
  }
  const bytes = base58Decode(address);
  return bytes !== null && bytes.length === 25 && bytes[0] === params.pubkey;
}

/**
 * The type of an address of the wallet, told from its form, as the Receive page names them (the ids
 * of ADDRESS_TYPES): bech32 (native segwit v0), bech32m (taproot), p2sh-segwit (base58 to a script:
 * what the wallet makes of that form), legacy (base58 to a key); '' when it is none of them.
 */
export function addressTypeOf(address: string, network: string): '' | 'bech32' | 'bech32m' | 'p2sh-segwit' | 'legacy' {
  const params = networkParams(network);
  if (address.toLowerCase().startsWith(params.hrp + '1')) {
    const decoded = bech32Decode(address);
    if (!decoded || decoded.hrp !== params.hrp) return '';
    const version = decoded.data[0];
    if (version === 0 && decoded.encoding === 'bech32') return 'bech32';
    if (version === 1 && decoded.encoding === 'bech32m') return 'bech32m';
    return '';
  }
  const bytes = base58Decode(address);
  if (!bytes || bytes.length !== 25) return '';
  if (bytes[0] === params.pubkey) return 'legacy';
  if (bytes[0] === params.script) return 'p2sh-segwit';
  return '';
}
