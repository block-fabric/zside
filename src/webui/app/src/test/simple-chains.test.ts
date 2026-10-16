// Simple mode on Chains' own pages: the Sidechains page as "Move coins" (a wizard that deposits with the
// same call as Advanced's form) and Home with merged mining in a line. Advanced keeps the full pages.
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import DialogHost from '../components/DialogHost.svelte';
import Sidechains from '../pages/Sidechains.svelte';
import Home from '../views/home/Home.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { dialogs } from '../lib/dialogs.svelte';
import { nav } from '../lib/nav.svelte';
import { app, node, prefs, wallets } from '../lib/state.svelte';
import { SIDECHAIN } from '../chain/config';

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
  app.info = { name: 'Chains', version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp/x', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 60 };
  prefs.unit = 'CHN';
  prefs.mask = false;
  prefs.uiMode = 'simple';
  prefs.loaded = true;
  prefs.tipsDone = true;
  nav.page = 'sidechains';
  nav.tab = '';
  node.tip = { height: 130, time: 1_700_000_000, hash: '00'.repeat(32), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
});

afterEach(() => {
  setTransport(null);
  dialogs.queue.splice(0);
  wallets.list = [];
  wallets.current = null;
  wallets.balances = {};
});

type Handler = (r: BridgeRequest) => unknown;
function host(handlers: Record<string, Handler>): BridgeRequest[] {
  const calls: BridgeRequest[] = [];
  setTransport(async (r) => {
    calls.push(r);
    const h = handlers[r.method ?? ''];
    if (!h) throw { code: -32601, message: `No ${r.method}` };
    return h(r);
  });
  return calls;
}

const INFO = { maxsidechains: 256, activationperiod: 20, activationmaxfailures: 9, replacementperiod: 40, withdrawalperiod: 60, withdrawalminscore: 30, maxpendingbundles: 64, upvoteexpiryblocks: 20, height: 130, activesidechains: 2, proposals: 0, pendingbundles: 0, escrowtotal: 3, defaultwithdrawalvote: 'upvote' };
const SC = (slot: number, title: string) => ({ slot, title, description: `${title}, a sidechain`, version: 0, hashid1: '0'.repeat(64), hashid2: '0'.repeat(40), proposalhash: '71'.repeat(32), activationheight: 121, escrowscript: 'b4010151', pendingbundles: 0 });

function drivechain(extra: Record<string, Handler> = {}) {
  return host({
    getdrivechaininfo: () => INFO,
    listactivesidechains: () => [SC(1, 'Testchain'), SC(4, 'Assets')],
    listsidechainproposals: () => ({ pending: [], queued: [] }),
    listwithdrawalbundles: () => [],
    listsidechaindeposits: () => [],
    'clipboard.read': () => 's1_erin_b91ea9',
    'drivechain.depositAddress': (r) => {
      const text = (r.params as { text: string }).text;
      if (text === 's1_erin_b91ea9') return { kind: 'valid', slot: 1, address: 'erin' };
      if (text === 's4_fred_000000') return { kind: 'valid', slot: 4, address: 'fred' };
      return /^s\d+_/.test(text) ? { kind: 'invalid' } : { kind: 'plain' };
    },
    ...extra,
  });
}

function withWallet() {
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
  wallets.current = 'w';
  wallets.balances = { w: { trusted: 1_000_000_000n, untrustedPending: 0n, immature: 0n } };
}

