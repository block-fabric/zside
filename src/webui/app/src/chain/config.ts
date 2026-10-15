// The chain of this app, from chain.json (the one place a chain sets what it is: its name, its coin,
// the scheme of its payment URIs, its networks, its mark, its help links, the pages of the kit it has
// no use for). The host reads the same file (src/webui/CMakeLists.txt → webui/chainconfig.h).
import type { PageId } from '../lib/nav.svelte';
import raw from './chain.json';

export interface NetworkInfo {
  /** Default ports of the node: peer to peer, RPC. */
  p2p?: number;
  rpc: number;
  /** Human-readable part of bech32 addresses. */
  hrp: string;
  /** Version bytes of base58 addresses: pay to public key hash, pay to script hash. */
  pubkey: number;
  script: number;
}

export type NetworkId = 'main' | 'test' | 'signet' | 'regtest';

export interface MainchainInfo {
  name: string;
  ticker: string;
  uriScheme: string;
  networks: Record<NetworkId, NetworkInfo>;
}

export interface ChainConfig {
  name: string;
  /** The stem of the programs: <bin>d, <bin>-cli, <bin>-app. */
  bin: string;
  role: 'mainchain' | 'sidechain';
  coin: { ticker: string; name: string };
  uriScheme: string;
  brand: { color: string; iconHue: number; mark: string[] };
  networks: Record<NetworkId, NetworkInfo>;
  /** The chain a sidechain is pegged to. */
  mainchain?: MainchainInfo;
  links: { website?: string; source?: string; issues?: string; guide?: string };
  /** Pages of the kit this chain does not have (a sidechain: no sidechains of its own, no solo mining). */
  drop: PageId[];
}

export const CHAIN: ChainConfig = raw as ChainConfig;

/** The app of a sidechain: its peg and merged mining pages, its own home. */
export const SIDECHAIN = CHAIN.role === 'sidechain';

/** Whether this chain has a page of the kit. */
export function chainHasPage(id: PageId): boolean {
  return !CHAIN.drop.includes(id);
}

/** The network as the host names it (app.info.network: "main", "test", "testnet4", "signet", "regtest"). */
export function networkId(network: string | undefined): NetworkId {
  if (network === 'test' || network === 'testnet4') return 'test';
  if (network === 'signet' || network === 'regtest') return network;
  return 'main';
}
