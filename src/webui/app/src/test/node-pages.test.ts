import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import ConsoleView from '../components/ConsoleView.svelte';
import DialogHost from '../components/DialogHost.svelte';
import PeersView from '../components/PeersView.svelte';
import StatusBar from '../components/StatusBar.svelte';
import Crypto from '../pages/Crypto.svelte';
import Explorer from '../pages/Explorer.svelte';
import Settings from '../pages/Settings.svelte';
import SidechainNodes from '../pages/SidechainNodes.svelte';
import Sidechains from '../pages/Sidechains.svelte';
import Timestamp from '../pages/Timestamp.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { clearConsole, consoleState, history } from '../lib/consolestate.svelte';
import { dialogs } from '../lib/dialogs.svelte';
import { nav } from '../lib/nav.svelte';
import { app, node, prefs, wallets } from '../lib/state.svelte';

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
  app.info = { name: 'Chains', version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp/x', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 600 };
  prefs.unit = 'CHN';
  prefs.mask = false;
  nav.tab = '';
  node.tip = { height: 130, time: 1_700_000_000, hash: '00'.repeat(32), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
});

afterEach(() => {
  setTransport(null);
  dialogs.queue.splice(0);
  wallets.list = [];
  wallets.current = null;
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

const INFO = {
  maxsidechains: 256,
  activationperiod: 20,
  activationmaxfailures: 9,
  replacementperiod: 40,
  withdrawalperiod: 60,
  withdrawalminscore: 30,
  maxpendingbundles: 64,
  upvoteexpiryblocks: 20,
  height: 130,
  activesidechains: 1,
  proposals: 1,
  pendingbundles: 1,
  escrowtotal: 3,
  defaultwithdrawalvote: 'upvote',
};
const SIDECHAIN = { slot: 1, title: 'Testchain', description: 'A sidechain', version: 0, hashid1: '0'.repeat(64), hashid2: '0'.repeat(40), proposalhash: '71'.repeat(32), activationheight: 121, escrowscript: 'b4010151', escrow: { txid: 'ee'.repeat(32), n: 0, amount: 3 }, pendingbundles: 1 };
const PROPOSAL = { slot: 2, title: 'Second', description: '', version: 0, hashid1: '0'.repeat(64), hashid2: '0'.repeat(40), proposalhash: 'a7'.repeat(32), height: 123, age: 2, acks: 5, failures: 1, blocksleft: 18, replacement: false, ack: true };
const BUNDLE = { slot: 1, hash: 'bd'.repeat(32), score: 12, blocksleft: 40, payable: false, known: true, vote: 'upvote', payouts: [{ amount: 1, script: '0014' }], fee: 0.01 };

function drivechain(extra: Record<string, Handler> = {}) {
  return host({
    getdrivechaininfo: () => INFO,
    listactivesidechains: () => [SIDECHAIN],
    listsidechainproposals: () => ({ pending: [PROPOSAL], queued: [] }),
    listwithdrawalbundles: () => [BUNDLE],
    listsidechaindeposits: () => [{ slot: 1, destination: 's1_erin_b91ea9', amount: 2, total: 2, txid: 'd1'.repeat(32), burnindex: 0, txindex: 1, blockhash: 'bb'.repeat(32), confirmations: 3 }],
    'drivechain.depositAddress': (r) => {
      const text = (r.params as { text: string }).text;
      return text === 's1_erin_b91ea9' ? { kind: 'valid', slot: 1, address: 'erin' } : /^s\d+_/.test(text) ? { kind: 'invalid' } : { kind: 'plain' };
    },
    ...extra,
  });
}

describe('the sidechain page', () => {
  it('shows the slots, the sidechains and their deposits, and deposits after the confirmation', async () => {
    wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
    wallets.current = 'w';
    const calls = drivechain({ createsidechaindeposit: () => ({ txid: 'cd'.repeat(32), total: 5 }) });
    render(DialogHost);
    render(Sidechains);
    await waitFor(() => screen.getByText(/1 of 256 sidechain slots in use, holding 3.00000000 CHN in escrow/));
    expect(document.querySelectorAll('.slot').length).toBe(256);
    expect(document.querySelector('.slot.active')?.getAttribute('aria-label')).toBe('Slot 1, Testchain, active');
    // The title of the sidechain is text of the chain (SafeText), in the sentence of the app.
    await waitFor(() => expect(document.querySelector('.target')?.textContent).toBe('Testchain (slot 1)'));
    expect(document.querySelector('.target [translate="no"]')?.textContent).toBe('Testchain');
    await waitFor(() => screen.getByText('s1_erin_b91ea9'));

    await fireEvent.input(screen.getByLabelText('Destination'), { target: { value: 's1_erin_b91ea9' } });
    await waitFor(() => screen.getByText('A deposit address for slot 1.'));
    await fireEvent.input(screen.getByLabelText('Amount'), { target: { value: '1.5' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Deposit' }));
    await waitFor(() => screen.getByText('Confirm sidechain deposit'));
    expect(document.body.textContent).toContain('Deposit 1.50000000 CHN to the sidechain "Testchain" (slot 1)');
    await fireEvent.click(within(document.querySelector('dialog[open]') as HTMLElement).getByRole('button', { name: 'Deposit' }));
    await waitFor(() => screen.getByText(`Deposit sent in transaction ${'cd'.repeat(32)}.`));
    const deposit = calls.find((c) => c.method === 'createsidechaindeposit');
    expect(deposit?.params).toEqual([1, 's1_erin_b91ea9', '1.5']);
    expect(deposit?.wallet).toBe('w');
  });

  it('refuses a deposit address with a typing error', async () => {
    wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
    wallets.current = 'w';
    const calls = drivechain();
    render(DialogHost);
    render(Sidechains);
    // The title of the sidechain is text of the chain (SafeText), in the sentence of the app.
    await waitFor(() => expect(document.querySelector('.target')?.textContent).toBe('Testchain (slot 1)'));
    expect(document.querySelector('.target [translate="no"]')?.textContent).toBe('Testchain');
    await fireEvent.input(screen.getByLabelText('Destination'), { target: { value: 's1_eric_b91ea9' } });
    await waitFor(() => screen.getByText('This deposit address has a typing error in it.'));
    await fireEvent.input(screen.getByLabelText('Amount'), { target: { value: '1' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Deposit' }));
    await waitFor(() => screen.getByText('This deposit address has a typing error in it. Copy it again from the wallet of the sidechain.'));
    expect(calls.some((c) => c.method === 'createsidechaindeposit')).toBe(false);
  });

  it('acks proposals, proposes, and votes on bundles', async () => {
    const calls = drivechain({ acksidechain: () => true, createsidechainproposal: () => ({}), setwithdrawalvote: () => true, setdefaultwithdrawalvote: () => true });
    nav.tab = 'proposals';
    render(DialogHost);
    render(Sidechains);
    await waitFor(() => screen.getByText('Second'));
    const meter = screen.getByRole('progressbar', { name: 'Acks of Second' });
    expect(meter.getAttribute('aria-valuetext')).toBe('5 / 20');
    await fireEvent.click(screen.getByText('Second'));
    await fireEvent.click(screen.getByRole('button', { name: 'Stop acking' }));
    await waitFor(() => expect(calls.find((c) => c.method === 'acksidechain')?.params).toEqual(['a7'.repeat(32), false]));

    await fireEvent.input(screen.getByLabelText('Slot'), { target: { value: '7' } });
    await fireEvent.input(screen.getByLabelText('Title'), { target: { value: 'Seventh' } });
    await fireEvent.input(screen.getByLabelText('Build commit hash'), { target: { value: 'xyz' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Propose' }));
    await waitFor(() => screen.getByText('The build commit hash is 40 hexadecimal characters.'));
    await fireEvent.input(screen.getByLabelText('Build commit hash'), { target: { value: '' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Propose' }));
    await waitFor(() => expect(calls.find((c) => c.method === 'createsidechainproposal')?.params).toEqual([7, 'Seventh', '', '0'.repeat(64), '0'.repeat(40)]));

    nav.tab = 'withdrawals';
    await waitFor(() => screen.getByRole('progressbar', { name: `Score of bundle ${'bd'.repeat(4)}…${'bd'.repeat(4)}` }));
    await fireEvent.click(screen.getByText('Testchain', { selector: '.muted' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Upvote' }));
    await waitFor(() => expect(calls.find((c) => c.method === 'setwithdrawalvote')?.params).toEqual([1, 'upvote', 'bd'.repeat(32)]));
    await fireEvent.change(screen.getByLabelText('Default withdrawal vote'), { target: { value: 'abstain' } });
    await waitFor(() => expect(calls.find((c) => c.method === 'setdefaultwithdrawalvote')?.params).toEqual(['abstain']));
  });
});

describe('the node window', () => {
  it('runs console commands on a wallet, keeps the filtered history, and shows errors', async () => {
    wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
    wallets.current = 'w';
    clearConsole();
    const calls = host({
      help: () => 'getblockcount\nwalletpassphrase "passphrase" timeout',
      'console.check': (r) => {
        const line = (r.params as { line: string }).line;
        return line.startsWith('walletpassphrase') ? { ok: true, filtered: 'walletpassphrase(…)' } : { ok: true, filtered: line };
      },
      'console.run': (r) => {
        const line = (r.params as { line: string }).line;
        return line === 'getblockcount' ? { ok: true, result: '130' } : { ok: false, error: 'Error: the wallet is not encrypted (code -15)' };
      },
    });
    render(ConsoleView);
    await waitFor(() => screen.getByText(/Welcome to the Chains RPC console/));
    const input = screen.getByLabelText('Console command') as HTMLInputElement;
    await fireEvent.input(input, { target: { value: 'getblockcount' } });
    await fireEvent.keyDown(input, { key: 'Enter' });
    await waitFor(() => screen.getByText('130'));
    expect(screen.getByText('Executing command using "w" wallet')).toBeTruthy();
    expect(calls.find((c) => c.method === 'console.run')?.params).toEqual({ line: 'getblockcount', wallet: 'w' });
    await fireEvent.input(input, { target: { value: 'walletpassphrase secret 60' } });
    await fireEvent.keyDown(input, { key: 'Enter' });
    await waitFor(() => screen.getByText('Error: the wallet is not encrypted (code -15)'));
    // The passphrase never goes into the history.
    expect(history.lines).toEqual(['getblockcount', 'walletpassphrase(…)']);
    expect(document.body.textContent).not.toContain('secret');
    await fireEvent.keyDown(input, { key: 'ArrowUp' });
    expect(input.value).toBe('walletpassphrase(…)');
    expect(consoleState.executing).toBe(false);
  });

  it('shows the peers, their details, and bans one', async () => {
    const calls = host({
      getpeerinfo: () => [
        { id: 4, addr: '127.0.0.1:29700', network: 'not_publicly_routable', services: '', servicesnames: ['NETWORK'], relaytxes: true, lastsend: 0, lastrecv: 0, last_transaction: 0, last_block: 0, bytessent: 100, bytesrecv: 200, conntime: Math.floor(Date.now() / 1000) - 60, timeoffset: 0, pingtime: 0.002, version: 70016, subver: '/Chains:32.0.0/', inbound: false, bip152_hb_to: false, bip152_hb_from: false, synced_headers: 130, synced_blocks: 130, connection_type: 'manual' },
      ],
      listbanned: () => [{ address: '10.0.0.1/32', ban_created: 1, banned_until: 2_000_000_000, ban_duration: 1, time_remaining: 1 }],
      'peers.ban': () => true,
      'bans.unban': () => true,
    });
    render(PeersView);
    await waitFor(() => screen.getByText('127.0.0.1:29700'));
    expect(screen.getByText('/Chains:32.0.0/')).toBeTruthy();
    await fireEvent.click(screen.getByText('127.0.0.1:29700'));
    await waitFor(() => screen.getByText('(peer: 4)'));
    expect(screen.getByText('Outbound Manual')).toBeTruthy();
    await fireEvent.contextMenu(screen.getAllByText('127.0.0.1:29700')[0], { clientX: 10, clientY: 10 });
    await fireEvent.click(screen.getByRole('menuitem', { name: 'Ban for 1 week' }));
    await waitFor(() => expect(calls.find((c) => c.method === 'peers.ban')?.params).toEqual({ id: 4, seconds: 604800 }));
    await waitFor(() => screen.getByText('10.0.0.1/32'));
    await fireEvent.click(screen.getByText('10.0.0.1/32'));
    await fireEvent.click(screen.getByRole('button', { name: 'Unban' }));
    await waitFor(() => expect(calls.find((c) => c.method === 'bans.unban')?.params).toEqual({ address: '10.0.0.1/32' }));
  });
});

describe('settings', () => {
  it('changes options of the node and says when a restart is needed', async () => {
    const options = {
      dbcache: 450,
      par: 0,
      prune: false,
      pruneSize: 2,
      spendZeroConf: true,
      signer: '',
      natpmp: true,
      listen: true,
      server: false,
      proxyUse: false,
      proxyIp: '127.0.0.1',
      proxyPort: '9050',
      torUse: false,
      torIp: '127.0.0.1',
      torPort: '9050',
      lang: '',
      startAtLogin: false,
      ignored: { listen: true },
      overridden: '-listen=0 ',
      limits: { dbcacheMin: 4, parMin: -8, parMax: 15, pruneMinGB: 1 },
      proxyReach: { ipv4: false, ipv6: false, tor: false },
      externalSigner: true,
      restartRequired: false,
    };
    const calls = host({
      'options.get': () => options,
      'options.set': (r) => {
        const { name, value } = r.params as { name: string; value: unknown };
        // The RPC server turned on: the user says no in the dialog of the desktop.
        if (name === 'server' && value === true) return { ...options, declined: true };
        Object.assign(options, { [name]: value, restartRequired: name !== 'natpmp' });
        return { ...options };
      },
      'options.validProxy': (r) => (r.params as { address: string }).address !== 'bad',
      'tray.state': () => ({ available: true, visible: true, windowVisible: true }),
    });
    nav.tab = 'main';
    render(Settings);
    await waitFor(() => screen.getByText('-listen=0'));
    await fireEvent.click(screen.getByLabelText('Prune block storage to'));
    await waitFor(() => expect(calls.find((c) => c.method === 'options.set')?.params).toEqual({ name: 'prune', value: true }));
    await waitFor(() => screen.getByText('This change would require a client restart.'));
    nav.tab = 'network';
    await waitFor(() => screen.getByLabelText('Allow incoming connections'));
    expect((screen.getByLabelText('Allow incoming connections') as HTMLInputElement).disabled).toBe(true);
    await fireEvent.click(screen.getByLabelText('Connect through SOCKS5 proxy (default proxy)'));
    await waitFor(() => expect((document.getElementById('opt-proxy-ip') as HTMLInputElement).disabled).toBe(false));
    await fireEvent.input(document.getElementById('opt-proxy-ip')!, { target: { value: 'bad' } });
    await fireEvent.change(document.getElementById('opt-proxy-ip')!);
    await waitFor(() => screen.getByText('The supplied proxy address is invalid.'));
    // Declined in the confirmation of the desktop: the switch shows what is still set.
    nav.tab = 'main';
    const server = (await waitFor(() => {
      const el = document.getElementById('opt-server');
      if (!el) throw new Error('not yet');
      return el;
    })) as HTMLInputElement;
    await fireEvent.click(server);
    await waitFor(() => expect(calls.some((c) => c.method === 'options.set' && (c.params as { name: string }).name === 'server')).toBe(true));
    await waitFor(() => expect(server.checked).toBe(false));
  });
});

describe('tools', () => {
  it('searches the explorer by height, and shows a block with its transactions', async () => {
    const hash = '0b'.repeat(32);
    host({
      getbestblockhash: () => hash,
      getblock: () => ({ height: 130, time: 1_700_000_000, nTx: 1, size: 250, weight: 1000, difficulty: 4.6e-10, hash, tx: ['c0'.repeat(32)], merkleroot: 'c0'.repeat(32), confirmations: 1, bits: '207fffff', nonce: 1, version: 536870912, mediantime: 1_700_000_000, chainwork: '00' }),
      getrawmempool: () => ({}),
      getblockhash: () => hash,
    });
    prefs.explorerCount = 10;
    render(Explorer);
    await waitFor(() => screen.getAllByText(hash));
    await fireEvent.input(screen.getByLabelText('Search'), { target: { value: '130' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Search' }));
    await waitFor(() => screen.getByText('Block 130'));
    expect(screen.getByRole('list', { name: 'Transactions of the block' }).textContent).toContain('c0'.repeat(32));
  });

  it('hashes text and hexadecimal', async () => {
    host({
      'tools.hashes': (r) => {
        const p = r.params as { text: string; hex: boolean };
        return p.hex && p.text === 'zz' ? { error: 'This is not hexadecimal data.' } : { sha256: 'ba7816bf', sha256d: 'x', sha256dId: 'y', ripemd160: 'r', hash160: 'h', sha512: 's', sha1: 'a9993e36', bytes: 3 };
      },
    });
    nav.tab = 'hash';
    render(Crypto);
    await fireEvent.input(screen.getByLabelText('Data to hash'), { target: { value: 'abc' } });
    await waitFor(() => screen.getByText('ba7816bf'));
    expect(screen.getByText(/Bytes hashed: 3/)).toBeTruthy();
    await fireEvent.click(screen.getByLabelText(/The data is hexadecimal/));
    await fireEvent.input(screen.getByLabelText('Data to hash'), { target: { value: 'zz' } });
    await waitFor(() => screen.getByText('This is not hexadecimal data.'));
  });

  it('finds the stamp of a file, the oldest one', async () => {
    let step = 0;
    host({
      'file.choose': () => '/home/me/contract.pdf',
      'timestamp.hashFile': () => ({ sha256: 'ab'.repeat(32), size: 2048 }),
      getbestblockhash: () => 'tip',
      'timestamp.search': (r) => {
        const p = r.params as { payload: string; from: string; count: number };
        expect(p.payload).toBe('5354414d50' + 'ab'.repeat(32));
        step++;
        return step === 1 ? { searched: p.count, next: 'older', found: [{ height: 125, time: 1_700_000_000, txid: 'f1'.repeat(32), block: 'b1' }] } : { searched: 3, next: null, found: [{ height: 60, time: 1_600_000_000, txid: 'f2'.repeat(32), block: 'b2' }] };
      },
    });
    render(Timestamp);
    await fireEvent.click(screen.getByRole('button', { name: /Choose file/ }));
    await waitFor(() => screen.getByText('ab'.repeat(32)));
    await fireEvent.click(screen.getByRole('button', { name: 'Find the stamp of this file' }));
    await waitFor(() => screen.getByText(/This file was stamped in block 60/));
    expect(document.body.textContent).toContain('f2'.repeat(32));
  });

  it('lists the sidechain nodes and starts one with the wallet of this node', async () => {
    wallets.current = 'w';
    const calls = host({
      'sidechainNodes.list': () => ({
        server: false,
        network: 'regtest',
        sidechains: [{ name: 'Thunder', stem: 'thunder', slot: 2, description: 'Large blocks, for volume', builtin: true, dir: '/opt/thunder', installed: true, programs: '/opt/thunder', dataDir: '/d/thunder' }],
      }),
      'sidechainNodes.status': () => ({ stem: 'thunder', installed: true, running: false }),
      'sidechainNodes.start': () => ({ log: ['Thunder: node started'] }),
    });
    render(DialogHost);
    render(SidechainNodes);
    await waitFor(() => screen.getByText('Thunder'));
    expect(document.getElementById('sidechain-nodes-notice')?.textContent).toContain('This node does not accept RPC connections');
    await waitFor(() => screen.getByText('stopped'));
    await fireEvent.click(screen.getByRole('radio', { name: /Thunder/ }));
    await fireEvent.click(screen.getByRole('button', { name: 'Start node' }));
    await waitFor(() => expect(document.getElementById('sidechain-nodes-log')?.textContent).toContain('Thunder: node started'));
    expect(calls.find((c) => c.method === 'sidechainNodes.start')?.params).toEqual({ stem: 'thunder', wallet: 'w' });
  });

  const thunderEntry = { name: 'Thunder', stem: 'thunder', slot: 2, description: '', builtin: true, dir: '', installed: false, programs: '', dataDir: '/d/thunder' };

  it('downloads the release built into the app without an address from the page (security audit 2026-10-14, 2a)', async () => {
    const calls = host({
      'sidechainNodes.list': () => ({ server: true, network: 'regtest', sidechains: [{ ...thunderEntry, release: { version: '1.0', url: 'https://example.invalid/t.tar.gz', sha256: 'b'.repeat(64) } }] }),
      'sidechainNodes.download': () => ({ log: ['Thunder: the download was not accepted.'] }),
    });
    render(DialogHost);
    render(SidechainNodes);
    await waitFor(() => screen.getByText('Thunder'));
    await fireEvent.click(screen.getByRole('radio', { name: /Thunder/ }));
    await fireEvent.click(screen.getByRole('button', { name: /Download/ }));
    await waitFor(() => expect(calls.find((c) => c.method === 'sidechainNodes.download')?.params).toEqual({ stem: 'thunder', release: true }));
  });

  it('asks for an address and a hash when no release is built in; the host refuses http (security audit 2026-10-14, 2a)', async () => {
    const calls = host({
      'sidechainNodes.list': () => ({ server: true, network: 'regtest', sidechains: [thunderEntry] }),
      // The host refuses what is not https.
      'sidechainNodes.download': (r) => {
        const { url } = r.params as { url: string };
        if (!url.startsWith('https:')) throw { code: -1, message: 'The address of a release starts with https://' };
        return { log: [] };
      },
    });
    render(DialogHost);
    render(SidechainNodes);
    await waitFor(() => screen.getByText('Thunder'));
    await fireEvent.click(screen.getByRole('radio', { name: /Thunder/ }));
    await fireEvent.click(screen.getByRole('button', { name: /Download/ }));
    const dialog = await waitFor(() => {
      const d = document.querySelector('dialog[open]') as HTMLElement | null;
      expect(d?.querySelector('input')).toBeTruthy();
      return d!;
    });
    const input = dialog.querySelector('input') as HTMLInputElement;
    input.value = 'http://example.invalid/t.tar.gz';
    await fireEvent.input(input);
    await fireEvent.click([...dialog.querySelectorAll('button')].find((b) => b.textContent?.trim() === 'OK')!);
    // The hash is asked for next; then the host says no.
    const hashDialog = await waitFor(() => {
      const d = document.querySelector('dialog[open]') as HTMLElement | null;
      if (!d?.textContent?.includes('SHA-256')) throw new Error('no hash prompt');
      return d;
    });
    const hash = hashDialog.querySelector('input') as HTMLInputElement;
    hash.value = 'a'.repeat(64);
    await fireEvent.input(hash);
    await fireEvent.click([...hashDialog.querySelectorAll('button')].find((b) => b.textContent?.trim() === 'OK')!);
    await waitFor(() => expect(document.body.textContent).toContain('The address of a release starts with https://'));
    expect(calls.find((c) => c.method === 'sidechainNodes.download')?.params).toEqual({ stem: 'thunder', url: 'http://example.invalid/t.tar.gz', sha256: 'a'.repeat(64) });
  });
});

describe('the status bar', () => {
  it('shows when a proxy is used', async () => {
    host({ getnetworkinfo: () => ({ networks: [{ name: 'ipv4', proxy: '127.0.0.1:9050' }, { name: 'ipv6', proxy: '' }] }) });
    render(StatusBar);
    await waitFor(() => expect(document.getElementById('proxy-status')?.getAttribute('title')).toBe('Proxy is enabled: 127.0.0.1:9050'));
  });
});
