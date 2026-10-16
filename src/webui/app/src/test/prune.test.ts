// The least prune size is the chain's (MinPruneTarget: 10000 MiB on a sidechain, so 11 GB): the intro and
// Settings offer no less, and a smaller number typed becomes it, rather than a node that refuses to start.
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import Intro from '../views/Intro.svelte';
import Settings from '../pages/Settings.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { nav } from '../lib/nav.svelte';
import { app } from '../lib/state.svelte';

let calls: BridgeRequest[] = [];
function host(handlers: Record<string, (r: BridgeRequest) => unknown>) {
  calls = [];
  setTransport(async (r) => {
    calls.push(r);
    const h = handlers[r.method ?? ''];
    return h ? h(r) : null;
  });
}

beforeEach(() => {
  app.info = { name: 'Chains', version: 'test', bugreport: '', network: 'main', networkLabel: '', datadir: '/d', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 60 };
});
afterEach(() => {
  setTransport(null);
  app.phaseData = {};
});

describe('the least prune size of the chain', () => {
  it('is what the intro offers and the least it takes', async () => {
    host({ 'intro.space': () => ({ exists: false, free: 2e12 }), 'intro.choose': () => true });
    app.phaseData = { defaultDir: '/d', dir: '/d', blockchainGB: 30, chainstateGB: 2, pruneGB: 11, minPruneGB: 11 };
    render(Intro);
    const size = screen.getByLabelText('Storage limit in GB') as HTMLInputElement;
    await waitFor(() => expect(size.value).toBe('11'));
    expect(size.min).toBe('11');
    await fireEvent.click(screen.getByRole('checkbox'));
    await fireEvent.input(size, { target: { value: '2' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Start' }));
    await waitFor(() => expect(calls.find((c) => c.method === 'intro.choose')?.params).toEqual({ dir: '/d', pruneGB: 11 }));
  });

  it('is the least Settings takes', async () => {
    const options = {
      dbcache: 450, par: 0, prune: true, pruneSize: 11, spendZeroConf: true, signer: '', natpmp: true, listen: true, server: false,
      proxyUse: false, proxyIp: '127.0.0.1', proxyPort: '9050', torUse: false, torIp: '127.0.0.1', torPort: '9050', lang: '', startAtLogin: false,
      ignored: {}, overridden: '', limits: { dbcacheMin: 4, parMin: -8, parMax: 15, pruneMinGB: 11 }, proxyReach: { ipv4: false, ipv6: false, tor: false },
      externalSigner: false, restartRequired: false, pending: 0,
    };
    host({ 'options.get': () => options, 'options.set': () => options, 'tray.state': () => ({ available: true, visible: true, windowVisible: true }) });
    nav.tab = 'main';
    render(Settings);
    const size = (await waitFor(() => screen.getByLabelText('Prune size'))) as HTMLInputElement;
    expect(size.min).toBe('11');
    await fireEvent.change(size, { target: { value: '3' } });
    await waitFor(() => expect(calls.find((c) => c.method === 'options.set')?.params).toEqual({ name: 'pruneSize', value: 11 }));
  });
});
