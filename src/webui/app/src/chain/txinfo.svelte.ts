// What the chain knows of a transaction of the wallet beyond its coin, for the Transactions page (pages/Transactions.svelte):
// an asset sent or received, a name registered... The template knows nothing more: a sidechain gives, per transaction id,
// what to show in the Type column, the words the search finds it by, its columns of the CSV export (CHAIN_TX_CSV), and a
// filter of the Type list (CHAIN_TX_FILTER); its cell (chain/LocalTxCell.svelte) shows the rest. One call for the whole
// wallet (refreshChainTx), never one per row.

export interface ChainTx {
  /** The type, in words of the app ("Sent asset"): shown in the Type column instead of the coin's. */
  type: string;
  /** What the search finds it by (the asset, its number, the addresses). */
  search: string;
  /** Its values of CHAIN_TX_CSV, in order. */
  csv: string[];
}

/** The filter of the Type list for the transactions the chain knows ('' for none): its label. */
export const CHAIN_TX_FILTER = '';
/** What the Label column is also called when the chain knows more of a transaction (LocalTxCell shows it there), '' for none. */
export const CHAIN_TX_COLUMN = '';
/** The columns the CSV export adds. */
export const CHAIN_TX_CSV: string[] = [];

/** What the chain knows of a transaction, if anything. Reactive. */
export function chainTxOf(txid: string): ChainTx | undefined {
  void txid;
  return undefined;
}

/** Read it again for a wallet (the page calls it with the history, on a new block or transaction). */
export function refreshChainTx(wallet: string): void {
  void wallet;
}
