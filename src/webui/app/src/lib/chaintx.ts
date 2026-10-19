// What a chain may say of a transaction of the wallet beyond chain/txinfo.svelte.ts's required exports (optional
// exports, so that a chain that does not have them needs no change): whether it hides it (an ignored asset's:
// BitAssets, Layers), and what the notice of it offers (Ignore this asset) or warns of (a look-alike asset).
import * as txinfo from '../chain/txinfo.svelte';

export interface ChainTxNotice {
  /** A warning line put in the notice (a name like an asset the wallet holds). */
  warning?: string;
  /** Buttons of the notice. */
  actions?: { label: string; run: () => void }[];
}

const optional = txinfo as unknown as {
  chainTxHidden?: (txid: string) => boolean;
  chainTxNotice?: (txid: string) => ChainTxNotice | undefined;
};

/** Whether the chain hides this transaction of the wallet shown (reactive where the chain's state is). */
export function chainTxHidden(txid: string): boolean {
  return optional.chainTxHidden?.(txid) ?? false;
}

/** What the notice of a new transaction offers or warns of, if anything. */
export function chainTxNotice(txid: string): ChainTxNotice | undefined {
  return optional.chainTxNotice?.(txid);
}
