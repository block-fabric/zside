// The navigation of Simple mode, and the folding sidebar (user feedback 2026-10-19).
//
// The rule (doc/webui.md, "Simple and Advanced modes"), the same in every app made from the kit, so that
// this test, merged into each, holds each to it with its own pages (chain/local.ts):
//   Home · Send · Receive · Activity · Move coins · at most three main features of the chain · Settings
// each in plain words, with a line that says what it is for.
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import Sidebar from '../components/Sidebar.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { buildCommands } from '../lib/commands.svelte';
import { handleShortcut } from '../lib/keyboard';
import { navSectionOf, navSections } from '../lib/layout';
import { nav, PAGES, type PageId } from '../lib/nav.svelte';
import { app, applyPrefs, prefs, wallets } from '../lib/state.svelte';
import { pageTitleSource, SIMPLE_END, SIMPLE_MAX_FEATURES, SIMPLE_MOVE, SIMPLE_START, simpleFeatures, simpleNavPages } from '../lib/uimode.svelte';
import { SIDECHAIN } from '../chain/config';
import { LOCAL_PAGES, type LocalPage } from '../chain/local';

// Words of the trade that a name or a line of Simple mode's navigation must not use (the glossary explains
// them on the pages; the navigation of a beginner says what a page is for).
const JARGON = /\b(bmm|psbt|mempool|utxo|rpc|hash|merkle|bundle|validator|epoch|nullifier|orchard|amm|dns|votecoin|liquidity|sidechain node|console|peer|descriptor|bip\d*)\b/i;

let calls: BridgeRequest[] = [];
beforeEach(() => {
  app.info = { name: 'Chains', version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp/x', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 60 };
  calls = [];
  setTransport(async (r) => {
    calls.push(r);
    return r.method === 'prefs.set' ? {} : null;
  });
  prefs.loaded = true;
  prefs.tipsDone = true;
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false } as (typeof wallets.list)[number]];
  wallets.current = 'w';
});
afterEach(() => {
  setTransport(null);
  prefs.uiMode = 'advanced';
  prefs.sidebarCollapsed = false;
  wallets.list = [];
  wallets.current = null;
  nav.page = 'overview';
  nav.tab = '';
});

const ownSimple = () => (LOCAL_PAGES as readonly LocalPage[]).filter((p) => p.simple);

