// Where the user is: the page shown, and the dialog of the app that is open.
import type { IconName } from '../components/Icon.svelte';
import { LOCAL_PAGES, type LocalPage, type LocalPageId } from '../chain/local';

export type PageId =
  | 'overview'
  | 'send'
  | 'receive'
  | 'transactions'
  | 'sidechains'
  | 'sidechain-nodes'
  | 'mine'
  | 'explorer'
  | 'node'
  | 'settings'
  | 'addresses'
  | 'messages'
  | 'psbt'
  | 'proof'
  | 'multisig'
  | 'timestamp'
  | 'crypto'
  | 'wallets'
  | 'kit'
  | 'home'
  | 'tools'
  | 'advanced'
  // A sidechain (chain.json role "sidechain"): the peg to its mainchain, blind merged mining.
  | 'peg'
  | 'bmm'
  // The pages of the chain of its own (chain/local.ts).
  | LocalPageId;

export interface PageInfo {
  id: PageId;
  title: string;
  icon: IconName;
  /** What the page is for, in a line (tooltips, the planned page). */
  summary: string;
  /** Items of doc/webui-parity.md the page covers. */
  parity: string[];
  /** The page needs a loaded wallet. */
  wallet?: boolean;
  /** Built (false: the page says what it will do). */
  ready?: boolean;
  /** Not a page of its own any more: a tab of another page, where go() leads. */
  tabOf?: { page: PageId; tab: string };
  /** Shortcut, as written in menus. */
  shortcut?: string;
  /** Only in the navigation of Advanced mode (lib/uimode.svelte.ts): Simple mode reaches it by the palette and links. */
  advanced?: boolean;
}

