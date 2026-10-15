// The translator against text from outside the app (security audit 2026-10-14, item 3): a label that
// reads like a message of the app is shown as it is, in French too, and long text from the chain or a
// peer cannot freeze the page (no backtracking matcher, nothing that looks at the page).
import { flushSync } from 'svelte';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { render, waitFor } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import Transactions from '../pages/Transactions.svelte';
import { setTransport } from '../lib/bridge';
import { dialogs } from '../lib/dialogs.svelte';
import { setCatalog, t, tn, type Locale } from '../lib/i18n.svelte';
import Toasts from '../components/Toasts.svelte';
import DialogHost from '../components/DialogHost.svelte';
import { errorText, inform } from '../lib/dialogs.svelte';
import { niceTimeOffset } from '../lib/format';
import { syncStatus } from '../lib/sync';
import { toast, toasts } from '../lib/state.svelte';
import { statusText } from '../lib/wallet';
import { app, prefs, wallets } from '../lib/state.svelte';
import { resetWalletData } from '../lib/walletdata.svelte';
import { regtestP2tr, regtestP2wpkh } from './bech32';

const FR_FILE = JSON.parse(readFileSync(join(__dirname, '../../public/locales/fr.json'), 'utf8')) as { code: string; name: string; rtl: boolean; gui: Record<string, string | string[]> };
/** French as the host gives it (app.locale: the messages of the interface). */
const FR: Locale = { code: FR_FILE.code, name: FR_FILE.name, rtl: FR_FILE.rtl, messages: FR_FILE.gui };

beforeAll(() => {
  const proto = HTMLDialogElement.prototype as unknown as Record<string, unknown>;
  proto.showModal ??= function (this: HTMLDialogElement) {
    this.setAttribute('open', '');
  };
  proto.close ??= function (this: HTMLDialogElement) {
    this.removeAttribute('open');
  };
});

beforeEach(() => {
  app.info = { name: 'Chains', version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 600 };
  prefs.unit = 'CHN';
  prefs.mask = false;
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
  wallets.current = 'w';
  wallets.balances.w = { trusted: 5_000_000_000n, untrustedPending: 0n, immature: 0n };
});

afterEach(() => {
  setTransport(null);
  setCatalog(null);
  dialogs.queue.splice(0);
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
});

/** The transactions of the wallet, with these labels. */
function history(labels: string[]) {
  setTransport(async (r) => {
    if (r.method === 'wallet.transactions') {
      return {
        height: 10,
        hash: 'h',
        records: labels.map((label, i) => [String(i).padStart(2, '0').repeat(32), 0, 'recv', i % 2 ? regtestP2tr : regtestP2wpkh, label, 1_700_000_000 + i, 250_000_000, 0, 'confirmed', 7, 0, true, `k${i}`]),
      };
    }
    if (r.method === 'wallet.addresses') return [];
    return null;
  });
}

/** The labels shown in the list, as the page shows them. */
const shownLabels = () => [...document.querySelectorAll('.cellwrap [translate="no"]:not(.amt)')].map((e) => e.textContent ?? '');

