// Simple mode (lib/uimode.svelte.ts, user decision 2026-10-16): the preference and its switch in the
// sidebar's foot, the palette's command, the pages it leaves out of the navigation, the glossary and its
// <Term>, the first-run tips of Home, and the status of a transaction in plain words.
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import FirstRunTips from '../components/FirstRunTips.svelte';
import ModeSwitch from '../components/ModeSwitch.svelte';
import Sidebar from '../components/Sidebar.svelte';
import Term from '../components/Term.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { buildCommands } from '../lib/commands.svelte';
import { FIRST_STEPS } from '../lib/firststeps';
import { addTerms, glossaryEntry, glossaryIds } from '../lib/glossary';
import { nav, PAGES, type PageId } from '../lib/nav.svelte';
import { app, applyPrefs, prefs } from '../lib/state.svelte';
import { pageShown, simpleMode } from '../lib/uimode.svelte';
import { plainStatusText, statusText } from '../lib/wallet';
import Transactions from '../pages/Transactions.svelte';
import { wallets } from '../lib/state.svelte';
import { resetWalletData } from '../lib/walletdata.svelte';
import { regtestP2wpkh } from './bech32';
// The source of the state, for its default (the tests themselves start in Advanced, test/setup.ts).
import stateSource from '../lib/state.svelte.ts?raw';

