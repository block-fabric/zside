// Deposits to a sidechain the mempool dropped (the node says so: "sidechain_deposit_dropped", a record of the history in
// the status "dropped"): they will not be mined as they are, and their coins are spendable again. "Make it again" opens the
// deposit form of the Sidechains page with the dropped deposit in it, and the deposit made then replaces it
// (createsidechaindeposit "replaces": only one of the two can ever be mined), on the user's word given to the desktop.
import { goTo } from './nav.svelte';
import { walletApi, type TxRecord } from './wallet';

export interface DepositAgain {
  /** The dropped deposit. */
  replaces: string;
  slot: number;
  destination: string;
  /** The amount, when the host can tell it from the transaction (satoshis). */
  amount: bigint | null;
}

/** The dropped deposit the deposit form is to make again (taken by the Sidechains page). */
export const depositAgain = $state<{ next: DepositAgain | null }>({ next: null });

/** Whether a record of the history is a deposit the mempool dropped. */
export function isDropped(r: Pick<TxRecord, 'status'>): boolean {
  return r.status === 'dropped';
}

/** Open the deposit form with a dropped deposit of the wallet, to make it again. */
export async function makeDepositAgain(wallet: string, txid: string): Promise<void> {
  const info = await walletApi(wallet).depositInfo(txid);
  depositAgain.next = { replaces: info.txid, slot: info.slot, destination: info.destination ?? '', amount: info.amount === undefined ? null : BigInt(info.amount) };
  goTo('sidechains', 'sidechains', String(info.slot));
}
