// The pages of this chain of its own, beside those of the kit: the one file of the interface (with
// LocalPages.svelte, which shows them, and chain.json) that a chain made from the template changes, so
// that merges of the template stay clean. See doc/webui.md, "Pages of a chain of its own".
//
// zSide: the shielded wallet (the two balances, paying privately with a memo, shielding and unshielding,
// the shielded addresses, the viewing keys), the memos received, the history and the notes of the shielded
// account, melt & cast, and the shielded pool of the chain; their components in LocalPages.svelte
// (chain/pages, chain/zs), their RPC methods in src/webui/local.cpp, the cards of the home in
// LocalCards.svelte, their icons in icons.ts.
import type { IconName } from '../components/Icon.svelte';

export interface LocalPage {
  /** Stable: the page id (go('names')), the command page.<id>. */
  id: string;
  title: string;
  icon: IconName;
  /** What the page is for, in a line (tooltips, the palette). */
  summary: string;
  /**
   * The section of the sidebars and the area of the workspace it goes in. What concerns the chain comes
   * before the tools, whatever the section (the chainBeforeTools tests).
   */
  section: 'wallet' | 'chain' | 'tools';
  /** The page needs a loaded wallet. */
  wallet?: boolean;
  shortcut?: string;
  /** More words to find it by in the palette. */
  keywords?: string;
  /** A quick action of the dashboard's home that opens it (after Send and Receive, with the chain's). */
  quick?: { label: string; icon?: IconName; tab?: string };
  /** Only in the navigation of Advanced mode (lib/uimode.svelte.ts): Simple mode reaches it by the palette and links. */
  advanced?: boolean;
}

export const LOCAL_PAGES = [
  { id: 'private', title: 'Private', icon: 'private', section: 'chain', summary: 'The shielded balance: pay privately with a memo, shield and unshield, shielded addresses, viewing keys', wallet: true, shortcut: 'Alt+6', keywords: 'shielded zside orchard pay privately shield unshield memo address viewing key', quick: { label: 'Pay privately', icon: 'private', tab: 'pay' } },
  { id: 'memos', title: 'Memos', icon: 'memo', section: 'chain', summary: 'The memos the shielded account received, to read', wallet: true, keywords: 'inbox messages notes memo received', advanced: true },
  { id: 'zhistory', title: 'Private activity', icon: 'zhistory', section: 'chain', summary: 'The transactions and the notes of the shielded account', wallet: true, keywords: 'shielded history transactions notes bump fee', advanced: true },
  { id: 'meltcast', title: 'Melt & cast', icon: 'meltcast', section: 'chain', summary: 'Shield and unshield in steps that leave nothing to link', wallet: true, keywords: 'melt cast bills unlinkable plan schedule', advanced: true },
  { id: 'pool', title: 'Shielded pool', icon: 'pool', section: 'chain', summary: 'What the shielded pool of the chain holds, and its tree of notes', keywords: 'orchard pool notes nullifiers tree root index', advanced: true },
] as const satisfies readonly LocalPage[];

export type LocalPageId = (typeof LOCAL_PAGES)[number]['id'];

/** Items of the menus (the Menus button and the palette) of this chain: in the group 'Chains' (before the tools). */
export interface LocalCommand {
  id: string;
  label: string;
  page: LocalPageId;
  tab?: string;
  tip?: string;
  keywords?: string;
}

export const LOCAL_COMMANDS = [
  { id: 'zs.pay', label: 'Pay privately…', page: 'private', tab: 'pay', tip: 'Pay out of the shielded balance, with a memo only the recipient reads', keywords: 'send shielded memo' },
  { id: 'zs.move', label: 'Shield or unshield…', page: 'private', tab: 'move', tip: 'Move coins between the transparent and the shielded balances', keywords: 'shield unshield' },
  { id: 'zs.receive', label: 'Shielded address', page: 'private', tab: 'receive', tip: 'An address to be paid at privately', keywords: 'receive address zo' },
  { id: 'zs.keys', label: 'Viewing keys…', page: 'private', tab: 'keys', tip: 'Export the viewing key of the account, or watch one', keywords: 'viewing key export import watch auditor' },
  { id: 'zs.melt', label: 'Melt my transparent coins…', page: 'meltcast', tab: 'melt', tip: 'Shield every transparent coin alone, over the next blocks', keywords: 'melt' },
  { id: 'zs.cast', label: 'Cast as bills…', page: 'meltcast', tab: 'cast', tip: 'Unshield as standard bills, at the blocks every wallet uses', keywords: 'cast bills' },
] as const satisfies readonly LocalCommand[];
