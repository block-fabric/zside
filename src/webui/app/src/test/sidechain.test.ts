// The app of a sidechain (chain.json role "sidechain"): its chain in one place, the pages of the kit
// it does not have, its own pages (Peg, Merged mining, its home, the Mainchain tab of Settings) and
// what they ask of the node and of the mainchain node (src/webui/sidechainapi.cpp: peg.*).
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import { CHAIN, SIDECHAIN, chainHasPage } from '../chain/config';
import { LOCAL_COMMANDS, LOCAL_PAGES, type LocalCommand, type LocalPage } from '../chain/local';
import DialogHost from '../components/DialogHost.svelte';
import MainchainSettings from '../components/MainchainSettings.svelte';
import Bmm from '../pages/Bmm.svelte';
import Peg from '../pages/Peg.svelte';
import SideHome from '../views/home/SideHome.svelte';
import { checkAddress, networkParams } from '../lib/address';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { buildCommands, chainHas, menuSections, pageAvailable } from '../lib/commands.svelte';
import { dialogs } from '../lib/dialogs.svelte';
import { handleShortcut } from '../lib/keyboard';
import { AREAS, chainBeforeTools, HOME_QUICK_ACTIONS, NAV_SECTIONS } from '../lib/layout';
import { go, nav, PAGES, type PageId } from '../lib/nav.svelte';
import { FIRST_STEPS } from '../lib/firststeps';
import type { NodeOptions } from '../lib/rpc';
import { bmmBlocks, bmmShare, bundleHistory, depositsOf, depositTarget, EventLog, voteProgress, type SideEvent } from '../lib/side';
import { noteBmm, side, stopSide } from '../lib/sidestate.svelte';
import { app, node, prefs, wallets } from '../lib/state.svelte';
import { UNITS } from '../lib/units';
import { parsePaymentUri } from '../lib/uri';
import { resetWalletData } from '../lib/walletdata.svelte';
import { encode, program } from './bech32';

const SLOT = 7;
const MAIN_HRP = CHAIN.mainchain?.networks.regtest.hrp ?? 'rchn';
const mainAddress = encode(MAIN_HRP, [0, ...program(20)], false);
const ownAddress = encode(CHAIN.networks.regtest.hrp, [0, ...program(20)], false);
const DEPOSIT = `s${SLOT}_${ownAddress}_abc123`;

function event(height: number, extra: Partial<SideEvent> = {}): SideEvent {
  return { height, hash: `h${height}`, previousblockhash: `h${height - 1}`, time: 1_700_000_000 + height * 60, deposits: [], bundles: [], proposed: [], pending: [], ...extra };
}

/** The mainchain blocks of the fake: a deposit at 98 (credited), one at 101 (on its way), a bundle proposed at 99, BMM every other block. */
const EVENTS: SideEvent[] = Array.from({ length: 6 }, (_, i) => {
  const h = 96 + i;
  const e = event(h, { bmm: h % 2 === 0 ? 'bb'.repeat(32) : undefined });
  if (h === 98) e.deposits = [{ destination: DEPOSIT, amount: 1.5, txid: 'd1'.repeat(32), burnindex: 0 }];
  if (h === 101) e.deposits = [{ destination: DEPOSIT, amount: 2, txid: 'd2'.repeat(32), burnindex: 1 }];
  if (h === 99) e.proposed = ['cc'.repeat(32)];
  if (h >= 99) e.pending = [{ hash: 'cc'.repeat(32), score: h - 98 }];
  return e;
});

const calls: BridgeRequest[] = [];
let mining = false;