export const PAGES: Record<PageId, PageInfo> = {
  overview: { id: 'overview', title: 'Overview', icon: 'overview', summary: 'Balances of the wallet and what happens on the chain', parity: ['F01', 'F02', 'F03', 'F04', 'F05'], ready: true, shortcut: 'Alt+1' },
  send: { id: 'send', title: 'Send', icon: 'send', summary: 'Send coins to one or more addresses', parity: ['G01', 'G02', 'G03', 'G04', 'G05', 'G06', 'G07', 'G08', 'B06', 'B07'], wallet: true, ready: true, shortcut: 'Alt+2' },
  receive: { id: 'receive', title: 'Receive', icon: 'receive', summary: 'Request payments, with an address, a QR code and a payment link; the receiving addresses', parity: ['H01', 'H02', 'H03', 'J02'], wallet: true, ready: true, shortcut: 'Alt+3' },
  transactions: { id: 'transactions', title: 'Transactions', icon: 'history', summary: 'Every transaction of the wallet, to search, sort and export', parity: ['I01', 'I02', 'I03', 'I04', 'I05', 'I06'], wallet: true, ready: true, shortcut: 'Alt+4' },
  sidechains: { id: 'sidechains', title: 'Sidechains', icon: 'sidechains', summary: 'Deposit to sidechains, ack proposals and vote on withdrawals', parity: ['L01', 'L02', 'L03'], shortcut: 'Alt+5', ready: true },
  'sidechain-nodes': { id: 'sidechain-nodes', title: 'Sidechain nodes', icon: 'nodes', summary: 'Install, start and stop the nodes of sidechains, and watch them', parity: ['L04'], ready: true, tabOf: { page: 'sidechains', tab: 'nodes' } },
  mine: { id: 'mine', title: 'Mine', icon: 'mine', summary: 'Mine blocks with the processor of this computer', parity: ['L05'], ready: true, advanced: true },
  explorer: { id: 'explorer', title: 'Block explorer', icon: 'explorer', summary: 'Browse blocks and the transactions waiting to be mined', parity: ['L06'], ready: true },
  node: { id: 'node', title: 'Node window', icon: 'terminal', summary: 'Information, console, network traffic and peers of this node', parity: ['M01', 'M02', 'M03', 'M04', 'M05'], ready: true, shortcut: 'Ctrl+Shift+D', advanced: true },
  settings: { id: 'settings', title: 'Settings', icon: 'settings', summary: 'Options of the node, the wallet, the network, the window and the look', parity: ['N01', 'N02', 'N03', 'N04', 'N05', 'N06', 'T01', 'T02', 'T03', 'T04'], ready: true },
  addresses: { id: 'addresses', title: 'Receiving addresses', icon: 'text', summary: 'The receiving addresses of the wallet: labels, what each received, new ones', parity: ['J02'], wallet: true, ready: true, tabOf: { page: 'receive', tab: 'addresses' } },
  messages: { id: 'messages', title: 'Sign and verify', icon: 'shield', summary: 'Sign a message with an address of the wallet, or check a signature', parity: ['K01', 'K02'], wallet: true, ready: true, tabOf: { page: 'advanced', tab: 'messages' } },
  psbt: { id: 'psbt', title: 'Partially signed transactions', icon: 'paste', summary: 'Load, sign and broadcast a PSBT from a file or the clipboard', parity: ['K03'], wallet: true, ready: true, tabOf: { page: 'advanced', tab: 'psbt' } },
  proof: { id: 'proof', title: 'Proof of funds', icon: 'shield', summary: 'Prove that this wallet holds funds, or check such a proof', parity: ['L08'], wallet: true, ready: true, tabOf: { page: 'tools', tab: 'proof' } },
  multisig: { id: 'multisig', title: 'Multisig lounge', icon: 'peers', summary: 'Share coins with partners, in addresses that need several signatures', parity: ['L09'], wallet: true, ready: true, tabOf: { page: 'advanced', tab: 'multisig' } },
  timestamp: { id: 'timestamp', title: 'Timestamp a file', icon: 'block', summary: 'Prove that a file existed, by publishing its fingerprint on the chain', parity: ['L07'], ready: true, tabOf: { page: 'tools', tab: 'timestamp' } },
  crypto: { id: 'crypto', title: 'Crypto tools', icon: 'qr', summary: 'Hash calculator, Merkle tree and address decoder', parity: ['L10'], ready: true, tabOf: { page: 'tools', tab: 'crypto' } },
  wallets: { id: 'wallets', title: 'Wallets', icon: 'wallet', summary: 'Create, open, close, back up, restore, migrate and encrypt wallets', parity: ['E01', 'E02', 'E03', 'E04', 'E05', 'E06', 'E07', 'E08', 'E09', 'E10'], ready: true },
  kit: { id: 'kit', title: 'Interface kit', icon: 'overview', summary: 'The shared components of the interface, for testing them', parity: [], ready: true, advanced: true },
  // One page for the tools, one for the advanced transactions (user decision, 2026-10-15): the pages that
  // were apart are their tabs (tabOf).
  tools: { id: 'tools', title: 'Tools', icon: 'hash', summary: 'Timestamp a file, prove the funds of the wallet, and the crypto tools (hash, Merkle tree, address decoder)', parity: ['L07', 'L08', 'L10'], ready: true, advanced: true },
  advanced: { id: 'advanced', title: 'Advanced transactions', icon: 'key', summary: 'Sign and verify messages, partially signed transactions (PSBT), and the multisig lounge', parity: ['K01', 'K02', 'K03', 'L09'], wallet: true, ready: true, advanced: true },
  home: { id: 'home', title: 'Home', icon: 'home', summary: 'Live cards of the wallet, the chain, the sidechains and the node', parity: [], ready: true },
  // The pages of a sidechain (doc/webui.md): the peg (deposits from the mainchain, withdrawals to it and
  // their bundles) and blind merged mining. Only in the app of a sidechain (pageAvailable).
  // Shown as Drivechains (user decision, 2026-10-15); its id stays peg.
  peg: { id: 'peg', title: 'Drivechains', icon: 'peg', summary: 'Deposit from the mainchain, withdraw to it, and follow the withdrawal bundles and their votes', parity: ['S01', 'S02', 'S03'], shortcut: 'Alt+5', ready: true },
  // A tab of Drivechains (user decision, 2026-10-15), next to Bundle; its id leads there.
  bmm: { id: 'bmm', title: 'Merged mining', icon: 'merged', summary: 'Have the miners of the mainchain mine the blocks of this chain, for its fees (blind merged mining)', parity: ['S04'], ready: true, tabOf: { page: 'peg', tab: 'bmm' }, advanced: true },
  ...(Object.fromEntries(
    (LOCAL_PAGES as readonly LocalPage[]).map((p) => [p.id, { id: p.id, title: p.title, icon: p.icon, summary: p.summary, wallet: p.wallet, shortcut: p.shortcut, parity: [], ready: true, advanced: p.advanced }]),
  ) as unknown as Record<LocalPageId, PageInfo>),
};