describe('Move coins (the Sidechains page in Simple mode)', () => {
  it('moves CHN to a sidechain in four steps, with the call of Advanced', async () => {
    withWallet();
    const calls = drivechain({ createsidechaindeposit: () => ({ txid: 'cd'.repeat(32), total: 5 }) });
    render(DialogHost);
    render(Sidechains);
    // The tabs of Simple mode: no proposals nor withdrawals.
    expect(screen.getByRole('tab', { name: 'Move coins' })).toBeTruthy();
    expect(screen.getByRole('tab', { name: 'Sidechain apps' })).toBeTruthy();
    expect(screen.queryByRole('tab', { name: 'Proposals' })).toBeNull();
    expect(document.querySelector('.slots')).toBeNull();
    // 1: the sidechain, by its name.
    await fireEvent.click(await waitFor(() => screen.getByRole('button', { name: /Testchain/ })));
    // 2: the amount, at most what is available.
    await waitFor(() => screen.getByText(/How much CHN should go to/));
    expect(document.body.textContent).toMatch(/You have 10\.00000000\sCHN available/);
    const next = () => screen.getByRole('button', { name: 'Next' });
    expect((next() as HTMLButtonElement).disabled).toBe(true);
    await fireEvent.input(screen.getByLabelText('Amount'), { target: { value: '1.5' } });
    await waitFor(() => expect((next() as HTMLButtonElement).disabled).toBe(false));
    await fireEvent.click(next());
    // 3: where it arrives; a plain address, a typo and another sidechain's address are refused.
    const field = await waitFor(() => screen.getByLabelText('Deposit address'));
    await fireEvent.input(field, { target: { value: 'erin' } });
    await waitFor(() => screen.getByText(/This is not a deposit address/));
    expect((next() as HTMLButtonElement).disabled).toBe(true);
    await fireEvent.input(field, { target: { value: 's1_eric_b91ea9' } });
    await waitFor(() => screen.getByText(/typing error/));
    await fireEvent.input(field, { target: { value: 's4_fred_000000' } });
    await waitFor(() => screen.getByText(/for another sidechain \(slot 4\)/));
    expect((next() as HTMLButtonElement).disabled).toBe(true);
    // Paste: the host's clipboard.
    await fireEvent.click(screen.getByRole('button', { name: /Paste/ }));
    await waitFor(() => screen.getByText('A deposit address of Testchain.'));
    await fireEvent.click(next());
    // 4: confirm.
    await waitFor(() => screen.getByText('Check, then move the coins.'));
    await fireEvent.click(screen.getByRole('button', { name: 'Move 1.50000000 CHN' }));
    await waitFor(() => screen.getByText(/are on their way to/));
    const deposit = calls.find((c) => c.method === 'createsidechaindeposit');
    expect(deposit?.params).toEqual([1, 's1_erin_b91ea9', '1.5']);
    expect(deposit?.wallet).toBe('w');
    // How the coins come back.
    expect(document.body.textContent).toContain('Bringing coins back');
    expect(document.body.textContent).toContain('Drivechains');
  });

  it('shows the full page in Advanced mode, and what is asked for in Simple', async () => {
    drivechain();
    prefs.uiMode = 'advanced';
    render(Sidechains);
    await waitFor(() => expect(document.querySelectorAll('.slot').length).toBe(256));
    expect(screen.getByRole('tab', { name: 'Proposals' })).toBeTruthy();
    expect(screen.queryByText('Move CHN to a sidechain')).toBeNull();
    // Simple mode, opened on the withdrawals (a link, the palette): shown, not hidden.
    prefs.uiMode = 'simple';
    nav.tab = 'withdrawals';
    await waitFor(() => expect(screen.getByRole('tab', { name: 'Withdrawals' }).getAttribute('aria-selected')).toBe('true'));
  });
});

// Chains' own Home (a sidechain has its own, tested in sidechain.test.ts).
describe.skipIf(SIDECHAIN)('Home in Simple mode', () => {
  function chain() {
    return host({
      getblockheader: () => ({ height: 130, time: 1_700_000_000, nTx: 1 }),
      getmininginfo: () => ({ blocks: 130, difficulty: 1, networkhashps: 1, pooledtx: 0 }),
      getgenerate: () => ({ generate: false, threads: 0, hashespersec: 0 }),
      getdrivechaininfo: () => INFO,
      listactivesidechains: () => [SC(1, 'Testchain'), SC(4, 'Assets')],
      listwithdrawalbundles: () => [],
      getsidechainevents: (r) => ((r.params as number[])[0] === 1 ? [{ bmm: 'aa' }, {}] : [{}, {}]),
      getpeerinfo: () => [],
    });
  }

  it('says merged mining in one line, the meters on demand, and no mining card', async () => {
    chain();
    render(Home);
    await waitFor(() => expect(screen.getByTestId('bmm-line').textContent).toMatch(/Sidechains are merge-mined: 1 of 2 active\./));
    expect(document.querySelector('.meters')).toBeNull();
    expect(screen.queryByTestId('mining-rate')).toBeNull();
    expect(screen.getAllByRole('button', { name: /Move coins/ }).length).toBe(2);
    await fireEvent.click(screen.getByRole('button', { name: 'Details' }));
    await waitFor(() => expect(document.querySelector('.meters')).toBeTruthy());
  });

  it('keeps every card in Advanced mode', async () => {
    chain();
    prefs.uiMode = 'advanced';
    render(Home);
    await waitFor(() => expect(document.querySelector('.meters')).toBeTruthy());
    expect(screen.getByTestId('mining-rate')).toBeTruthy();
    expect(screen.queryByTestId('bmm-line')).toBeNull();
  });
});
