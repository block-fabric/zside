import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { buildCommands, MENU_GROUPS, menuSections, pageAvailable, palette, searchCommands, setLayout } from '../lib/commands.svelte';
import { closeDetail, detail } from '../lib/detail.svelte';
import { handleShortcut } from '../lib/keyboard';
import { AREAS, chainBeforeTools, DEFAULT_LAYOUT, HOME_QUICK_ACTIONS, LAYOUTS, NAV_SECTIONS, pageKind } from '../lib/layout';
import { go, nav, PAGES, type PageId } from '../lib/nav.svelte';
import { app, applyPrefs, node, prefs, wallets } from '../lib/state.svelte';
import { resetWalletData } from '../lib/walletdata.svelte';
import Shell from '../views/Shell.svelte';
import { CHAIN, SIDECHAIN } from '../chain/config';
import { chainHas } from '../lib/commands.svelte';
import { LOCAL_PAGES, type LocalPage } from '../chain/local';

/** The items of the old menus that are about the sidechains of the mainchain: not in the app of a sidechain. */
const MAINCHAIN_ONLY = new Set(['Sidechains', 'Sidechain nodes', 'Solo mine', 'Sidechain proposals', 'Withdrawal votes']);
/** The quick actions of the home, in their order: the chain's (its sidechains and mining, or its peg) between Send and Receive and the tools. */
const localQuick = (tools: boolean) => (LOCAL_PAGES as readonly LocalPage[]).filter((p) => p.quick && (p.section === 'tools') === tools).map((p) => p.quick!.label);
const QUICK = SIDECHAIN
  ? ['Send', 'Receive', 'Deposit', 'Withdraw', 'Merged mining', ...localQuick(false), 'Block explorer', ...localQuick(true)]
  : ['Send', 'Receive', 'Deposit to a sidechain', 'Block explorer', 'Mine'];

/** The menu bar of chains-qt as views/Shell.svelte had it before the layouts: every item must stay reachable. */
const OLD_MENUS: Record<string, string[]> = {
  'Your wallet': [
    'Create wallet…',
    'Open wallet…',
    'Close wallet…',
    'Close all wallets…',
    'Migrate wallet…',
    'Restore and migrate wallet file…',
    // 'Sending addresses' is gone (user decision, 2026-10-15: no sending address book; tested below).
    'Receiving addresses',
    'Backup wallet…',
    'Restore wallet…',
    'Export watch-only wallet…',
    'Encrypt wallet…',
    'Change passphrase…',
    'Unlock wallet for a while…',
    'Mask values',
  ],
  Banking: ['Send', 'Receive', 'Transactions', 'Open URI…', 'Proof of funds', 'Multisig lounge', 'Load PSBT from file…', 'Load PSBT from clipboard…'],
  'Use Chains': ['Timestamp file', 'Sign message…', 'Verify message…', 'Sidechains'],
  Chains: ['Sidechain nodes', 'Solo mine', 'Sidechain proposals', 'Withdrawal votes'],
  'Crypto tools': ['Block explorer', 'Hash calculator', 'Merkle tree', 'Address decoder'],
  Window: ['Minimize', 'Information', 'Console', 'Network traffic', 'Peers'],
  'This node': [
    'Node window',
    'Options…',
    'Theme: Same as the desktop',
    'Theme: Light',
    'Theme: Dark',
    'Theme: Ocean Teal',
    'Theme: Matrix Green',
    'Theme: Dark (classic Drivechain)',
    'Theme: Custom…',
    'Increase font size',
    'Decrease font size',
    'Command-line options',
    `About ${CHAIN.name}`,
    'Exit',
  ],
};

const calls: BridgeRequest[] = [];

/** This node's own mining as getgenerate tells it (null: the call failed; off by default). */
let generating: { generate: boolean; threads: number; hashespersec: number } | null = { generate: false, threads: 0, hashespersec: 0 };