describe('text from outside the app', () => {
  it('is shown verbatim in French even when it reads like a message of the app', async () => {
    setCatalog(FR);
    // Messages of the catalog, one with a placeholder ("%1 behind"), and the French of one.
    const labels = ['Send', 'Confirmed', 'Label', '3 hours behind', 'Envoyer'];
    expect(t('Send')).toBe('Envoyer');
    history(labels);
    render(Transactions);
    await waitFor(() => expect(shownLabels().length).toBe(labels.length));
    expect(shownLabels().sort()).toEqual([...labels].sort());
    // The words of the app around them are in French.
    expect(document.querySelector('[role="columnheader"]:nth-child(4)')?.textContent?.trim()).toBe('Étiquette');
  });

  it('of 10 KB renders in under 50 ms, in French', async () => {
    setCatalog(FR);
    // A page shown once, so that what is measured is the label.
    history(['warm up']);
    const first = render(Transactions);
    await waitFor(() => expect(shownLabels()).toEqual(['warm up']));
    first.unmount();
    resetWalletData();

    // What made the regular expressions of the old translator backtrack: the words of the messages,
    // over and over, with no end that fits.
    const unit = ' behind of to at %1 and Sent to Received with ';
    const big = unit.repeat(Math.ceil(10_240 / unit.length)).slice(0, 10_240);
    let delivered = 0;
    setTransport(async (r) => {
      if (r.method === 'wallet.transactions') {
        delivered = performance.now();
        return { height: 10, hash: 'h', records: [['cc'.repeat(32), 0, 'recv', regtestP2wpkh, big, 1_700_000_000, 250_000_000, 0, 'confirmed', 7, 0, true, 'k']] };
      }
      if (r.method === 'wallet.addresses') return [];
      return null;
    });
    render(Transactions);
    let shown = 0;
    await waitFor(
      () => {
        flushSync();
        expect(shownLabels()).toEqual([big]);
        shown = performance.now();
      },
      { interval: 1 },
    );
    expect(delivered).toBeGreaterThan(0);
    expect(shown - delivered).toBeLessThan(50);
  });

  it('is never matched against the catalog, whatever its length', () => {
    setCatalog({ code: 'fr', name: 'Français', rtl: false, messages: { '%1 a %2 a %3 a %4 b': '%1 x %2 x %3 x %4 y', '%1 behind': '%1 de retard' } });
    const evil = 'a '.repeat(5_000);
    const start = performance.now();
    // Only a message of the app is looked up, as a whole: nothing is matched in what it is not.
    expect(t(evil)).toBe(evil);
    expect(t(evil + 'behind')).toBe(evil + 'behind');
    expect(t('5 hours behind')).toBe('5 hours behind');
    expect(performance.now() - start).toBeLessThan(50);
  });

  it('put in a message of the app is spliced in as it is, not translated', () => {
    setCatalog({ code: 'fr', name: 'Français', rtl: false, messages: { 'Sent to %1': 'Envoyé à %1', Send: 'Envoyer', '%1 behind': '%1 de retard' } });
    expect(t('Sent to %1', 'Send')).toBe('Envoyé à Send');
    // A value holding a placeholder is not filled again.
    expect(t('Sent to %1', '%1 behind')).toBe('Envoyé à %1 behind');
    // A template of the app is a message of its own: "%1 hours behind" is not "%1 behind" with "hours" left in English.
    expect(t('%1 hours behind', 3)).toBe('3 hours behind');
    expect(t('%1 behind', '3 hours')).toBe('3 hours de retard');
  });
});

