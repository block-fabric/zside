// The start of the page: whatever the order in which the answer of app.hello and the phase events come,
// the page ends in the phase the host is in. (A node with no wallet to load is ready before the page
// has loaded its language: the "ready" event came while the page waited, the older answer of app.hello
// ("init") was put over it, and the page stayed on the splash for ever.)
import { render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeAll, describe, expect, it } from 'vitest';
import App from '../App.svelte';
import { dispatch, setTransport, type BridgeRequest } from '../lib/bridge';
import { app, wallets } from '../lib/state.svelte';
import { resetWalletData } from '../lib/walletdata.svelte';

const INFO = { name: 'Chains', version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp', walletEnabled: true, devtools: false, test: false, platform: 'linux', blockSpacing: 600 };

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

afterEach(() => {
  setTransport(null);
  app.phase = 'init' as typeof app.phase;
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
});

function node(r: BridgeRequest, phase: string, onLocale?: () => void): unknown {
  switch (r.method) {
    case 'app.hello':
      return { phase, phaseData: {}, info: INFO, dialogs: [] };
    case 'app.locale':
      onLocale?.();
      return { code: '' };
    case 'node.state':
      return { connections: 0, networkActive: true, warnings: [], tip: { height: 10, time: 1_700_000_000, hash: 'ab'.repeat(32), progress: 1, ibd: false }, headerTip: { height: 10, time: 1_700_000_000 }, wallets: [] };
    case 'window.state':
      return { titleBar: 'integrated', supported: false, frameless: false, restartNeeded: false, maximized: false };
    case 'wallets.list':
    case 'listbanned':
    case 'getpeerinfo':
      return [];
    default:
      return null;
  }
}

describe('the start of the page', () => {
  it('takes the ready event that comes while it loads its language, over the older answer of app.hello', async () => {
    // The host answers "init", then is ready before the page has its language.
    setTransport(async (r) => node(r, 'init', () => dispatch([{ type: 'phase', data: { phase: 'ready', data: {} } }])));
    render(App);
    await waitFor(() => expect(app.phase).toBe('ready'));
    // Not the splash: the frame of the app.
    await waitFor(() => expect(screen.queryByText(/Press/)).toBeNull());
    await new Promise((r) => setTimeout(r, 50));
    expect(app.phase).toBe('ready');
  });

  it('takes the phase of app.hello when no event came', async () => {
    setTransport(async (r) => node(r, 'ready'));
    render(App);
    await waitFor(() => expect(app.phase).toBe('ready'));
  });
});

describe('every layout at the start', () => {
  for (const layout of ['dashboard', 'workspace'] as const) {
    it(`${layout}: opens Home, also when the preferences come after the frame is drawn`, async () => {
      const { prefs } = await import('../lib/state.svelte');
      const { go, nav } = await import('../lib/nav.svelte');
      const Shell = (await import('../views/Shell.svelte')).default;
      app.info = INFO;
      prefs.layout = layout;
      prefs.loaded = false;
      go('overview');
      setTransport(async (r) => node(r, 'ready'));
      const view = render(Shell);
      await new Promise((r) => setTimeout(r, 20));
      expect(nav.page).toBe('overview');
      prefs.loaded = true;
      await waitFor(() => expect(nav.page).toBe('home'));
      view.unmount();
    });
  }
});