/** The node as the pages and cards ask it. */
async function fakeNode(r: BridgeRequest): Promise<unknown> {
  calls.push(r);
  switch (r.method) {
    case 'listbanned':
      return [];
    case 'getpeerinfo':
      return [{ id: 7, addr: '10.0.0.7:8333', inbound: false, pingtime: 0.05, synced_blocks: 10, network: 'ipv4', connection_type: 'outbound-full-relay', subver: '/Chains:32/', version: 70016, startingheight: 1, synced_headers: 10, bytessent: 1, bytesrecv: 2, conntime: 1, lastsend: 1, lastrecv: 1 }];
    case 'getblockheader':
      return { height: 10, time: 1_700_000_000, nTx: 1 };
    case 'getgenerate':
      return generating;
    case 'getmininginfo':
      return { blocks: 10, difficulty: 1, networkhashps: 12345, pooledtx: 0, chain: 'regtest', next: { height: 11, bits: '207fffff', difficulty: 1, target: '7f'.padEnd(64, 'f') } };
    case 'getdrivechaininfo':
      return { activesidechains: 1, proposals: 0, pendingbundles: 0, escrowtotal: 2.5, withdrawalperiod: 100, maxsidechains: 256, withdrawalminscore: 13150 };
    case 'listsidechainproposals':
      return { pending: [], queued: [] };
    case 'listsidechaindeposits':
      return [];
    case 'listactivesidechains':
      return [{ slot: 2, title: 'Thunder', description: 'Fast payments', version: 0, hashid1: '00', hashid2: '00', activationheight: 5, escrow: { txid: 'aa', n: 0, amount: 2.5 }, pendingbundles: 0 }];
    case 'listwithdrawalbundles':
    case 'getsidechainevents':
    case 'wallet.addresses':
      return [];
    case 'getblock':
      return { hash: r.params && (r.params as unknown[])[0], height: 10, time: 1_700_000_000, confirmations: 1, nTx: 1, size: 200, weight: 800, difficulty: 1, tx: ['cc'.repeat(32)] };
    case 'wallet.info':
      return { name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, externalSigner: false, hd: true, canGetAddresses: true, taproot: true, defaultAddressType: 'bech32', confirmTarget: 6, requiredFeePerK: 1000, maxTxFee: 10_000_000, multiwallet: false, balance: 1, unconfirmed: 0, immature: 0 };
    case 'getbalances':
      return { mine: { trusted: 1, untrusted_pending: 0, immature: 0 } };
    case 'wallet.transactions':
      return { height: 10, hash: 'h', records: [['aa'.repeat(32), 0, 'recv', 'bcrt1qexample', 'salary', 1_700_000_000, 250_000_000, 0, 'confirmed', 7, 0, true, 'k1']] };
    case 'wallet.transaction':
      return { txid: 'aa'.repeat(32), coinbase: false, rows: [{ k: 'Status', text: '7 confirmations' }], notes: [], ids: [{ k: 'Transaction ID', text: 'aa'.repeat(32) }], messages: [] };
    default:
      return null;
  }
}