describe('messages the app puts together, in French', () => {
  afterEach(() => {
    toasts.list.splice(0);
  });

  it('show an error of the host as it is, never partly translated', async () => {
    setCatalog(FR);
    // Words of the catalog are in it ("%1 and %2", "Error", "Send"): the old translator made "… sidechain et of its programs …".
    const hostError = 'The name of the sidechain and of its programs are needed.';
    expect(t('%1 and %2', 'a', 'b')).toBe('a et b');
    const { container } = render(Toasts);
    toast('error', t('Settings'), errorText(new Error(hostError)));
    flushSync();
    expect(container.querySelector('.toast strong')?.textContent).toBe('Paramètres');
    expect(container.querySelector('.toast p')?.textContent).toBe(hostError);
    // In a dialog, the same.
    const host = render(DialogHost);
    void inform(t('Error'), errorText(new Error(`Send failed: ${hostError} 3 hours behind`)), 'error');
    await waitFor(() => expect(host.container.ownerDocument.querySelector('.question .message')).toBeTruthy());
    const dialog = host.container.ownerDocument.querySelector('dialog[open]') as HTMLElement;
    expect(dialog.querySelector('h2')?.textContent).toBe('Erreur');
    expect(dialog.querySelector('.message')?.textContent).toBe(`Send failed: ${hostError} 3 hours behind`);
    expect(dialog.querySelector('.message [translate="no"]')).toBeTruthy();
  });

  it('are wholly in French: the sync status, durations, confirmations', () => {
    setCatalog(FR);
    const now = 1_800_000_000;
    const behind = syncStatus({ tipHeight: 100, tipTime: now - 3 * 3600, progress: 0.5, loadingBlocks: false, headerHeight: 100, headerTime: now, connections: 3, networkActive: true, spacing: 600, now });
    expect(behind.label).toBe('Synchronisation avec le réseau…');
    expect(behind.behind).toBe('en retard de 3\u00a0heures');
    expect(behind.detail).toContain('Le dernier bloc reçu avait été généré il y a 3\u00a0heures.');
    expect(behind.detail).not.toMatch(/\b(hours|behind|Processed)\b/);
    expect(niceTimeOffset(400 * 86400)).toBe('1\u00a0an et 4\u00a0semaines');
    expect(statusText({ status: 'confirmed', depth: 7, maturesIn: 0 })).toBe('Confirmée (7 confirmations)');
    expect(tn('Processed %n block(s) of transaction history.', 100)).toBe('100 blocs d’historique transactionnel ont été traités.');
    // And English as it was written.
    setCatalog(null);
    expect(syncStatus({ tipHeight: 1, tipTime: now - 3 * 3600, progress: 0.5, loadingBlocks: false, headerHeight: 1, headerTime: now, connections: 3, networkActive: true, spacing: 600, now }).behind).toBe('3 hours behind');
    expect(niceTimeOffset(400 * 86400)).toBe('1 year and 4 weeks');
  });
});

describe('direction: text from outside cannot turn the words of the app around (audit port-1 #2)', () => {
  afterEach(() => setCatalog(null));

  it('isolates a value that can turn the text, in t() and tn(), in English and in French', async () => {
    const { isolate } = await import('../lib/i18n.svelte');
    const title = '‮txet emos';
    const said = t('Buy shares of %1 for at most %2', title, '0.01010002');
    // The title is between FSI and PDI, and the amount after it, untouched.
    expect(said).toBe('Buy shares of ⁨‮txet emos⁩ for at most 0.01010002');
    expect(tn('%n share(s) of %1', 2, title)).toBe('2 shares of ⁨‮txet emos⁩');
    setCatalog(FR);
    expect(t('%1 behind', '‮3 h')).toBe(t('%1 behind', 'X').replace('X', '⁨‮3 h⁩'));
    setCatalog(null);
    // Plain ASCII is left as it is (it cannot turn the text).
    expect(t('Sent to %1', 'alice')).toBe('Sent to alice');
    // A PDI of its own cannot close the isolate early; isolates it opens are closed before it ends.
    expect(isolate('⁩‮abc')).toBe('⁨‮abc⁩');
    expect(isolate('⁧abc')).toBe('⁨⁧abc⁩⁩');
    expect(isolate('⁦a⁩b⁩')).toBe('⁨⁦a⁩b⁩');
    // Hebrew and Arabic text are isolated too.
    expect(isolate('שלום')).toBe('⁨שלום⁩');
  });

  it('shows text from outside isolated (dir="auto"), and the stylesheet isolates every translate="no"', () => {
    const view = render(Toasts);
    toast('error', 'Error', '‮abc 0.01');
    flushSync();
    const span = [...view.container.querySelectorAll('span[translate="no"]')].find((s) => s.textContent === '‮abc 0.01');
    expect(span?.getAttribute('dir')).toBe('auto');
    const css = readFileSync(join(__dirname, '../styles/base.css'), 'utf8');
    expect(css).toMatch(/\[translate='no'\]\s*\{\s*unicode-bidi:\s*isolate;/);
    toasts.list.length = 0;
  });
});