export type DialogId = 'about' | 'help' | 'uri' | 'sync' | null;

export const nav = $state({
  page: 'overview' as PageId,
  /** The tab of a page that has tabs (node window, settings). */
  tab: '' as string,
  dialog: null as DialogId,
  /** A payment URI waiting for the send page. */
  pendingUri: null as string | null,
  /** Counts the payment URIs given (the send page takes each once; pendingUri stays as the last one). */
  uriSeq: 0,
  /** The last of them the send page took. */
  uriHandled: 0,
  /** A transaction to show selected on the transactions page (after sending, from the overview). */
  highlightTx: null as string | null,
  /** A row the page opened next selects (a peer id, a sidechain slot), from the dashboard or the detail panel. */
  focus: null as { page: PageId; key: string } | null,
});

export function go(page: PageId, tab = ''): void {
  // A page that became a tab of another one (Sidechain nodes: a tab of Sidechains; Addresses: of Receive;
  // Sign and verify, PSBT, Multisig: of Advanced transactions; Timestamp, Proof of funds, Crypto tools: of
  // Tools). Its own tab follows the tab of the page: "psbt/file", "crypto/merkle".
  const into = PAGES[page].tabOf;
  if (into) {
    nav.page = into.page;
    nav.tab = tab ? `${into.tab}/${tab}` : into.tab;
    return;
  }
  nav.page = page;
  nav.tab = tab;
}

/**
 * The tab of a page, also when it is shown as a tab of another one (then the part of nav.tab after
 * "<its tab>/"): what the page reads instead of nav.tab.
 */
export function ownTab(page: PageId): string {
  const into = PAGES[page].tabOf;
  if (!into || nav.page === page) return nav.tab;
  if (nav.page !== into.page) return '';
  if (nav.tab === into.tab) return '';
  return nav.tab.startsWith(`${into.tab}/`) ? nav.tab.slice(into.tab.length + 1) : '';
}

/** Set the tab of a page (see ownTab). */
export function setOwnTab(page: PageId, tab: string): void {
  const into = PAGES[page].tabOf;
  if (!into || nav.page === page) nav.tab = tab;
  else nav.tab = tab ? `${into.tab}/${tab}` : into.tab;
}

/** The tab of the page that holds others (Tools, Advanced transactions): the part of nav.tab before "/". */
export function outerTab(): string {
  const i = nav.tab.indexOf('/');
  return i < 0 ? nav.tab : nav.tab.slice(0, i);
}

/** Open a page with a row of it selected (the page takes nav.focus once). */
export function goTo(page: PageId, tab: string, key: string): void {
  go(page, tab);
  nav.focus = { page, key };
}

/** The row the page was opened for, once (null when none). */
export function takeFocus(page: PageId): string | null {
  if (nav.focus?.page !== page) return null;
  const key = nav.focus.key;
  nav.focus = null;
  return key;
}