beforeAll(() => {
  const proto = HTMLDialogElement.prototype as unknown as Record<string, unknown>;
  proto.showModal ??= function (this: HTMLDialogElement) {
    this.setAttribute('open', '');
  };
  proto.close ??= function (this: HTMLDialogElement) {
    this.removeAttribute('open');
  };
  window.matchMedia ??= ((query: string) => ({ matches: false, media: query, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia;
  Element.prototype.scrollIntoView ??= function () {};
});

beforeEach(() => {
  app.info = { name: CHAIN.name, version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp', walletEnabled: true, devtools: false, test: false, platform: 'linux', blockSpacing: 600 };
  prefs.mask = false;
  prefs.loaded = true;
  node.tip = { height: 10, time: 1_700_000_000, hash: 'ab'.repeat(32), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
  wallets.current = 'w';
  wallets.balances.w = { trusted: 100_000_000n, untrustedPending: 0n, immature: 0n };
  calls.length = 0;
  setTransport(fakeNode);
  go('overview');
});

afterEach(() => {
  setTransport(null);
  palette.open = false;
  closeDetail();
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
  prefs.layout = 'dashboard';
});

describe('the commands', () => {
  it('keep every item of the old menu bar, in its menu', () => {
    // Every item of the old menus is still in a menu (the menus were regrouped: chain before tools).
    const all = buildCommands().filter((c) => (MENU_GROUPS as readonly string[]).includes(c.group));
    for (const [menu, labels] of Object.entries(OLD_MENUS)) {
      for (const label of labels) if (!(SIDECHAIN && MAINCHAIN_ONLY.has(label))) expect(all.map((c) => c.label), `${menu} → ${label}`).toContain(label);
    }
    const groups = [...MENU_GROUPS] as string[];
    expect(groups.indexOf('Chains')).toBeLessThan(groups.indexOf('Tools'));
    const chains = all.filter((c) => c.group === 'Chains').map((c) => c.label);
    for (const label of SIDECHAIN ? ['Peg', 'Deposit…', 'Withdraw…', 'Merged mining', 'Block explorer'] : ['Sidechains', 'Sidechain nodes', 'Solo mine', 'Block explorer']) expect(chains).toContain(label);
    // A sidechain has none of the items of the sidechains of the mainchain.
    if (SIDECHAIN) for (const label of MAINCHAIN_ONLY) expect(all.map((c) => c.label), label).not.toContain(label);
  });

  it('reach every page, and every page is in each layout', () => {
    const all = buildCommands();
    const pages = (Object.keys(PAGES) as PageId[]).filter((p) => p !== 'kit' && p !== 'home' && !PAGES[p].tabOf && chainHas(p));
    for (const p of pages) {
      expect(all.some((c) => c.id === `page.${p}`), p).toBe(true);
      expect(AREAS.some((a) => a.tabs.some((t) => t.page === p)), `workspace: ${p}`).toBe(true);
      expect(NAV_SECTIONS.some((s) => s.pages.includes(p)), `sidebars: ${p}`).toBe(true);
    }
    // The compact sidebar is gone (user decision): two layouts.
    expect(LAYOUTS.map((l) => l.id)).toEqual(['workspace', 'dashboard']);
    // Home in every layout: the workspace's first area, the dashboard's first page.
    expect(AREAS[0]).toMatchObject({ id: 'home', tabs: [{ page: 'home' }] });
    for (const layout of ['workspace', 'dashboard'] as const) {
      prefs.layout = layout;
      expect(pageAvailable('home'), layout).toBe(true);
      expect(buildCommands().some((c) => c.id === 'page.home'), layout).toBe(true);
    }
    expect(buildCommands().some((c) => c.id.startsWith('layout.sidebar') || /compact/i.test(c.label))).toBe(false);
  });

  it.skipIf(SIDECHAIN)('sidechain nodes: a tab of the Sidechains page, not a page of its own', () => {
    expect(PAGES['sidechain-nodes'].tabOf).toEqual({ page: 'sidechains', tab: 'nodes' });
    expect(NAV_SECTIONS.some((s) => s.pages.includes('sidechain-nodes'))).toBe(false);
    expect(AREAS.some((a) => a.tabs.some((t) => t.page === 'sidechain-nodes'))).toBe(false);
    // Its command, the old way to it and the dashboard's link all lead to the tab.
    const all = buildCommands();
    expect(all.some((c) => c.id === 'page.sidechain-nodes')).toBe(false);
    all.find((c) => c.label === 'Sidechain nodes')!.run();
    expect([nav.page, nav.tab]).toEqual(['sidechains', 'nodes']);
    go('overview');
    go('sidechain-nodes');
    expect([nav.page, nav.tab]).toEqual(['sidechains', 'nodes']);
  });

  it('receiving addresses: a tab of the Receive page; no sending addresses (user decision)', () => {
    expect(PAGES.addresses.tabOf).toEqual({ page: 'receive', tab: 'addresses' });
    expect(NAV_SECTIONS.some((s) => s.pages.includes('addresses'))).toBe(false);
    expect(AREAS.some((a) => a.tabs.some((t) => t.page === 'addresses'))).toBe(false);
    const all = buildCommands();
    expect(all.some((c) => c.id === 'page.addresses')).toBe(false);
    // No sending addresses anywhere: not in the menus, not in the palette.
    expect(all.some((c) => /sending address/i.test(c.label))).toBe(false);
    expect(searchCommands(all, 'sending addresses').some((c) => /sending/i.test(c.label))).toBe(false);
    // Receiving addresses, from the menu, the palette and the old page id, open the tab.
    all.find((c) => c.label === 'Receiving addresses')!.run();
    expect([nav.page, nav.tab]).toEqual(['receive', 'addresses']);
    go('overview');
    searchCommands(all, 'receiving addresses')[0].run();
    expect([nav.page, nav.tab]).toEqual(['receive', 'addresses']);
    go('overview');
    go('addresses');
    expect([nav.page, nav.tab]).toEqual(['receive', 'addresses']);
  });

  it('receiving addresses: the Receive page shows its Addresses tab', async () => {
    prefs.layout = 'dashboard';
    render(Shell);
    go('receive', 'addresses');
    const tab = await screen.findByRole('tab', { name: 'Addresses' });
    expect(tab.getAttribute('aria-selected')).toBe('true');
    expect(await screen.findByRole('button', { name: /Generate new address/ })).toBeTruthy();
    expect(screen.getByRole('grid', { name: 'Receiving addresses' })).toBeTruthy();
    await fireEvent.click(screen.getByRole('tab', { name: 'Requests' }));
    expect(nav.tab).toBe('');
    expect(await screen.findByRole('button', { name: /Create new receiving address/ })).toBeTruthy();
  });

  it('tools: one page, its tabs Timestamp, Proof of funds, Crypto tools (user decision)', () => {
    for (const id of ['timestamp', 'proof', 'crypto'] as PageId[]) expect(PAGES[id].tabOf, id).toEqual({ page: 'tools', tab: id });
    expect(NAV_SECTIONS.find((s) => s.title === 'Tools')!.pages).toEqual(['tools']);
    expect(AREAS.find((a) => a.id === 'tools')!.tabs.map((t) => t.page)).toEqual(['tools']);
    const all = buildCommands();
    for (const id of ['timestamp', 'proof', 'crypto']) expect(all.some((c) => c.id === `page.${id}`), id).toBe(false);
    expect(all.some((c) => c.id === 'page.tools')).toBe(true);
    // Every old item opens its tab (and the tab of its own).
    const cases: [string, string][] = [
      ['Timestamp file', 'timestamp'],
      ['Proof of funds', 'proof'],
      ['Hash calculator', 'crypto/hash'],
      ['Merkle tree', 'crypto/merkle'],
      ['Address decoder', 'crypto/address'],
    ];
    for (const [label, tab] of cases) {
      go('overview');
      all.find((c) => c.label === label && c.group !== 'Go to')!.run();
      expect([nav.page, nav.tab], label).toEqual(['tools', tab]);
    }
  });

  it('advanced transactions: one page, its tabs Sign & verify, PSBT, Multisig, with the wallet (user decision)', () => {
    for (const id of ['messages', 'psbt', 'multisig'] as PageId[]) expect(PAGES[id].tabOf, id).toEqual({ page: 'advanced', tab: id });
    const wallet = NAV_SECTIONS.find((s) => s.title === 'Wallet')!.pages;
    expect(wallet.indexOf('advanced')).toBeGreaterThan(wallet.indexOf('transactions'));
    expect(wallet.indexOf('advanced')).toBeGreaterThan(wallet.indexOf('wallets'));
    expect(AREAS.find((a) => a.id === 'wallet')!.tabs.some((t) => t.page === 'advanced' && t.label === 'Advanced')).toBe(true);
    const all = buildCommands();
    for (const id of ['messages', 'psbt', 'multisig']) expect(all.some((c) => c.id === `page.${id}`), id).toBe(false);
    expect(all.some((c) => c.id === 'page.advanced')).toBe(true);
    const cases: [string, string][] = [
      ['Sign message…', 'messages/sign'],
      ['Verify message…', 'messages/verify'],
      ['Load PSBT from file…', 'psbt/file'],
      ['Load PSBT from clipboard…', 'psbt/clipboard'],
      ['Multisig lounge', 'multisig'],
    ];
    for (const [label, tab] of cases) {
      go('overview');
      all.find((c) => c.label === label)!.run();
      expect([nav.page, nav.tab], label).toEqual(['advanced', tab]);
    }
  });

  it('tools and advanced transactions: the pages draw the tab asked for', async () => {
    prefs.layout = 'dashboard';
    render(Shell);
    go('crypto', 'merkle');
    const tools = await screen.findByRole('tablist', { name: 'Tools' });
    expect(within(tools).getByRole('tab', { name: 'Crypto tools' }).getAttribute('aria-selected')).toBe('true');
    expect((await screen.findByRole('tab', { name: 'Merkle Tree' })).getAttribute('aria-selected')).toBe('true');
    go('messages', 'verify');
    const adv = await screen.findByRole('tablist', { name: 'Advanced transactions' });
    expect(within(adv).getByRole('tab', { name: 'Sign & verify' }).getAttribute('aria-selected')).toBe('true');
    await waitFor(() => expect(screen.getByRole('tab', { name: 'Verify Message' }).getAttribute('aria-selected')).toBe('true'));
    await fireEvent.click(within(adv).getByRole('tab', { name: 'Multisig' }));
    expect([nav.page, nav.tab]).toEqual(['advanced', 'multisig']);
  });

  it('dashboard: the wallet is switched in the sidebar card only, and the unit in the status bar only', () => {
    prefs.layout = 'dashboard';
    render(Shell);
    const side = screen.getByRole('navigation', { name: 'Pages' });
    const card = within(side).getByRole('region', { name: 'Wallet shown' });
    // No wallet choice in the top bar beside the search.
    const top = document.querySelector('.content-head, header.top, .dash-head') ?? document.body;
    const outside = Array.from(document.querySelectorAll('select')).filter((el) => !card.contains(el) && el.querySelector('option') && /wallet/i.test(el.getAttribute('title') ?? '') && !el.closest('.statusbar, footer'));
    expect(outside.length, top.className).toBe(0);
    // One unit chooser: in the status bar.
    expect(within(card).queryByRole('combobox', { name: 'Unit of amounts' })).toBeNull();
  });

  it('are found by name, words in any order, and run', () => {
    const all = buildCommands();
    expect(searchCommands(all, 'sign message')[0].label).toBe('Sign message…');
    expect(searchCommands(all, 'sign').map((c) => c.label)).toContain('Sign message…');
    expect(searchCommands(all, 'clipboard psbt')[0].id).toBe('banking.psbtClipboard');
    expect(searchCommands(all, 'peers')[0].label).toBe('Peers');
    expect(searchCommands(all, 'zzzz')).toEqual([]);
    searchCommands(all, 'merkle')[0].run();
    expect([nav.page, nav.tab]).toEqual(['tools', 'crypto/merkle']);
  });

  it('switch the layout, the page shown staying (Home is in every layout)', () => {
    prefs.layout = 'workspace';
    go('home');
    setLayout('dashboard');
    expect(prefs.layout).toBe('dashboard');
    expect(nav.page).toBe('home');
    setLayout('workspace');
    expect(nav.page).toBe('home');
    go('overview');
    setLayout('dashboard');
    expect(nav.page).toBe('overview');
    expect(buildCommands().find((c) => c.id === 'layout.dashboard')?.checked).toBe(true);
  });

  it('keep the layout in the preferences', () => {
    applyPrefs({ layout: 'dashboard' });
    expect(prefs.layout).toBe('dashboard');
    applyPrefs({ layout: 'workspace' });
    expect(prefs.layout).toBe('workspace');
    // The compact sidebar is gone: a saved 'sidebar', or any layout not known, is the dashboard.
    applyPrefs({ layout: 'sidebar' });
    expect(prefs.layout).toBe('dashboard');
    prefs.layout = 'workspace';
    applyPrefs({ layout: 'nonsense' });
    expect(prefs.layout).toBe('dashboard');
    prefs.layout = 'workspace';
    applyPrefs({});
    expect(prefs.layout).toBe('workspace');
  });

  it('open the palette with Ctrl+K, and the node tabs with their keys', () => {
    expect(handleShortcut(new KeyboardEvent('keydown', { key: 'k', ctrlKey: true }))).toBe(true);
    expect(palette.open).toBe(true);
    handleShortcut(new KeyboardEvent('keydown', { key: 'p', ctrlKey: true }));
    expect([nav.page, nav.tab]).toEqual(['node', 'peers']);
  });
});

describe('the layouts', () => {
  it('workspace: areas, sub-tabs, the more menu, a transaction in the detail panel', async () => {
    prefs.layout = 'workspace';
    render(Shell);
    const areas = screen.getByRole('navigation', { name: 'Areas' });
    expect(within(areas).getAllByRole('button').map((b) => b.textContent?.trim())).toEqual(SIDECHAIN ? ['Home', 'Wallet', 'Chain', 'Tools', 'Node'] : ['Home', 'Wallet', 'Chain', 'Sidechains', 'Tools', 'Node']);
    // It starts on Home, as every layout.
    await waitFor(() => expect(nav.page).toBe('home'));
    expect(within(areas).getByRole('button', { name: 'Home' }).getAttribute('aria-current')).toBe('true');
    await fireEvent.click(within(areas).getByRole('button', { name: 'Wallet' }));
    const subtabs = screen.getByRole('navigation', { name: 'Wallet pages' });
    expect(within(subtabs).getByRole('button', { name: 'Overview' }).getAttribute('aria-current')).toBe('page');
    await fireEvent.click(within(areas).getByRole('button', { name: 'Node' }));
    expect(nav.page).toBe('node');
    expect(screen.getByRole('navigation', { name: 'Node pages' })).toBeTruthy();
    // The menus of chains-qt, in one menu.
    await fireEvent.click(screen.getByRole('button', { name: /^Menus of chains-qt/ }));
    const menu = await waitFor(() => screen.getByRole('menu'));
    expect(within(menu).getByRole('menuitem', { name: 'Hash calculator' })).toBeTruthy();
    await fireEvent.click(within(menu).getByRole('menuitem', { name: 'Hash calculator' }));
    expect([nav.page, nav.tab]).toEqual(['tools', 'crypto/hash']);
    // The history: a click on a row shows it in the panel.
    go('transactions');
    const cell = await waitFor(() => screen.getByText('salary'));
    await fireEvent.click(cell.closest('[role="row"]')!);
    await waitFor(() => expect(detail.item?.kind).toBe('tx'));
    const panel = await waitFor(() => screen.getByRole('complementary', { name: 'Transaction details' }));
    await waitFor(() => expect(panel.textContent).toContain('7 confirmations'));
  });

  // The home of a sidechain has cards of its own: test/sidechain.test.ts.
  it.skipIf(SIDECHAIN)('dashboard: live cards that open their pages and the panel', async () => {
    prefs.layout = 'dashboard';
    go('home');
    render(Shell);
    // The summary row, the chart, merged mining.
    for (const title of ['Balance', 'Network', 'Sidechains', 'Mining', 'Wallet balance', 'Blind merged mining']) {
      expect(screen.getByRole('heading', { name: title })).toBeTruthy();
    }
    // The tables in tabs: the transactions first, with the actions of each row.
    const tabs = screen.getByRole('tablist', { name: 'Activity' });
    expect(within(tabs).getAllByRole('tab').map((t) => t.textContent?.replace(/\d+/g, '').trim())).toEqual(['Transactions', 'Sidechains', 'Blocks', 'Peers']);
    const txRow = (await waitFor(() => screen.getByRole('button', { name: /salary/ }))).closest('tr')!;
    await fireEvent.click(within(txRow).getByRole('button', { name: 'Copy transaction ID' }));
    await waitFor(() => expect(calls.some((c) => c.method === 'clipboard.write')).toBe(true));
    await fireEvent.click(within(txRow).getByRole('button', { name: 'Details' }));
    await waitFor(() => expect(detail.item?.kind).toBe('tx'));
    await fireEvent.click(within(tabs).getByRole('tab', { name: /Sidechains/ }));
    const sidechain = await waitFor(() => screen.getByRole('button', { name: /Thunder/ }));
    await fireEvent.click(sidechain);
    await waitFor(() => screen.getByRole('complementary', { name: 'Sidechain details' }));
    await fireEvent.click(within(tabs).getByRole('tab', { name: /Peers/ }));
    const peer = await waitFor(() => screen.getByRole('button', { name: /10\.0\.0\.7/ }));
    await fireEvent.click(peer);
    await waitFor(() => expect(detail.item).toEqual({ kind: 'peer', id: 7 }));
    await fireEvent.click(screen.getByRole('button', { name: 'Open mining' }));
    expect(nav.page).toBe('mine');
    // Every page in the sidebar, Home first.
    const side = screen.getByRole('navigation', { name: 'Pages' });
    expect(within(side).getAllByRole('button', { name: /Home|Overview|Wallets|^Tools|Settings/ }).length).toBeGreaterThanOrEqual(5);
  }, 20000);

  it.skipIf(SIDECHAIN)('dashboard: the Deposit and Withdrawals of a sidechain open the real tabs, the sidechain chosen', async () => {
    prefs.layout = 'dashboard';
    go('home');
    render(Shell);
    for (const [action, tab] of [['Deposit', 'sidechains'], ['Withdrawals', 'withdrawals']] as const) {
      go('home');
      const tabs = await waitFor(() => screen.getByRole('tablist', { name: 'Activity' }));
      await fireEvent.click(within(tabs).getByRole('tab', { name: /Sidechains/ }));
      const row = (await waitFor(() => screen.getByRole('button', { name: /Thunder/ }))).closest('tr')!;
      await fireEvent.click(within(row).getByRole('button', { name: action }));
      expect(nav.page).toBe('sidechains');
      expect(nav.tab).toBe(tab);
      // The Sidechains page took the sidechain it was opened for.
      await waitFor(() => expect(nav.focus).toBeNull());
      await waitFor(() => screen.getByRole('tab', { name: tab === 'sidechains' ? 'Sidechains' : 'Withdrawals', selected: true }));
    }
  }, 20000);

  it('workspace: a peer chosen in the node window shows in the detail panel, and back', async () => {
    prefs.layout = 'workspace';
    go('node', 'peers');
    render(Shell);
    const peer = await waitFor(() => screen.getByText('10.0.0.7:8333'));
    await fireEvent.click(peer.closest('[role="row"]')!);
    await waitFor(() => expect(detail.item).toEqual({ kind: 'peer', id: 7 }));
    // From the panel, the peers tab with that peer chosen.
    await fireEvent.click(await waitFor(() => screen.getByRole('button', { name: 'Open the peers tab' })));
    expect(nav.page).toBe('node');
  }, 20000);

  it('dashboard: Send and Receive in the side panel, the full pages a click away', async () => {
    prefs.layout = 'dashboard';
    go('home');
    render(Shell);
    const balance = screen.getByRole('heading', { name: 'Balance' }).closest('section')!;
    await fireEvent.click(within(balance).getByRole('button', { name: 'Send' }));
    const panel = await waitFor(() => screen.getByRole('complementary', { name: 'Send' }));
    expect(within(panel).getByLabelText('Pay to')).toBeTruthy();
    await waitFor(() => expect(within(panel).getByRole('button', { name: /^Send/ })).toBeTruthy());
    await fireEvent.click(within(panel).getByRole('tab', { name: /Receive/ }));
    const receive = await waitFor(() => screen.getByRole('complementary', { name: 'Receive' }));
    expect(within(receive).getByRole('button', { name: /Create address/ })).toBeTruthy();
    await fireEvent.click(within(receive).getByRole('button', { name: /the Receive page/ }));
    expect(nav.page).toBe('receive');
    expect(screen.queryByRole('complementary', { name: 'Receive' })).toBeNull();
  });

  it('every layout: the notifications and the light/dark switch in the top bar', async () => {
    for (const layout of ['dashboard', 'workspace'] as const) {
      prefs.layout = layout;
      prefs.theme = 'midnight';
      wallets.newTxs = [];
      const view = render(Shell);
      const toggle = screen.getByRole('button', { name: 'Switch to the light look' });
      await fireEvent.click(toggle);
      expect(prefs.theme).toBe('light');
      await fireEvent.click(screen.getByRole('button', { name: 'Switch to the dark look' }));
      expect(prefs.theme).toBe('midnight');
      expect(screen.getByRole('button', { name: 'Notifications' })).toBeTruthy();
      wallets.newTxs.push({ wallet: 'w', txid: 'aa'.repeat(32) });
      const bell = await waitFor(() => screen.getByRole('button', { name: 'Notifications: 1 new' }));
      await fireEvent.click(bell);
      expect(screen.getByRole('dialog', { name: 'Notifications' })).toBeTruthy();
      view.unmount();
      wallets.newTxs = [];
    }
  }, 30000);

  it('the sidebar: a card of the wallet, Close wallet and Quit at its foot', () => {
    prefs.layout = 'dashboard';
    render(Shell);
    const side = screen.getByRole('navigation', { name: 'Pages' });
    const card = within(side).getByRole('region', { name: 'Wallet shown' });
    expect(within(card).getByText('Not encrypted')).toBeTruthy();
    // The unit is chosen in the status bar only, not in the card.
    expect(within(card).queryByRole('combobox', { name: 'Unit of amounts' })).toBeNull();
    expect(within(side).getByRole('button', { name: /Close wallet/ })).toBeTruthy();
    expect(within(side).getByRole('button', { name: new RegExp(`Quit ${CHAIN.name}`) })).toBeTruthy();
  });

  it('workspace: Home first, its live cards, Send and Receive in the side panel', async () => {
    prefs.layout = 'workspace';
    go('home');
    render(Shell);
    // A sidechain has its own home (views/home/SideHome.svelte): the peg and merged mining.
    const titles = SIDECHAIN ? ['Balance', 'Network', 'Peg', 'Merged mining'] : ['Balance', 'Network', 'Sidechains', 'Mining', 'Wallet balance', 'Blind merged mining'];
    for (const title of titles) {
      expect(screen.getByRole('heading', { name: title })).toBeTruthy();
    }
    expect(screen.getByRole('navigation', { name: 'Quick actions' })).toBeTruthy();
    const balance = screen.getByRole('heading', { name: 'Balance' }).closest('section')!;
    await fireEvent.click(within(balance).getByRole('button', { name: 'Send' }));
    const panel = await waitFor(() => screen.getByRole('complementary', { name: 'Send' }));
    expect(within(panel).getByLabelText('Pay to')).toBeTruthy();
    // A new page closes it.
    const areas = screen.getByRole('navigation', { name: 'Areas' });
    await fireEvent.click(within(areas).getByRole('button', { name: 'Chain' }));
    expect(nav.page).toBe(SIDECHAIN ? 'peg' : 'explorer');
    await waitFor(() => expect(screen.queryByRole('complementary', { name: 'Send' })).toBeNull());
    // The logo goes home.
    await fireEvent.click(document.querySelector('header.top button.brand') as HTMLButtonElement);
    expect(nav.page).toBe('home');
  }, 20000);

  it('chain before tools, in the sidebar and the workspace', () => {
    const titles = NAV_SECTIONS.map((s) => s.title);
    expect(titles.indexOf('Chain')).toBeLessThan(titles.indexOf('Tools'));
    const areas = AREAS.map((a) => a.id);
    expect(areas.indexOf('chain')).toBeLessThan(areas.indexOf('tools'));
    expect(areas.indexOf('sidechains')).toBeLessThan(areas.indexOf('tools'));
  });

  it('chain before tools, in every list of actions and every menu', () => {
    // The helper finds a list out of order.
    expect(chainBeforeTools(['send', 'timestamp', 'explorer'] as PageId[], (p) => p)).toEqual({ chain: 'explorer', tool: 'timestamp' });
    expect(chainBeforeTools(['send', 'explorer', 'tools', 'node'] as PageId[], (p) => p)).toBeNull();
    expect(pageKind('explorer')).toBe('chain');
    expect(pageKind('tools')).toBe('tools');
    expect(pageKind('crypto')).toBe('tools');
    // Sign and verify, PSBT, Multisig: with the wallet (Advanced transactions).
    expect(pageKind('messages')).toBe('wallet');
    const lists: [string, PageId[]][] = [
      ['Home quick actions', HOME_QUICK_ACTIONS.map((q) => q.page)],
      ['sidebar sections', NAV_SECTIONS.flatMap((s) => s.pages)],
      ['workspace areas', AREAS.flatMap((a) => a.tabs.map((tb) => tb.page))],
    ];
    const all = buildCommands();
    lists.push(['Go to', all.filter((c) => c.group === 'Go to').map((c) => c.page!)]);
    for (const section of menuSections(all)) lists.push([`menu ${section.title}`, section.items.flatMap((c) => (c.page ? [c.page] : []))]);
    // The menus themselves: the one of the chain before the one of the tools.
    expect(MENU_GROUPS.indexOf('Chains')).toBeLessThan(MENU_GROUPS.indexOf('Tools'));
    for (const [name, pages] of lists) {
      expect(pages.length, name).toBeGreaterThan(0);
      expect(chainBeforeTools(pages, (p) => p), name).toBeNull();
    }
    // Home: Send and Receive first, then the chain, then the tools.
    expect(HOME_QUICK_ACTIONS.map((q) => q.label)).toEqual(QUICK);
  });

  it('dashboard: the quick actions of Home in that order, as shown', () => {
    prefs.layout = 'dashboard';
    go('home');
    render(Shell);
    const row = screen.getByRole('navigation', { name: 'Quick actions' });
    const labels = within(row).getAllByRole('button').map((b) => b.textContent?.trim() ?? '');
    // No Sign message nor Load PSBT (user decision); "All actions" stays.
    expect(labels.slice(0, QUICK.length)).toEqual(QUICK);
    expect(chainBeforeTools(labels.slice(0, QUICK.length), (l) => HOME_QUICK_ACTIONS.find((q) => q.label === l)?.page)).toBeNull();
    expect(labels.some((l) => /Sign message|Load PSBT/.test(l))).toBe(false);
    expect(within(row).getByRole('button', { name: /All actions/ })).toBeTruthy();
  });

  it('the dashboard is the default layout', () => {
    expect(DEFAULT_LAYOUT).toBe('dashboard');
  });

  it('palette: type, Enter runs the first match', async () => {
    render(Shell);
    palette.open = true;
    const input = await waitFor(() => screen.getByRole('combobox', { name: 'Search pages and actions' }));
    await fireEvent.input(input, { target: { value: 'verify' } });
    expect(within(screen.getByRole('listbox')).getAllByRole('option')[0].textContent).toContain('Verify message');
    await fireEvent.keyDown(input, { key: 'Enter' });
    expect(palette.open).toBe(false);
    expect([nav.page, nav.tab]).toEqual(['advanced', 'messages/verify']);
  });

  it.skipIf(SIDECHAIN)("the dashboard's Mining card shows this node's mining, not the network's estimate", async () => {
    prefs.layout = 'dashboard';
    generating = { generate: false, threads: 0, hashespersec: 0 };
    go('home');
    const view = render(Shell);
    const rate = await waitFor(() => {
      const el = document.querySelector('[data-testid="mining-rate"]');
      expect(el?.textContent).toBe('Off');
      return el!;
    });
    // The network's estimate (12,345 H/s in the fake node) is a line of its own.
    expect(rate.closest('section, article, div')!.textContent).toMatch(/Network/);
    view.unmount();
    generating = { generate: true, threads: 2, hashespersec: 1500 };
    go('home');
    render(Shell);
    await waitFor(() => expect(document.querySelector('[data-testid="mining-rate"]')?.textContent).toMatch(/1\.5\d* kH\/s|1,?500 H\/s/));
    generating = { generate: false, threads: 0, hashespersec: 0 };
  });

  it.skipIf(SIDECHAIN)("the Mining card's Network line opens the network hash estimator of the Mine page", async () => {
    prefs.layout = 'dashboard';
    go('home');
    render(Shell);
    const link = await waitFor(() => {
      const el = document.querySelector<HTMLButtonElement>('[data-testid="mining-network"]');
      expect(el?.textContent).toMatch(/12\.3\d* kH\/s/);
      return el!;
    });
    link.click();
    expect(nav.page).toBe('mine');
    expect(nav.tab).toBe('estimator');
  });

  it("the dashboard's sidebar has no Menus button (user decision); the workspace keeps one", () => {
    prefs.layout = 'dashboard';
    go('home');
    const view = render(Shell);
    const side = screen.getByRole('navigation', { name: 'Pages' });
    expect(within(side).queryByRole('button', { name: /^Menus of chains-qt/ })).toBeNull();
    view.unmount();
    prefs.layout = 'workspace';
    render(Shell);
    expect(screen.getByRole('button', { name: /^Menus of chains-qt/ })).toBeTruthy();
    prefs.layout = 'dashboard';
  });
});
