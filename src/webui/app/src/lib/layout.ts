// The layouts of the main window: the same pages and actions, arranged two ways
// (doc/webui-layouts.md). Only data here; the shells are in views/layouts.
import type { IconName } from '../components/Icon.svelte';
import { PAGES, type PageId } from './nav.svelte';
import { SIDECHAIN } from '../chain/config';
import { LOCAL_PAGES, type LocalPage } from '../chain/local';

/** The pages of the chain of its own in a section (chain/local.ts). */
function localPages(section: LocalPage['section']): PageId[] {
  return (LOCAL_PAGES as readonly LocalPage[]).filter((p) => p.section === section).map((p) => p.id as PageId);
}

export type LayoutId = 'workspace' | 'dashboard';

export interface LayoutInfo {
  id: LayoutId;
  name: string;
  icon: IconName;
  summary: string;
}

export const LAYOUTS: readonly LayoutInfo[] = [
  { id: 'workspace', name: 'Workspace tabs', icon: 'tabs', summary: 'Home and five areas across the top, each with its own tabs, and a detail panel on the right' },
  { id: 'dashboard', name: 'Dashboard', icon: 'dashboard', summary: 'A home of live cards with quick actions, and every page in the sidebar' },
];

/** The layout of a first start, and of a saved layout no longer known (the compact sidebar, gone). */
export const DEFAULT_LAYOUT: LayoutId = 'dashboard';

export function isLayout(value: unknown): value is LayoutId {
  return typeof value === 'string' && LAYOUTS.some((l) => l.id === value);
}

/** The icon of a page in the navigation: one of its own where pages share an icon in PAGES. */
const NAV_ICONS: Partial<Record<PageId, IconName>> = {
  addresses: 'book',
  messages: 'edit',
  multisig: 'key',
  timestamp: 'clock',
  crypto: 'hash',
  tools: 'tool',
  advanced: 'key',
  kit: 'tool',
};

export function pageIcon(id: PageId): IconName {
  return NAV_ICONS[id] ?? PAGES[id].icon;
}

/** A place in the app: a page, and the tab of it. */
export interface Place {
  page: PageId;
  tab?: string;
  /** The name in a tab strip, when not the title of the page. */
  label?: string;
}

/** The tab a page shows when none is asked for (to tell which sub-tab is current). */
const DEFAULT_TAB: Partial<Record<PageId, string>> = { node: 'info', settings: 'display' };

export function isAt(place: Place, page: PageId, tab: string): boolean {
  if (place.page !== page) return false;
  if (place.tab === undefined) return true;
  return place.tab === (tab || DEFAULT_TAB[page] || '');
}

export interface Area {
  id: 'home' | 'wallet' | 'chain' | 'sidechains' | 'tools' | 'node';
  label: string;
  icon: IconName;
  tabs: Place[];
}

/**
 * The workspace layout: Home, then five areas, each with its sub-tabs (four on a sidechain, whose Chain
 * area has its peg and merged mining). Every page is in one. What concerns the chain (its explorer, mining, the
 * sidechains, the peg) comes before the tools.
 */
const SIDECHAIN_AREAS: readonly Area[] = [
  // The home of live cards (views/home/Home.svelte), first in every layout.
  { id: 'home', label: 'Home', icon: 'home', tabs: [{ page: 'home' }] },
  {
    id: 'wallet',
    label: 'Wallet',
    icon: 'wallet',
    tabs: [
      { page: 'overview' },
      { page: 'send' },
      { page: 'receive' },
      { page: 'transactions', label: 'History' },
      { page: 'wallets' },
      { page: 'advanced', label: 'Advanced' },
      ...localPages('wallet').map((page) => ({ page })),
    ],
  },
  {
    id: 'chain',
    label: 'Chain',
    icon: 'block',
    tabs: [{ page: 'peg' }, { page: 'bmm' }, ...localPages('chain').map((page) => ({ page })), { page: 'explorer', label: 'Explorer' }],
  },
  { id: 'tools', label: 'Tools', icon: 'hash', tabs: [{ page: 'tools' }, ...localPages('tools').map((page) => ({ page }))] },
  {
    id: 'node',
    label: 'Node',
    icon: 'terminal',
    tabs: [
      { page: 'node', tab: 'console', label: 'Console' },
      { page: 'node', tab: 'peers', label: 'Peers' },
      { page: 'node', tab: 'info', label: 'Info' },
      { page: 'node', tab: 'traffic', label: 'Traffic' },
      { page: 'settings' },
      { page: 'kit' },
    ],
  },
];

