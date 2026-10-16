// What an output of a coinbase of the wallet pays, by the role the host names (local::CoinbaseRoles in
// src/webui/local.cpp): the history shows its label instead of "Mined", its details the source, and the type
// filter has one entry per role. The template has none; a sidechain whose coinbase pays outputs of its own
// kinds names them here (FastBlocks: the producer's share of a block of stake, the fee pot paid to a BMM
// bidder, a reporter's share of a penalty).
export interface CoinbaseRole {
  /** In the Type column and the type filter. */
  label: string;
  /** The source the details of a transaction give. */
  source: string;
}

export const COINBASE_ROLES: Record<string, CoinbaseRole> = {};
