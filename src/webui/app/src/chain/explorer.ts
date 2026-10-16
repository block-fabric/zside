// The block explorer of this chain (pages/Explorer.svelte): columns of its own after the height, facts at the top
// of a block's details, a tag on the boxes of the ribbon, columns of the kit it has no use for, the block target
// the ribbon compares with. Each reads the block as getblock (verbosity 1) gives it, without its transactions;
// `refresh` is called with each new tip while the explorer is shown, for what the columns read besides the blocks.
//
// The template has none of its own: the kit's columns, the coinbase's tag, chain.json's block target. A
// sidechain with blocks of its own kinds says them here (FastBlocks: the kind, slot, producer and finality of
// each block).

export type ChainBlock = Record<string, unknown>;

export interface ChainColumn {
  key: string;
  label: string;
  width: string;
  align?: 'start' | 'end';
  /** The value (what is sorted and shown); undefined while the block is not read. */
  value: (block: ChainBlock) => string | number | undefined;
  /** A badge of the value: its class (badge <tone>), and the words of its tooltip. */
  badge?: (block: ChainBlock) => { tone: string; tip: string } | undefined;
  /** The tooltip of a cell that is not a badge. */
  tip?: (block: ChainBlock) => string | undefined;
  mono?: boolean;
}

export interface ChainFact {
  /** Its term and its description, words of the app (made with t()). */
  dt: string;
  dd: string;
  mono?: boolean;
}

export interface ChainExplorer {
  columns: ChainColumn[];
  /** Columns of the kit's list of blocks this chain leaves out (by key). */
  hide: string[];
  facts: (block: ChainBlock) => ChainFact[];
  /** The tag of a block's box in the ribbon, instead of its coinbase's; undefined: the coinbase's. */
  tag?: (block: ChainBlock) => string | undefined;
  /** The time between blocks the chain aims at, in seconds, as the node says it; undefined: chain.json's blockTarget. */
  target?: () => number | undefined;
  refresh?: () => Promise<void>;
}

export const EXPLORER: ChainExplorer = {
  columns: [],
  hide: [],
  facts: () => [],
};