async function fakeNode(r: BridgeRequest): Promise<unknown> {
  calls.push(r);
  const p = (r.params ?? {}) as Record<string, unknown>;
  switch (r.method) {
    case 'getmainchaininfo':
      return { slot: SLOT, connected: true, node: '127.0.0.1:29554', height: 101, bestblockhash: 'h101', tipheight: 100 };
    case 'getbmminfo':
      return mining ? { mining: true, address: ownAddress, always: false, idle: false, lastfees: 0.001, lastoffer: 0.00099, requests: 4, blocks: 3, outbid: 1, candidates: 1 } : { mining: false, requests: 4, blocks: 3, outbid: 1, candidates: 0 };
    case 'getwithdrawalbundle':
      return { status: 'pending', hash: 'cc'.repeat(32), height: 50, withdrawals: 1, amount: 0.5, mainchainfee: 0.0001, waiting: 1 };
    case 'listwithdrawals':
      return [
        { txid: 'w1'.repeat(32), vout: 0, amount: 0.5, mainchainfee: 0.0001, mainchainscript: '00', refundaddress: ownAddress, height: 40, status: 'bundled' },
        { txid: 'w2'.repeat(32), vout: 1, amount: 0.25, mainchainfee: 0.0001, mainchainscript: '00', refundaddress: ownAddress, height: 45, status: 'waiting' },
      ];
    case 'getaddressinfo':
      return { ismine: true };
    case 'getdepositaddress':
      return { depositaddress: DEPOSIT, address: ownAddress, slot: SLOT };
    case 'getblockheader':
      return { height: 10, time: 1_700_000_000, nTx: 1, confirmations: 1 };
    case 'getmininginfo':
      return { blocks: 10, difficulty: 1, networkhashps: 0, pooledtx: 0, chain: 'regtest' };
    case 'getpeerinfo':
      return [];
    case 'getbalances':
      return { mine: { trusted: 1, untrusted_pending: 0, immature: 0 } };
    case 'bmm.set':
      mining = p.mine === true;
      return { mining };
    case 'createwithdrawal':
      return { txid: 'w3'.repeat(32), vout: 0, refundaddress: ownAddress };
    case 'refundwithdrawal':
      return { txid: 'r1'.repeat(32), amount: 0.2501, refundaddress: ownAddress };
    case 'bmm.request':
      return { hash: 'bc'.repeat(32), mainchainheight: 101, transactions: 1, fees: 0, amount: 0.0001, mainchaintxid: 'mt'.repeat(32) };
    case 'peg.connection':
      return { connected: true, node: '127.0.0.1:29554', slot: SLOT, walletSet: true, wallet: 'miner', auth: 'cookie', cookieFile: '/home/u/.chains/regtest/.cookie' };
    case 'peg.call':
      switch (p.method) {
        case 'getdrivechaininfo':
          return { withdrawalperiod: 60, withdrawalminscore: 30, height: 101 };
        case 'listwithdrawalbundles':
          return [{ slot: SLOT, hash: 'cc'.repeat(32), index: 0, height: 99, score: 3, lastupvote: 101, blocksleft: 57, payable: false, known: true, vote: 'upvote' }];
        case 'getsidechainevents': {
          const [, from, count] = p.params as number[];
          return EVENTS.filter((e) => e.height >= from && e.height < from + count);
        }
        case 'getbalances':
          return { mine: { trusted: 12 } };
        case 'getnewaddress':
          return mainAddress;
        default:
          return null;
      }
    case 'peg.deposit':
      return { txid: 'dd'.repeat(32), total: 10 };
    case 'wallet.transactions':
      return { height: 10, hash: 'h', records: [] };
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
});