describe('the navigation of Simple mode (the rule of doc/webui.md)', () => {
  it('is Home, Send, Receive, Activity, Move coins, the main features of the chain, Settings, in that order', () => {
    expect(SIMPLE_START).toEqual(['home', 'send', 'receive', 'transactions']);
    expect(SIMPLE_MOVE).toBe(SIDECHAIN ? 'peg' : 'sidechains');
    expect(SIMPLE_END).toEqual(['settings']);
    const features = ownSimple().map((p) => p.id as PageId);
    expect(simpleFeatures()).toEqual(features);
    expect(simpleNavPages()).toEqual(['home', 'send', 'receive', 'transactions', SIMPLE_MOVE, ...features, 'settings']);
  });

  it('holds at most three main features of the chain, pages of the chain that are not of Advanced only', () => {
    const own = ownSimple();
    expect(own.length, 'more pages marked simple than Simple mode shows').toBeLessThanOrEqual(SIMPLE_MAX_FEATURES);
    for (const p of own) {
      expect(p.advanced, `${p.id} is marked simple and advanced`).toBeFalsy();
      expect(p.section, `${p.id}: a main feature is a page of the chain`).not.toBe('tools');
      expect(p.tabOf, `${p.id}: a main feature is a page of its own, not a tab of another`).toBeUndefined();
    }
  });

  it('names every page in plain words, with a line that says what it is for', () => {
    prefs.uiMode = 'simple';
    for (const id of simpleNavPages()) {
      const title = pageTitleSource(id);
      const summary = PAGES[id].simpleSummary ?? '';
      expect(summary, `${id} has no simpleSummary`).not.toBe('');
      expect(title.length, `${id}: "${title}" is long for the sidebar`).toBeLessThanOrEqual(22);
      expect(summary.length, `${id}: "${summary}" is long for a line`).toBeLessThanOrEqual(60);
      expect(title, `${id}: a word of the trade in "${title}"`).not.toMatch(JARGON);
      expect(summary, `${id}: a word of the trade in "${summary}"`).not.toMatch(JARGON);
    }
    // Activity is the history of the wallet; the coins move between the chains as "Move coins".
    expect(pageTitleSource('transactions')).toBe('Activity');
    expect(pageTitleSource(SIMPLE_MOVE)).toBe('Move coins');
  });

  it('is what the sidebar shows in Simple mode, in its order, and every page in Advanced', async () => {
    prefs.uiMode = 'simple';
    render(Sidebar);
    const entries = () => [...document.querySelectorAll<HTMLElement>('nav.sidebar .scroll .entry')];
    const shown = () => entries().map((e) => e.querySelector('.grow')?.textContent?.trim());
    expect(shown()).toEqual(simpleNavPages().map((id) => pageTitleSource(id)));
    // The line that says what each is for.
    expect(entries().every((e) => !!e.querySelector('.what')?.textContent?.trim())).toBe(true);
    expect(navSections().map((s) => s.title)).toEqual(['', 'Your money', 'Features', '']);
    prefs.uiMode = 'advanced';
    await waitFor(() => expect(shown()).toContain('Overview'));
    expect(shown()).toContain('Manage wallets');
    expect(document.querySelector('nav.sidebar .what')).toBeNull();
  });

  it('says the section of the page in the breadcrumbs, as the sidebar of the mode shown', () => {
    prefs.uiMode = 'simple';
    expect(navSectionOf('send')).toBe('Your money');
    expect(navSectionOf(SIMPLE_MOVE)).toBe('Features');
    // A page reached by the palette in Simple mode keeps the section of Advanced.
    expect(navSectionOf('explorer')).toBe('Chain');
    prefs.uiMode = 'advanced';
    expect(navSectionOf('send')).toBe('Wallet');
  });

  it('reaches the wallets from the card of the wallet, the page named Manage wallets (its old id kept)', async () => {
    prefs.uiMode = 'simple';
    render(Sidebar);
    await fireEvent.click(screen.getByRole('button', { name: 'Manage wallets' }));
    expect(nav.page).toBe('wallets');
    expect(PAGES.wallets.title).toBe('Manage wallets');
    expect(buildCommands().find((c) => c.id === 'page.wallets')?.label).toBe('Manage wallets');
  });
});

describe('the folding sidebar', () => {
  it('folds into a rail of icons with the button of its foot, each item named in its tooltip, and remembers it', async () => {
    render(Sidebar);
    const nav_ = screen.getByRole('navigation', { name: 'Pages' });
    expect(nav_.classList.contains('collapsed')).toBe(false);
    await fireEvent.click(screen.getByRole('button', { name: 'Collapse' }));
    expect(prefs.sidebarCollapsed).toBe(true);
    expect(calls.some((c) => c.method === 'prefs.set' && (c.params as Record<string, unknown>)?.sidebarCollapsed === true)).toBe(true);
    await waitFor(() => expect(nav_.classList.contains('collapsed')).toBe(true));
    const send = [...nav_.querySelectorAll<HTMLElement>('.scroll .entry')].find((e) => e.getAttribute('aria-label') === 'Send')!;
    expect(send).toBeTruthy();
    expect(send.title).toMatch(/^Send: /);
    const fold = screen.getByRole('button', { name: 'Expand the sidebar' });
    expect(fold.getAttribute('aria-expanded')).toBe('false');
    await fireEvent.click(fold);
    expect(prefs.sidebarCollapsed).toBe(false);
  });

  it('folds with Ctrl+B and the palette, and is read back from the preferences', () => {
    const key = new KeyboardEvent('keydown', { key: 'b', ctrlKey: true });
    expect(handleShortcut(key)).toBe(true);
    expect(prefs.sidebarCollapsed).toBe(true);
    const cmd = buildCommands().find((c) => c.id === 'display.sidebar')!;
    expect(cmd.label).toBe('Expand the sidebar');
    expect(cmd.shortcut).toBe('Ctrl+B');
    cmd.run();
    expect(prefs.sidebarCollapsed).toBe(false);
    applyPrefs({ sidebarCollapsed: true });
    expect(prefs.sidebarCollapsed).toBe(true);
    applyPrefs({ sidebarCollapsed: 'yes' });
    expect(prefs.sidebarCollapsed).toBe(true);
  });

  it('stays whole as the drawer of a narrow window', async () => {
    prefs.sidebarCollapsed = true;
    render(Sidebar, { props: { open: true } });
    expect(screen.getByRole('navigation', { name: 'Pages' }).classList.contains('collapsed')).toBe(false);
  });
});