let calls: BridgeRequest[] = [];
beforeEach(() => {
  app.info = { name: 'Chains', version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp/x', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 60 };
  calls = [];
  setTransport(async (r) => {
    calls.push(r);
    return r.method === 'prefs.set' ? {} : null;
  });
  prefs.loaded = true;
  prefs.tipsDone = false;
});
afterEach(() => {
  setTransport(null);
  nav.page = 'overview';
  nav.tab = '';
});

describe('the preference', () => {
  it('is Simple on a first start, and keeps only the two modes', () => {
    // A fresh module would start Simple: the declaration says so (the tests run in Advanced, test/setup.ts).
    expect(/uiMode: 'simple' as/.test(stateSource)).toBe(true);
    applyPrefs({ uiMode: 'advanced' });
    expect(prefs.uiMode).toBe('advanced');
    applyPrefs({ uiMode: 'expert' });
    expect(prefs.uiMode).toBe('advanced');
    applyPrefs({ uiMode: 'simple', tipsDone: true });
    expect(prefs.uiMode).toBe('simple');
    expect(prefs.tipsDone).toBe(true);
    expect(simpleMode()).toBe(true);
  });
});

describe('the switch in the sidebar', () => {
  it('is a radio group of two, saved in the preferences, with the arrow keys', async () => {
    prefs.uiMode = 'simple';
    render(ModeSwitch);
    const group = screen.getByRole('radiogroup', { name: 'Interface mode' });
    const simple = screen.getByRole('radio', { name: 'Simple' });
    const advanced = screen.getByRole('radio', { name: 'Advanced' });
    expect(group).toBeTruthy();
    expect(simple.getAttribute('aria-checked')).toBe('true');
    expect(simple.tabIndex).toBe(0);
    expect(advanced.tabIndex).toBe(-1);
    await fireEvent.click(advanced);
    expect(prefs.uiMode).toBe('advanced');
    await waitFor(() => expect(calls.find((c) => c.method === 'prefs.set')?.params).toEqual({ uiMode: 'advanced' }));
    await fireEvent.keyDown(advanced, { key: 'ArrowLeft' });
    expect(prefs.uiMode).toBe('simple');
    expect(document.activeElement).toBe(simple);
  });

  it('sits in the foot of the sidebar, beside the size of the text', () => {
    render(Sidebar);
    const row = screen.getByRole('radiogroup', { name: 'Interface mode' }).parentElement as HTMLElement;
    expect(row.querySelector('[aria-label="Smaller text"]')).toBeTruthy();
    expect(row.querySelector('[aria-label="Larger text"]')).toBeTruthy();
    expect(row.closest('.foot')).toBeTruthy();
  });

  it('leaves the pages of Advanced out of the sidebar in Simple mode, every page in Advanced', async () => {
    prefs.uiMode = 'simple';
    render(Sidebar);
    const nav = screen.getByRole('navigation', { name: 'Pages' });
    const titles = () => [...nav.querySelectorAll('.scroll .entry .grow')].map((e) => e.textContent?.trim());
    const hidden = (Object.keys(PAGES) as PageId[]).filter((id) => PAGES[id].advanced && !PAGES[id].tabOf).map((id) => PAGES[id].title);
    expect(hidden.length).toBeGreaterThan(0);
    for (const title of hidden) expect(titles()).not.toContain(title);
    expect(titles()).toContain('Send');
    expect(titles()).toContain('Settings');
    prefs.uiMode = 'advanced';
    await waitFor(() => expect(titles()).toContain('Advanced transactions'));
  });

  it('keeps the pages a beginner needs in Simple mode', () => {
    prefs.uiMode = 'simple';
    for (const id of ['home', 'overview', 'send', 'receive', 'transactions', 'wallets', 'settings'] as PageId[]) expect(pageShown(id)).toBe(true);
  });
});

describe('the palette', () => {
  it('switches to the other mode, and brings the first steps back', () => {
    prefs.uiMode = 'simple';
    const mode = buildCommands().find((c) => c.id === 'display.uiMode')!;
    expect(mode.label).toBe('Switch to Advanced mode');
    mode.run();
    expect(prefs.uiMode).toBe('advanced');
    expect(buildCommands().find((c) => c.id === 'display.uiMode')!.label).toBe('Switch to Simple mode');
    prefs.tipsDone = true;
    buildCommands().find((c) => c.id === 'display.tips')!.run();
    expect(prefs.tipsDone).toBe(false);
    expect(prefs.uiMode).toBe('simple');
    expect(nav.page).toBe('home');
  });

  it('still reaches every page in Simple mode', () => {
    prefs.uiMode = 'simple';
    const ids = buildCommands().map((c) => c.id);
    expect(ids).toContain('page.advanced');
    expect(ids).toContain('page.node');
  });
});

describe('the glossary', () => {
  it('explains the words of the trade the pages use', () => {
    for (const id of ['bmm', 'drivechain', 'deposit', 'bundle', 'slot', 'epoch', 'validator', 'bond', 'unbond', 'finality', 'votecoin', 'decision', 'market', 'slippage', 'pool', 'shielded', 'viewingkey', 'sidechain', 'mempool', 'feerate', 'confirmation']) {
      const e = glossaryEntry(id);
      expect(e, id).toBeTruthy();
      expect(e!.text.length).toBeGreaterThan(20);
      expect(e!.text.length).toBeLessThan(260);
    }
    expect(glossaryIds().length).toBeGreaterThan(20);
  });

  it('takes the words of a chain', () => {
    addTerms({ widget: { term: 'widget', text: 'A word only this chain uses, explained by it.' } });
    expect(glossaryEntry('widget')?.term).toBe('widget');
  });

  it('shows the meaning on hover, focus and a tap, and Escape closes it', async () => {
    render(Term, { props: { id: 'mempool' } });
    const word = screen.getByRole('button', { name: 'mempool' });
    const tip = () => document.querySelector('[role="tooltip"]');
    expect(tip()).toBeNull();
    await fireEvent.pointerEnter(word);
    expect(tip()?.textContent).toContain('waiting room');
    expect(word.getAttribute('aria-describedby')).toBe(tip()!.id);
    await fireEvent.pointerLeave(word);
    expect(tip()).toBeNull();
    await fireEvent.focus(word);
    expect(tip()).toBeTruthy();
    await fireEvent.keyDown(word, { key: 'Escape' });
    expect(tip()).toBeNull();
    await fireEvent.click(word);
    expect(tip()).toBeTruthy();
    await fireEvent.pointerLeave(word);
    // A tap pins it until a tap elsewhere.
    expect(tip()).toBeTruthy();
    await fireEvent.pointerDown(document.body);
    expect(tip()).toBeNull();
  });
});

describe('the first-run tips', () => {
  it('show three steps in Simple mode until dismissed', async () => {
    prefs.uiMode = 'simple';
    render(FirstRunTips);
    expect(FIRST_STEPS.length).toBe(3);
    expect(document.querySelectorAll('.tips li').length).toBe(3);
    await fireEvent.click(screen.getByRole('button', { name: 'Got it' }));
    expect(prefs.tipsDone).toBe(true);
    await waitFor(() => expect(document.querySelector('.tips')).toBeNull());
    await waitFor(() => expect(calls.find((c) => c.method === 'prefs.set')?.params).toEqual({ tipsDone: true }));
  });

  it('are not shown in Advanced mode', () => {
    prefs.uiMode = 'advanced';
    render(FirstRunTips);
    expect(document.querySelector('.tips')).toBeNull();
  });

  it('open the page of a step', async () => {
    prefs.uiMode = 'simple';
    render(FirstRunTips);
    const step = FIRST_STEPS.find((s) => s.action && !s.action.panel)!;
    await fireEvent.click(screen.getByRole('button', { name: new RegExp(step.action!.label) }));
    expect(nav.page).toBe(PAGES[step.action!.page].tabOf?.page ?? step.action!.page);
  });
});

describe('the status of a transaction', () => {
  it('is in plain words in Simple mode, with the counts in Advanced', () => {
    const r = (status: string, depth: number, maturesIn = 0) => ({ status, depth, maturesIn }) as Parameters<typeof statusText>[0];
    prefs.uiMode = 'simple';
    expect(statusText(r('unconfirmed', 0))).toBe('Waiting for the next block');
    expect(statusText(r('confirming', 2))).toBe('Confirmed');
    expect(statusText(r('confirmed', 9))).toBe('Final');
    expect(statusText(r('immature', 10, 350))).toBe('Usable in 350 blocks');
    expect(plainStatusText(r('immature', 359, 1))).toBe('Usable in 1 block');
    prefs.uiMode = 'advanced';
    expect(statusText(r('confirmed', 9))).toBe('Confirmed (9 confirmations)');
    expect(statusText(r('unconfirmed', 0))).toBe('Unconfirmed');
  });
});

describe('the history in Simple mode', () => {
  afterEach(() => {
    wallets.list = [];
    wallets.current = null;
    resetWalletData();
  });

  it('says the status of each transaction in words, in a column of its own', async () => {
    wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
    wallets.current = 'w';
    setTransport(async (r) => {
      if (r.method === 'wallet.transactions') return { height: 10, hash: 'h', records: [['aa'.repeat(32), 0, 'recv', regtestP2wpkh, 'Pay', 1_700_000_000, 250_000_000, 0, 'confirmed', 7, 0, true, 'k1']] };
      if (r.method === 'wallet.addresses') return [];
      return null;
    });
    prefs.uiMode = 'simple';
    render(Transactions);
    await waitFor(() => expect(document.querySelector('.status-word')?.textContent).toBe('Final'));
    expect(screen.getByRole('columnheader', { name: /Status/ })).toBeTruthy();
    prefs.uiMode = 'advanced';
    await waitFor(() => expect(document.querySelector('.status-word')).toBeNull());
  });
});