beforeEach(() => {
  app.info = { name: CHAIN.name, version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 60 };
  prefs.unit = 'CHN';
  prefs.mask = false;
  prefs.layout = 'dashboard';
  node.tip = { height: 10, time: 1_700_000_000, hash: 'ab'.repeat(32), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
  wallets.current = 'w';
  wallets.balances.w = { trusted: 100_000_000n, untrustedPending: 0n, immature: 0n };
  calls.length = 0;
  mining = false;
  setTransport(fakeNode);
});

afterEach(() => {
  stopSide();
  side.log = [];
  side.main.events = [];
  side.main.height = -1;
  setTransport(null);
  dialogs.queue.splice(0);
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
});

async function answer(choice: string) {
  await waitFor(() => expect(document.querySelector(`button[data-choice="${choice}"]`)).not.toBeNull());
  await fireEvent.click(document.querySelector(`button[data-choice="${choice}"]`)!);
}

describe.runIf(SIDECHAIN)('the chain of a sidechain, in one place (chain.json)', () => {
  it('names its coin, its payment URIs and its addresses', async () => {
    expect(UNITS[0].short).toBe(CHAIN.coin.ticker);
    expect(UNITS[1].short).toBe(`m${CHAIN.coin.ticker}`);
    expect(parsePaymentUri(`${CHAIN.uriScheme}:${ownAddress}?amount=1`)).toMatchObject({ ok: true, request: { address: ownAddress, amount: 100_000_000n } });
    expect(networkParams('regtest').hrp).toBe(CHAIN.networks.regtest.hrp);
    expect(await checkAddress(ownAddress, 'regtest')).toMatchObject({ valid: true });
    // An address of the mainchain is said to be one: an easy slip, either way.
    expect(await checkAddress(mainAddress, 'regtest')).toMatchObject({ valid: false, error: `This is an address of ${CHAIN.mainchain?.name}, not of ${CHAIN.name}` });
    expect(await checkAddress(mainAddress, 'regtest', 'mainchain')).toMatchObject({ valid: true });
    expect(await checkAddress(ownAddress, 'regtest', 'mainchain')).toMatchObject({ valid: false, error: `This is an address of ${CHAIN.name}, not of ${CHAIN.mainchain?.name}` });
  });

  it('drops the pages of the mainchain: no sidechains of its own, no solo mining', () => {
    for (const id of ['sidechains', 'sidechain-nodes', 'mine'] as PageId[]) {
      expect(chainHasPage(id), id).toBe(false);
      expect(chainHas(id), id).toBe(false);
      expect(pageAvailable(id), id).toBe(false);
      expect(NAV_SECTIONS.some((s) => s.pages.includes(id)), id).toBe(false);
      expect(AREAS.some((a) => a.tabs.some((t) => t.page === id)), id).toBe(false);
    }
    const all = buildCommands();
    expect(all.some((c) => c.page === 'sidechains' || c.page === 'mine')).toBe(false);
    // A page dropped is never shown: going there shows the overview.
    expect(AREAS.map((a) => a.id)).toEqual(['home', 'wallet', 'chain', 'tools', 'node']);
  });

  it('has its peg and merged mining in the Chain section, before the tools, and Alt+5 opens the peg', () => {
    // The pages of the chain of its own (chain/local.ts) in the Chain section go between them and the explorer.
    const own = (LOCAL_PAGES as readonly LocalPage[]).filter((p) => p.section === 'chain').map((p) => p.id);
    // Merged mining is a tab of Drivechains (user decision, 2026-10-15), not a page of the sidebar.
    expect(NAV_SECTIONS.find((s) => s.title === 'Chain')!.pages).toEqual(['peg', ...own, 'explorer']);
    expect(AREAS.find((a) => a.id === 'chain')!.tabs.map((t) => t.page)).toEqual(['peg', ...own, 'explorer']);
    expect(chainBeforeTools(HOME_QUICK_ACTIONS.map((q) => q.page), (p) => p)).toBeNull();
    const sections = menuSections(buildCommands());
    const chain = sections.find((s) => s.group === 'Chains')!;
    expect(chain.title).toBe(CHAIN.name);
    expect(chain.items.map((c) => c.label)).toEqual(['Drivechains', 'Deposit…', 'Withdraw…', 'Withdrawal bundle', 'Merged mining', ...(LOCAL_COMMANDS as readonly LocalCommand[]).map((c) => c.label), 'Block explorer']);
    chain.items.find((c) => c.label === 'Withdraw…')!.run();
    expect([nav.page, nav.tab]).toEqual(['peg', 'withdraw']);
    chain.items.find((c) => c.label === 'Merged mining')!.run();
    expect([nav.page, nav.tab]).toEqual(['peg', 'bmm']);
    go('overview');
    handleShortcut(new KeyboardEvent('keydown', { key: '5', altKey: true }));
    expect(nav.page).toBe('peg');
    // The help links of chain.json are in the menus.
    const links = buildCommands().filter((c) => c.id.startsWith('help.'));
    expect(links.length).toBe(Object.values(CHAIN.links).filter(Boolean).length);
  });
});

describe('the peg and merged mining, as the pages put them together', () => {
  it('keeps the events of the mainchain up to date: the new blocks, all again after a reorganisation', async () => {
    const asked: [number, number][] = [];
    let chain = EVENTS;
    const log = new EventLog(SLOT);
    const ask = async (from: number, count: number) => {
      asked.push([from, count]);
      return chain.filter((e) => e.height >= from && e.height < from + count);
    };
    expect((await log.update(101, ask)).map((e) => e.height)).toEqual([96, 97, 98, 99, 100, 101]);
    expect(asked[0]).toEqual([0, 102]);
    chain = [...EVENTS, event(102)];
    await log.update(102, ask);
    expect(asked[1][0]).toBeGreaterThan(90);
    expect(log.events.map((e) => e.height)).toEqual([96, 97, 98, 99, 100, 101, 102]);
    // A reorganisation within the blocks asked again: they replace those kept.
    chain = chain.map((e) => (e.height >= 98 ? { ...e, hash: `y${e.height}`, previousblockhash: e.height === 98 ? 'h97' : `y${e.height - 1}` } : e));
    await log.update(102, ask);
    expect(asked[asked.length - 1]).toEqual([97, 6]);
    expect(log.events.find((e) => e.height === 100)?.hash).toBe('y100');
    // Deeper: the first block asked again no longer follows the last one kept, so all of them again.
    chain = chain.map((e) => (e.height >= 96 ? { ...e, hash: `x${e.height}`, previousblockhash: e.height === 96 ? 'h95' : `x${e.height - 1}` } : e));
    await log.update(102, ask);
    expect(asked[asked.length - 1]).toEqual([0, 103]);
    expect(log.events.map((e) => e.hash)).toEqual(['x96', 'x97', 'x98', 'x99', 'x100', 'x101', 'x102']);
  });

  it('tells the bundles, the deposits and the blocks that committed to this chain', () => {
    const history = bundleHistory(EVENTS);
    expect(history).toEqual([{ hash: 'cc'.repeat(32), proposed: 99, proposedTime: EVENTS[3].time, score: 3 }]);
    const closed = bundleHistory([...EVENTS, event(102, { bundles: [{ hash: 'cc'.repeat(32), paid: true }] })]);
    expect(closed[0]).toMatchObject({ closed: 102, paid: true });
    const deposits = depositsOf(EVENTS, 100);
    expect(deposits.map((d) => [d.height, d.credited, d.sats])).toEqual([
      [101, false, 200_000_000n],
      [98, true, 150_000_000n],
    ]);
    // The change of a withdrawal is not a deposit.
    expect(depositsOf([event(1, { deposits: [{ destination: 'D', amount: 1, txid: 'aa', burnindex: 0, bundle: 'bb' }] })], 0)).toEqual([]);
    expect(bmmShare(EVENTS, 4)).toEqual({ committed: 2, of: 4 });
    expect(bmmBlocks(EVENTS, 2).map((b) => b.height)).toEqual([101, 100]);
    expect(depositTarget(DEPOSIT, SLOT)).toBe(ownAddress);
    expect(depositTarget(DEPOSIT, SLOT + 1)).toBeNull();
    expect(depositTarget('plain text', SLOT)).toBeNull();
    expect(voteProgress(15, 30)).toBe(50);
    expect(voteProgress(45, 30)).toBe(100);
  });

  it('keeps a log of merged mining: requests, blocks mined, outbids, errors', () => {
    side.log = [];
    const before = { mining: false, requests: 0, blocks: 0, outbid: 0, candidates: 0 };
    noteBmm(before, { ...before, mining: true });
    noteBmm({ ...before, mining: true }, { ...before, mining: true, requests: 2, blocks: 1, outbid: 1, lastfees: 0.01, lastoffer: 0.0099, error: 'no funds' });
    expect(side.log.map((e) => e.kind).sort()).toEqual(['error', 'mined', 'on', 'outbid', 'request']);
    expect(side.log.find((e) => e.kind === 'request')).toMatchObject({ count: 2, fees: 0.01, offer: 0.0099 });
  });
});

describe.runIf(SIDECHAIN)('the pages of a sidechain', () => {
  it('Peg → Deposit: a deposit address, the helper of the mainchain wallet, the deposits', async () => {
    go('peg', 'deposit');
    render(Peg);
    render(DialogHost);
    await waitFor(() => expect(screen.getAllByText('Connected').length).toBeGreaterThan(0));
    await fireEvent.click(screen.getByRole('button', { name: /New deposit address/ }));
    await waitFor(() => expect(document.getElementById('deposit-address')?.textContent).toContain(DEPOSIT));
    // The mainchain wallet's balance, then the helper: peg.deposit, which the host confirms on the desktop
    // (audit port-1 #4: no dialog of the page, which a compromised page could skip).
    await waitFor(() => expect(calls.some((c) => c.method === 'peg.call' && (c.params as Record<string, unknown>).method === 'getbalances')).toBe(true));
    await fireEvent.input(screen.getByLabelText('Amount'), { target: { value: '1.25' } });
    await fireEvent.click(screen.getByRole('button', { name: /^Deposit$/ }));
    await waitFor(() => expect(calls.find((c) => c.method === 'peg.deposit')?.params).toEqual({ destination: DEPOSIT, amount: '1.25' }));
    // The deposits of the mainchain blocks on record: one credited, one on its way, both to this wallet.
    const table = await waitFor(() => screen.getByRole('table', { name: 'Deposits to this chain' }));
    await waitFor(() => expect(within(table).getAllByText('yours').length).toBe(2));
    expect(within(table).getByText('On its way')).toBeTruthy();
    expect(within(table).getByText('Credited')).toBeTruthy();
    // In a narrow panel the rows are cards: every cell carries the words of its column (audit port-1 #9).
    const cells = within(table).getAllByRole('cell');
    expect(cells[0].dataset.label).toBe('State');
    expect(cells[3].dataset.label).toBe('Amount');
    const css = readFileSync(join(__dirname, '../styles/side.css'), 'utf8');
    expect(css).toMatch(/@container \(max-width: 36rem\)[\s\S]*td\[data-label\]::before/);
    // As the other long lists (user decision): rows per page and how many of the latest are kept.
    expect([...(screen.getByLabelText('Rows per page') as HTMLSelectElement).options].map((o) => o.value)).toEqual(['25', '50', '100']);
    const latest = screen.getByLabelText('How many of the latest deposits') as HTMLSelectElement;
    expect([...latest.options].map((o) => o.value)).toEqual(['25', '50', '100', '250', '500', '1000', '0']);
    expect(latest.options[latest.options.length - 1].textContent).toBe('All');
    await fireEvent.change(latest, { target: { value: '25' } });
    await waitFor(() => expect(within(screen.getByRole('table', { name: 'Deposits to this chain' })).getAllByText('yours').length).toBe(2));
    // Merged mining: a tab of Drivechains, next to Bundle (user decision).
    expect(screen.getAllByRole('tab').map((x) => x.textContent?.trim().replace(/\d+$/, '').trim())).toEqual(['Deposit', 'Withdraw', 'Bundle', 'Merged mining']);
  });

  it('Peg → Withdraw: to an address of the mainchain, confirmed; the withdrawals, the ones of the wallet taken back', async () => {
    go('peg', 'withdraw');
    render(Peg);
    render(DialogHost);
    const address = await waitFor(() => screen.getByLabelText(`Address of ${CHAIN.mainchain?.name}`));
    // An address of this chain is refused: the mainchain pays the withdrawal.
    await fireEvent.input(address, { target: { value: ownAddress } });
    await waitFor(() => screen.getByText(`This is an address of ${CHAIN.name}, not of ${CHAIN.mainchain?.name}`));
    await fireEvent.click(screen.getByRole('button', { name: new RegExp(`An address of the ${CHAIN.mainchain?.name} wallet`) }));
    await waitFor(() => expect((screen.getByLabelText(`Address of ${CHAIN.mainchain?.name}`) as HTMLInputElement).value).toBe(mainAddress));
    await fireEvent.input(screen.getByLabelText('Amount'), { target: { value: '0.3' } });
    await new Promise((r) => setTimeout(r, 300));
    await fireEvent.click(screen.getByRole('button', { name: /^Withdraw$/ }));
    await answer('yes');
    await waitFor(() => expect(calls.find((c) => c.method === 'createwithdrawal')?.params).toEqual([mainAddress, '0.3', '0.0001']));
    // The list: the bundled one cannot be taken back, the waiting one can.
    const table = await waitFor(() => screen.getByRole('table', { name: 'Withdrawals not paid yet' }));
    await waitFor(() => expect(within(table).getAllByText('yours').length).toBe(2));
    const back = within(table).getAllByRole('button', { name: 'Take back' });
    expect(back.length).toBe(1);
    await fireEvent.click(back[0]);
    await answer('yes');
    await waitFor(() => expect(calls.find((c) => c.method === 'refundwithdrawal')?.params).toEqual(['w2'.repeat(32), 1]));
  });

  it('Peg → Bundle: its votes on the mainchain, and the bundles before it', async () => {
    go('peg', 'bundle');
    render(Peg);
    await waitFor(() => screen.getByText('3 of 30'));
    expect(screen.getByText(/57 blocks left/)).toBeTruthy();
    const table = await waitFor(() => screen.getByRole('table', { name: 'Bundles' }));
    expect(within(table).getByText('Being voted on')).toBeTruthy();
  });

  it('Merged mining: on and off, one block by hand, the blocks of the mainchain, the connection', async () => {
    render(Bmm);
    render(DialogHost);
    await waitFor(() => expect(document.getElementById('bmm-blocks')?.textContent).toBe('3'));
    await fireEvent.input(screen.getByLabelText('Pay the fees to'), { target: { value: ownAddress } });
    await fireEvent.click(screen.getByRole('button', { name: /Start mining/ }));
    await waitFor(() => expect(calls.find((c) => c.method === 'bmm.set')?.params).toEqual({ mine: true, address: ownAddress, always: false }));
    await waitFor(() => expect((screen.getByRole('button', { name: 'Stop mining' }) as HTMLButtonElement).disabled).toBe(false));
    await fireEvent.click(screen.getByRole('button', { name: 'Stop mining' }));
    await waitFor(() => expect(calls.filter((c) => c.method === 'bmm.set').at(-1)?.params).toEqual({ mine: false }));
    await fireEvent.click(screen.getByRole('button', { name: /Mine one block/ }));
    await waitFor(() => expect(calls.find((c) => c.method === 'bmm.request')?.params).toEqual({ address: ownAddress, amount: '0.0001' }));
    // Nothing that spends the mainchain wallet is an RPC call of the page.
    expect(calls.some((c) => c.method === 'setbmm' || c.method === 'requestbmmblock' || c.method === 'createsidechaindeposit')).toBe(false);
    await waitFor(() => expect(document.getElementById('bmm-once-result')?.textContent).toContain('Asked for block'));
    // The blocks of the mainchain on record, and whether the blocks they committed to are in this chain.
    await waitFor(() => expect(screen.getAllByText('In this chain').length).toBeGreaterThan(0));
    expect(screen.getByText('its cookie file')).toBeTruthy();
    expect(screen.getByText(/Asked for one block by hand/)).toBeTruthy();
  });

  it('the home: the cards of the balance, the network, the peg and merged mining', async () => {
    go('home');
    render(SideHome);
    for (const title of ['Balance', 'Network', 'Drivechains', 'Merged mining', 'Wallet balance', `Blocks of ${CHAIN.mainchain?.name}`]) {
      expect(screen.getByRole('heading', { name: title }), title).toBeTruthy();
    }
    const peg = screen.getByRole('heading', { name: 'Drivechains' }).closest('section')!;
    await waitFor(() => expect(within(peg).getByText('3 / 30 votes')).toBeTruthy());
    await fireEvent.click(within(peg).getByRole('button', { name: /Deposit/ }));
    expect([nav.page, nav.tab]).toEqual(['peg', 'deposit']);
    const tabs = screen.getByRole('tablist', { name: 'Activity' });
    expect(within(tabs).getAllByRole('tab').map((t) => t.textContent?.replace(/\d+/g, '').trim())).toEqual(['Transactions', 'Blocks', 'Peers']);
  });

  it('Settings → the mainchain: its address, port and wallet; its files from the file chooser only', async () => {
    const options = { mainchainConnect: '', mainchainPort: '', mainchainWallet: 'miner', mainchainUser: '', mainchainCookieFile: '', mainchainPasswordFile: '', mainchainDefaults: { port: 29554, connect: '127.0.0.1' }, mainchainIgnored: {} } as unknown as NodeOptions;
    const set: string[][] = [];
    let chosen: NodeOptions | null = null;
    setTransport(async (r) => {
      calls.push(r);
      if (r.method === 'options.chooseFile') return { ...options, mainchainCookieFile: '/home/u/.chains/regtest/.cookie' };
      return null;
    });
    render(MainchainSettings, { options, set: async (name, value) => void set.push([name, String(value)]), changed: (next) => (chosen = next) });
    expect((screen.getByLabelText('RPC port') as HTMLInputElement).placeholder).toBe('29554');
    await fireEvent.input(screen.getByLabelText('RPC port'), { target: { value: '70000' } });
    await fireEvent.change(screen.getByLabelText('RPC port'));
    await waitFor(() => screen.getByText('The port is a number from 1 to 65535.'));
    expect(set).toEqual([]);
    await fireEvent.input(screen.getByLabelText('RPC port'), { target: { value: '18000' } });
    await fireEvent.change(screen.getByLabelText('RPC port'));
    await waitFor(() => expect(set).toEqual([['mainchainPort', '18000']]));
    // The cookie file: read only, set by the chooser of the desktop.
    expect((screen.getByLabelText('Cookie file') as HTMLInputElement).readOnly).toBe(true);
    await fireEvent.click(screen.getAllByRole('button', { name: 'Choose…' })[0]);
    await waitFor(() => expect(calls.find((c) => c.method === 'options.chooseFile')?.params).toEqual({ name: 'mainchainrpccookiefile' }));
    await waitFor(() => expect(chosen).not.toBeNull());
  });
});

// Simple mode (lib/uimode.svelte.ts, user decision 2026-10-16): coins in and back in plain words, the bundle
// and merged mining in Advanced; the same calls as Advanced.
describe('Simple mode on a sidechain', () => {
  beforeEach(() => {
    prefs.uiMode = 'simple';
    prefs.loaded = true;
    prefs.tipsDone = false;
  });

  it('the home: its first steps, merged mining in a line, no card of merged mining', async () => {
    go('home');
    render(SideHome);
    expect(screen.getByRole('heading', { name: `Welcome to ${CHAIN.name}` })).toBeTruthy();
    expect(document.querySelectorAll('.tips li').length).toBe(3);
    expect(screen.queryByRole('heading', { name: 'Merged mining' })).toBeNull();
    await waitFor(() => expect(screen.getByTestId('bmm-line').textContent).toMatch(/merge-mined by/));
    // The quick actions leave merged mining out; Drivechains stays.
    const quick = screen.getByRole('navigation', { name: 'Quick actions' });
    expect(within(quick).queryByRole('button', { name: /Merged mining/ })).toBeNull();
    expect(within(quick).getByRole('button', { name: /Bring coins in/ })).toBeTruthy();
    await fireEvent.click(screen.getByRole('button', { name: 'Details' }));
    await waitFor(() => expect(document.querySelector('.side-blocks')).toBeTruthy());
    // A step of the tips opens its page (the chain's own steps, chain/firststeps.ts).
    const step = FIRST_STEPS.find((s) => s.action && !s.action.panel)!.action!;
    await fireEvent.click(within(document.querySelector('.tips') as HTMLElement).getByRole('button', { name: new RegExp(step.label) }));
    expect(nav.page).toBe(PAGES[step.page as PageId].tabOf?.page ?? step.page);
  });

  it('Drivechains: two tabs in plain words, the withdrawal with the call of Advanced', async () => {
    go('peg', 'withdraw');
    render(Peg);
    render(DialogHost);
    expect(screen.getAllByRole('tab').map((x) => x.textContent?.trim().replace(/\d+$/, '').trim())).toEqual(['Bring coins in', 'Send coins back']);
    expect(document.querySelector('.side-stats')).toBeNull();
    const address = await waitFor(() => screen.getByLabelText(`Address of ${CHAIN.mainchain?.name}`));
    await fireEvent.input(address, { target: { value: mainAddress } });
    await fireEvent.input(screen.getByLabelText('Amount'), { target: { value: '0.3' } });
    await new Promise((r) => setTimeout(r, 300));
    await fireEvent.click(screen.getByRole('button', { name: /^Withdraw$/ }));
    await answer('yes');
    await waitFor(() => expect(calls.find((c) => c.method === 'createwithdrawal')?.params).toEqual([mainAddress, '0.3', '0.0001']));
    // The withdrawals waiting, without blocks and ids; how far the votes are, in a line.
    const table = await waitFor(() => screen.getByRole('table', { name: 'Withdrawals not paid yet' }));
    expect(within(table).queryByText('Block')).toBeNull();
    await waitFor(() => screen.getByText(/The withdrawals being voted on are 10% approved, 57 blocks left/));
  });

  it('Drivechains: the bundle when asked for, and every tab in Advanced', async () => {
    go('peg', 'bundle');
    render(Peg);
    await waitFor(() => screen.getByText('3 of 30'));
    prefs.uiMode = 'advanced';
    await waitFor(() => expect(screen.getAllByRole('tab').length).toBe(4));
    expect(document.querySelector('.side-stats')).toBeTruthy();
  });
});