const MAINCHAIN_AREAS: readonly Area[] = [
  // The home of live cards (views/home/Home.svelte), first in every layout.
  { id: 'home', label: 'Home', icon: 'home', tabs: [{ page: 'home' }] },
  {
    id: 'wallet',
    label: 'Wallet',
    icon: 'wallet',
    tabs: [
      { page: 'overview' },
      { page: 'send' },
      { page: 'receive' },
      { page: 'transactions', label: 'History' },
      { page: 'wallets' },
      // Sign and verify, PSBT, Multisig: the tabs of one page (with the wallet).
      { page: 'advanced', label: 'Advanced' },
    ],
  },
  {
    id: 'chain',
    label: 'Chain',
    icon: 'block',
    tabs: [{ page: 'explorer', label: 'Explorer' }, { page: 'mine' }],
  },
  {
    id: 'sidechains',
    label: 'Sidechains',
    icon: 'sidechains',
    // One page: its own tabs (Sidechains, Nodes, Proposals, Withdrawals) are the rest.
    tabs: [{ page: 'sidechains' }],
  },
  {
    id: 'tools',
    label: 'Tools',
    icon: 'hash',
    // One page: Timestamp, Proof of funds and Crypto tools are its tabs.
    tabs: [{ page: 'tools' }],
  },
  {
    id: 'node',
    label: 'Node',
    icon: 'terminal',
    tabs: [
      { page: 'node', tab: 'console', label: 'Console' },
      { page: 'node', tab: 'peers', label: 'Peers' },
      { page: 'node', tab: 'info', label: 'Info' },
      { page: 'node', tab: 'traffic', label: 'Traffic' },
      { page: 'settings' },
      { page: 'kit' },
    ],
  },
];

export const AREAS: readonly Area[] = SIDECHAIN ? SIDECHAIN_AREAS : MAINCHAIN_AREAS;

/** The area a page is in (a page in none: the wallet's). */
export function areaOf(page: PageId): Area {
  const into = PAGES[page].tabOf;
  if (into) return areaOf(into.page);
  return AREAS.find((a) => a.tabs.some((t) => t.page === page)) ?? AREAS[1];
}

export function placeLabel(place: Place): string {
  return place.label ?? PAGES[place.page].title;
}

export interface NavSection {
  title: string;
  pages: PageId[];
}

/** What a page is about, by the section of the sidebars it is in: what concerns the chain comes before the tools. */
export type PageKind = 'wallet' | 'chain' | 'tools' | 'node';

export function pageKind(page: PageId): PageKind {
  if (page === 'home') return 'wallet';
  // A tab of another page: of the kind of that page.
  const into = PAGES[page].tabOf;
  if (into) return pageKind(into.page);
  const section = NAV_SECTIONS.find((s) => s.pages.includes(page));
  return (section?.title.toLowerCase() as PageKind | undefined) ?? 'node';
}

/**
 * Whether a list of actions keeps what concerns the chain before the tools (the decision "chain before
 * tools"): no action that opens a page of the chain comes after one that opens a tool. Gives the first
 * pair out of order, or null.
 */
export function chainBeforeTools<T>(items: readonly T[], pageOf: (item: T) => PageId | undefined): { chain: T; tool: T } | null {
  let firstTool: T | undefined;
  for (const item of items) {
    const page = pageOf(item);
    if (!page) continue;
    const kind = pageKind(page);
    if (kind === 'tools' && firstTool === undefined) firstTool = item;
    if (kind === 'chain' && firstTool !== undefined) return { chain: item, tool: firstTool };
  }
  return null;
}

export interface QuickAction {
  label: string;
  icon: IconName;
  page: PageId;
  tab?: string;
  panel?: 'send' | 'receive';
}

/** The quick actions of Home: Send and Receive, then what concerns the chain, then the tools. */
export const HOME_QUICK_ACTIONS: readonly QuickAction[] = SIDECHAIN
  ? [
      { label: 'Send', icon: 'send', page: 'send', panel: 'send' },
      { label: 'Receive', icon: 'receive', page: 'receive', panel: 'receive' },
      { label: 'Deposit', icon: 'down', page: 'peg', tab: 'deposit' },
      { label: 'Withdraw', icon: 'up', page: 'peg', tab: 'withdraw' },
      { label: 'Merged mining', icon: 'merged', page: 'bmm' },
      ...(LOCAL_PAGES as readonly LocalPage[])
        .filter((p) => p.quick && p.section !== 'tools')
        .map((p): QuickAction => ({ label: p.quick!.label, icon: p.quick!.icon ?? p.icon, page: p.id as PageId, tab: p.quick!.tab })),
      { label: 'Block explorer', icon: 'explorer', page: 'explorer' },
      ...(LOCAL_PAGES as readonly LocalPage[])
        .filter((p) => p.quick && p.section === 'tools')
        .map((p): QuickAction => ({ label: p.quick!.label, icon: p.quick!.icon ?? p.icon, page: p.id as PageId, tab: p.quick!.tab })),
    ]
  : [
      { label: 'Send', icon: 'send', page: 'send', panel: 'send' },
      { label: 'Receive', icon: 'receive', page: 'receive', panel: 'receive' },
      { label: 'Deposit to a sidechain', icon: 'sidechains', page: 'sidechains' },
      { label: 'Block explorer', icon: 'explorer', page: 'explorer' },
      { label: 'Mine', icon: 'mine', page: 'mine' },
      // No Sign message nor Load PSBT here (user decision): "All actions" (the palette) reaches them.
    ];

/** The dashboard's sidebar: every page (after Home), in the sections of the areas. */
export const NAV_SECTIONS: readonly NavSection[] = [
  { title: 'Wallet', pages: ['overview', 'send', 'receive', 'transactions', 'wallets', 'advanced', ...localPages('wallet')] },
  // What concerns the chain before the tools: on a sidechain its peg and merged mining.
  { title: 'Chain', pages: SIDECHAIN ? ['peg', 'bmm', ...localPages('chain'), 'explorer'] : ['sidechains', 'mine', 'explorer'] },
  { title: 'Tools', pages: ['tools', ...localPages('tools')] },
  { title: 'Node', pages: ['node', 'settings', 'kit'] },
];
