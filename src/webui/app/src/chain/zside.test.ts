// The pages of zSide: what model.ts works out (shielded addresses and viewing keys of each network, memos,
// the history told in words, the bills of a cast, the plan, the estimate of a proof), and the pages driven
// against a pretend node: the shielded wallet (balances with their privacy, a private payment with a memo,
// an unshielding to a transparent address, shielding and unshielding, new addresses, the viewing key
// exported after its warning and one watched), the memos inbox, the history with a fee bump and the notes,
// melt & cast, the pool, the cards of the home, and where the pages are in the menus.
import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import { CHAIN } from './config';
import { LOCAL_COMMANDS, LOCAL_PAGES, type LocalPage } from './local';
import LocalCards from './LocalCards.svelte';
import DialogHost from '../components/DialogHost.svelte';
import MeltCast from './pages/MeltCast.svelte';
import Memos from './pages/Memos.svelte';
import Pool from './pages/Pool.svelte';
import Private from './pages/Private.svelte';
import ZHistory from './pages/ZHistory.svelte';
import ProvingPanel from './zs/ProvingPanel.svelte';
import {
  billsOf,
  BILL_UNIT,
  describeTx,
  isShieldedAddress,
  isViewingKey,
  MAX_CAST,
  memoBytes,
  memosOf,
  proofEstimate,
  proofProgress,
  PROOF_GUESS_MS,
  shieldedHrp,
  sortPlan,
  viewingKeyHrp,
  type NoteRaw,
  type PlanStep,
  type ShieldedTxRaw,
} from './zs/model';
import { isRead, prove, proving, resetZs, zs } from './zs/store.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { buildCommands, menuSections } from '../lib/commands.svelte';
import { dialogs } from '../lib/dialogs.svelte';
import { handleShortcut } from '../lib/keyboard';
import { AREAS, chainBeforeTools, HOME_QUICK_ACTIONS, NAV_SECTIONS } from '../lib/layout';
import { go, nav } from '../lib/nav.svelte';
import { app, node, prefs, wallets } from '../lib/state.svelte';
import { coinsToSats } from '../lib/units';
import { resetWalletData } from '../lib/walletdata.svelte';
import { encode, program } from '../test/bech32';

const TRANSPARENT = encode(CHAIN.networks.regtest.hrp, [0, ...program(20)], false);
const ZADDR = encode('rzo', program(43), true);
const ZADDR2 = encode('rzo', program(43).map((v) => (v * 3 + 1) % 32), true);
const ZKEY = encode('rzview', program(96), true);
const id = (c: string) => c.repeat(64);

const TXS: ShieldedTxRaw[] = [
  // Received privately, with a memo whose first character turns text around.
  { txid: id('1'), time: 1_700_000_300, confirmations: 0, received: 0.25, spent: 0, shielded: 0, unshielded: 0, memos: ['‮evil memo', 'second line'], payments: [] },
  // Paid privately: 1.5 to another, change 3.4999, fee 0.0001.
  { txid: id('2'), time: 1_700_000_200, confirmations: 0, received: 3.4999, spent: 5, shielded: 0, unshielded: 0.0001, memos: [], payments: [{ address: ZADDR2, amount: 1.5, memo: 'for the coffee', to_self: false }, { address: ZADDR, amount: 3.4999, memo: '', to_self: true }] },
  // Unshielded: 1 to a transparent address, the rest back.
  { txid: id('3'), time: 1_700_000_150, confirmations: 3, received: 1, spent: 2.0001, shielded: 0, unshielded: 1.0001, memos: [], payments: [] },
  // Shielded 5, with a note to self.
  { txid: id('4'), time: 1_700_000_100, confirmations: 5, received: 5, spent: 0, shielded: 5, unshielded: 0, memos: ['to myself'], payments: [] },
];

const NOTES: NoteRaw[] = [
  { txid: id('2'), action: 1, amount: 3.4999, memo: '', change: true, confirmations: 0 },
  { txid: id('4'), action: 0, amount: 5, memo: 'to myself', change: false, confirmations: 5, spent_by: id('2') },
  { txid: id('5'), action: 0, amount: 0.3, memo: 'seen with a key', change: false, confirmations: 2, watchonly: true, viewonly: true },
];

const PLAN: PlanStep[] = [
  { id: 1, kind: 'melt', amount: 2, coin: `${id('6')}:0`, status: 'sent', txid: id('7'), confirmations: 2 },
  { id: 2, kind: 'melt', amount: 3, coin: `${id('8')}:1`, status: 'planned', height: 30 },
  { id: 3, kind: 'cast', amount: 0.04, status: 'waiting', height: 25, error: 'The wallet is locked' },
];

const calls: BridgeRequest[] = [];
let privateKeysDisabled = false;
let balance: Record<string, number> = { available: 4.9999, pending: 0.25, notes: 2 };

async function fakeNode(r: BridgeRequest): Promise<unknown> {
  calls.push(r);
  const p = (r.params ?? []) as unknown[];
  switch (r.method) {
    case 'getshieldedinfo':
      return { pool: 12.5, notes: 40, spent: 30, root: 'ab'.repeat(32), index: { notes: 40, root: 'ab'.repeat(32), synced: true } };
    case 'getshieldedbalance':
      return balance;
    case 'listshieldedtransactions':
      return TXS;
    case 'listshieldednotes':
      return NOTES;
    case 'listmeltcast':
      return PLAN;
    case 'getshieldedaddress':
      return p[0] === true ? ZADDR2 : ZADDR;
    case 'getbalances':
      return { mine: { trusted: 3, untrusted_pending: 0, immature: 0 } };
    case 'validateaddress':
      return { isvalid: p[0] === TRANSPARENT };
    case 'sendshielded':
    case 'shieldcoins':
      return { txid: id('a') };
    case 'unshieldcoins':
      return { txid: id('b'), address: TRANSPARENT, amount: 4.9998 };
    case 'bumpshieldedfee':
      return { txid: id('c'), origfee: 0.0001, fee: 0.0002 };
    case 'exportshieldedviewingkey':
      return ZKEY;
    case 'importshieldedviewingkey':
      return { scanned_from: Number(p[1]) };
    case 'meltcoins':
      return [PLAN[1]];
    case 'castcoins':
      return [PLAN[2], PLAN[2]];
    case 'cancelmeltcast':
      return p[0] === 'all' ? 2 : 1;
    case 'wallet.info':
      return { name: 'w', encrypted: false, locked: false, privateKeysDisabled, externalSigner: false, hd: true, canGetAddresses: !privateKeysDisabled, taproot: true, defaultAddressType: 'bech32', confirmTarget: 6, requiredFeePerK: 1000, maxTxFee: 0.1, multiwallet: false, balance: 3, unconfirmed: 0, immature: 0 };
    case 'prefs.get':
      return {};
    case 'prefs.set':
      return {};
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
  (globalThis as Record<string, unknown>).ResizeObserver ??= class {
    observe() {}
    unobserve() {}
    disconnect() {}
  };
  window.matchMedia ??= ((query: string) => ({ matches: false, media: query, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia;
});

beforeEach(() => {
  app.info = { name: CHAIN.name, version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 60 } as typeof app.info;
  prefs.unit = 'CHN';
  prefs.mask = false;
  node.tip = { height: 20, time: 1_700_000_000, hash: 'ab'.repeat(32), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }] as typeof wallets.list;
  wallets.current = 'w';
  privateKeysDisabled = false;
  balance = { available: 4.9999, pending: 0.25, notes: 2 };
  calls.length = 0;
  try {
    localStorage.clear();
  } catch {
    // None.
  }
  resetZs();
  setTransport(fakeNode);
});

afterEach(() => {
  resetZs();
  setTransport(null);
  dialogs.queue.splice(0);
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
  nav.tab = '';
});

async function answer(choice: string) {
  await waitFor(() => expect(document.querySelector(`button[data-choice="${choice}"]`)).not.toBeNull());
  const button = document.querySelector(`button[data-choice="${choice}"]`) as HTMLButtonElement;
  await waitFor(() => expect(button.disabled).toBe(false), { timeout: 5000 });
  await fireEvent.click(button);
}
const called = (method: string) => calls.filter((c) => c.method === method);
const loaded = () => waitFor(() => expect(zs.loaded && zs.balance !== null && zs.txs.length > 0).toBe(true));

describe('zSide: what the pages work out', () => {
  it('knows the shielded addresses and the viewing keys of each network', () => {
    expect([shieldedHrp('main'), shieldedHrp('test'), shieldedHrp('signet'), shieldedHrp('regtest')]).toEqual(['zo', 'tzo', 'tzo', 'rzo']);
    expect([viewingKeyHrp('main'), viewingKeyHrp('regtest')]).toEqual(['zview', 'rzview']);
    expect(isShieldedAddress(ZADDR, 'regtest')).toBe(true);
    expect(isShieldedAddress(ZADDR, 'main')).toBe(false);
    expect(isShieldedAddress(TRANSPARENT, 'regtest')).toBe(false);
    // A typo breaks the checksum; bech32 (not bech32m) is not one; 42 bytes is not one.
    expect(isShieldedAddress(ZADDR.slice(0, -1) + (ZADDR.endsWith('q') ? 'p' : 'q'), 'regtest')).toBe(false);
    expect(isShieldedAddress(encode('rzo', program(43), false), 'regtest')).toBe(false);
    expect(isShieldedAddress(encode('rzo', program(42), true), 'regtest')).toBe(false);
    expect(isViewingKey(ZKEY, 'regtest')).toBe(true);
    expect(isViewingKey(ZKEY, 'main')).toBe(false);
    expect(isViewingKey(ZADDR, 'regtest')).toBe(false);
    expect(isViewingKey(ZKEY.toUpperCase(), 'regtest')).toBe(false);
  });

  it('counts a memo in bytes, as the node does', () => {
    expect(memoBytes('abc')).toBe(3);
    expect(memoBytes('é')).toBe(2);
    expect(memoBytes('€'.repeat(171))).toBe(513);
  });

  it('tells the history in words, with its privacy', () => {
    const [received, paid, unshielded, shielded] = TXS.map(describeTx);
    expect(received).toMatchObject({ kind: 'receive', amount: coinsToSats(0.25), public: false });
    expect(paid).toMatchObject({ kind: 'pay', amount: -coinsToSats(1.5001), fee: coinsToSats(0.0001), public: false });
    // The account's own change is not a payment.
    expect(paid.to).toEqual([{ address: ZADDR2, amount: coinsToSats(1.5), memo: 'for the coffee' }]);
    expect(unshielded).toMatchObject({ kind: 'unshield', amount: -coinsToSats(1.0001), public: true });
    expect(shielded).toMatchObject({ kind: 'shield', amount: coinsToSats(5), public: true });
  });

  it('lists the memos received, newest first, one entry each', () => {
    const memos = memosOf(TXS);
    expect(memos.map((m) => m.text)).toEqual(['‮evil memo', 'second line', 'to myself']);
    expect(memos[0].key).toBe(`${id('1')}:0`);
    expect(memos[0].amount).toBe(coinsToSats(0.25));
  });

  it('works out the bills of a cast', () => {
    expect(billsOf(coinsToSats(1.5)).bills).toEqual([coinsToSats(1.28), coinsToSats(0.16), coinsToSats(0.04), coinsToSats(0.02)]);
    expect(billsOf(coinsToSats(0.015))).toEqual({ bills: [BILL_UNIT], rest: coinsToSats(0.005), tooMuch: false });
    expect(billsOf(MAX_CAST).bills.length).toBe(21);
    expect(billsOf(MAX_CAST + BILL_UNIT).tooMuch).toBe(true);
  });

  it('orders the plan: what is to go, soonest first, then what went', () => {
    expect(sortPlan(PLAN).map((s) => s.id)).toEqual([3, 2, 1]);
  });

  it('estimates a proof from the earlier ones, and never says done before the node', () => {
    expect(proofEstimate([])).toBe(PROOF_GUESS_MS);
    expect(proofEstimate([1000, 9000, 2000])).toBe(2000);
    expect(proofProgress(0, 5000)).toBe(0);
    expect(proofProgress(5000, 5000)).toBeGreaterThan(0.75);
    expect(proofProgress(500_000, 5000)).toBeLessThan(1);
  });
});

describe.runIf(CHAIN.name === 'zSide')('the pages of zSide', () => {
  it('are in the Wallet and Chain sections and the menus, chain before tools, with a quick action and Alt+6', () => {
    expect((LOCAL_PAGES as readonly LocalPage[]).map((p) => p.id)).toEqual(['private', 'memos', 'zhistory', 'meltcast', 'pool']);
    const wallet = NAV_SECTIONS.find((s) => s.title === 'Wallet')!.pages;
    for (const p of ['private', 'memos', 'zhistory', 'meltcast']) expect(wallet).toContain(p);
    expect(NAV_SECTIONS.find((s) => s.title === 'Chain')!.pages).toContain('pool');
    expect(AREAS.find((a) => a.id === 'chain')!.tabs.map((t) => t.page)).toContain('pool');
    expect(chainBeforeTools(HOME_QUICK_ACTIONS.map((q) => q.page), (p) => p)).toBeNull();
    expect(HOME_QUICK_ACTIONS.map((q) => q.label)).toContain('Pay privately');
    const chain = menuSections(buildCommands()).find((s) => s.group === 'Chains')!;
    for (const c of LOCAL_COMMANDS) expect(chain.items.map((i) => i.label)).toContain(c.label);
    chain.items.find((c) => c.label === 'Viewing keys…')!.run();
    expect([nav.page, nav.tab]).toEqual(['private', 'keys']);
    go('overview');
    expect(handleShortcut(new KeyboardEvent('keydown', { key: '6', altKey: true }))).toBe(true);
    expect(nav.page).toBe('private');
    expect(CHAIN.uriScheme).toBe('zside');
  });

  it('Private: both balances, each with its privacy, and how much is shielded', async () => {
    render(Private);
    await loaded();
    const balances = screen.getByRole('region', { name: 'Balances' });
    await waitFor(() => expect(within(balances).getByText('Shielded')).toBeTruthy());
    expect(within(balances).getAllByLabelText(/^Private:/).length).toBe(1);
    expect(within(balances).getAllByLabelText(/^Public:/).length).toBe(1);
    // 4.9999 + 0.25 shielded, 3 transparent.
    expect(screen.getByRole('img', { name: '64% of the wallet is shielded' })).toBeTruthy();
    // The latest: each with its privacy.
    expect(screen.getAllByText('Paid privately').length).toBe(1);
    expect(screen.getAllByLabelText(/^(Private|Public):/).length).toBeGreaterThanOrEqual(6);
  });

  it('Pay: privately with a memo, after the confirmation; the proof shows meanwhile', async () => {
    go('private', 'pay');
    render(Private);
    render(DialogHost);
    await loaded();
    await fireEvent.input(document.querySelector('#zs-pay-to-0')!, { target: { value: ZADDR2 } });
    await waitFor(() => expect(screen.getByText('A shielded address')).toBeTruthy());
    await fireEvent.input(document.querySelector('#zs-pay-amount-0')!, { target: { value: '1.5' } });
    const memo = document.querySelector('#zs-pay-memo') as HTMLTextAreaElement;
    expect(memo.disabled).toBe(false);
    await fireEvent.input(memo, { target: { value: 'é'.repeat(257) } });
    await waitFor(() => expect(screen.getByRole('alert').textContent).toMatch(/longer than 512 bytes/));
    await fireEvent.input(memo, { target: { value: 'for the coffee' } });
    await waitFor(() => expect(screen.queryByRole('alert')).toBeNull());
    const payButton = screen.getByRole('button', { name: 'Prove and pay' }) as HTMLButtonElement;
    await waitFor(() => expect(payButton.disabled).toBe(false));
    await fireEvent.click(payButton);
    await answer('yes');
    await waitFor(() => expect(called('sendshielded').length).toBe(1));
    expect(called('sendshielded')[0].params).toEqual([{ [ZADDR2]: '1.5' }, 'for the coffee']);
    expect(called('sendshielded')[0].kind).toBe('rpc');
  });

  it('Pay: to a transparent address it unshields, says so, and sends no memo', async () => {
    go('private', 'pay');
    render(Private);
    render(DialogHost);
    await loaded();
    await fireEvent.input(document.querySelector('#zs-pay-to-0')!, { target: { value: TRANSPARENT } });
    await waitFor(() => expect(screen.getByText('A transparent address')).toBeTruthy(), { timeout: 3000 });
    expect((document.querySelector('#zs-pay-memo') as HTMLTextAreaElement).disabled).toBe(true);
    expect(screen.getByText(/this unshields/)).toBeTruthy();
    await fireEvent.input(document.querySelector('#zs-pay-amount-0')!, { target: { value: '1' } });
    const payButton = screen.getByRole('button', { name: 'Prove and pay' }) as HTMLButtonElement;
    await waitFor(() => expect(payButton.disabled).toBe(false));
    await fireEvent.click(payButton);
    await waitFor(() => expect(document.querySelector('.dialog, dialog[open]')?.textContent).toMatch(/will be public/));
    await answer('yes');
    await waitFor(() => expect(called('sendshielded').length).toBe(1));
    expect(called('sendshielded')[0].params).toEqual([{ [TRANSPARENT]: '1' }, '']);
  });

  it('Shield and unshield: the amounts, all of it, and where to', async () => {
    go('private', 'move');
    render(Private);
    render(DialogHost);
    await loaded();
    await fireEvent.input(document.querySelector('#zs-shield-amount')!, { target: { value: '2' } });
    await fireEvent.input(document.querySelector('#zs-shield-memo')!, { target: { value: 'savings' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Shield' }));
    await answer('yes');
    await waitFor(() => expect(called('shieldcoins').length).toBe(1));
    expect(called('shieldcoins')[0].params).toEqual(['2', null, 'savings']);

    const allButtons = screen.getAllByRole('button', { name: 'All' });
    await fireEvent.click(allButtons[1]);
    await fireEvent.click(screen.getByRole('button', { name: 'Unshield' }));
    await answer('yes');
    await waitFor(() => expect(called('unshieldcoins').length).toBe(1));
    expect(called('unshieldcoins')[0].params).toEqual(['all', null, true]);
  });

  it('Receive: the address of the account, and a new one nobody can link to it', async () => {
    go('private', 'receive');
    render(Private);
    await loaded();
    await waitFor(() => expect(screen.getByText(ZADDR)).toBeTruthy());
    expect(screen.getByRole('img', { name: 'QR code of the shielded address' })).toBeTruthy();
    await fireEvent.click(screen.getByRole('button', { name: /New address/ }));
    await waitFor(() => expect(called('getshieldedaddress').some((c) => (c.params as unknown[])[0] === true)).toBe(true));
    await waitFor(() => expect(screen.getByText(ZADDR2)).toBeTruthy());
  });

  it('Viewing keys: exported only once the warning is understood; watched only in a wallet without keys', async () => {
    go('private', 'keys');
    const page = render(Private);
    render(DialogHost);
    await loaded();
    await fireEvent.click(screen.getByRole('button', { name: /Show the viewing key/ }));
    // Not ticked: no key.
    await answer('yes');
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toMatch(/Tick the box/));
    await answer('ok');
    expect(called('exportshieldedviewingkey').length).toBe(0);
    await fireEvent.click(screen.getByRole('button', { name: /Show the viewing key/ }));
    await waitFor(() => expect(document.querySelector('dialog[open] input[type="checkbox"]')).not.toBeNull());
    await fireEvent.click(document.querySelector('dialog[open] input[type="checkbox"]')!);
    await answer('yes');
    await waitFor(() => expect(called('exportshieldedviewingkey').length).toBe(1));
    // Shown blurred until asked.
    await waitFor(() => expect(screen.getByText(ZKEY).classList.contains('hidden')).toBe(true));
    await fireEvent.click(screen.getByRole('button', { name: /Show$/ }));
    expect(screen.getByText(ZKEY).classList.contains('hidden')).toBe(false);
    // This wallet has keys: no import here.
    expect(document.querySelector('#zs-import-key')).toBeNull();
    page.unmount();

    privateKeysDisabled = true;
    wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: true, hd: false }] as typeof wallets.list;
    resetWalletData();
    render(Private);
    await waitFor(() => expect(document.querySelector('#zs-import-key')).not.toBeNull());
    await fireEvent.input(document.querySelector('#zs-import-key')!, { target: { value: ZADDR } });
    await waitFor(() => expect(screen.getByText('Not a viewing key of this network')).toBeTruthy());
    await fireEvent.input(document.querySelector('#zs-import-key')!, { target: { value: ZKEY } });
    await fireEvent.input(document.querySelector('#zs-import-height')!, { target: { value: '120' } });
    await fireEvent.click(screen.getByRole('button', { name: /^Watch$/ }));
    await answer('yes');
    await waitFor(() => expect(called('importshieldedviewingkey').length).toBe(1));
    expect(called('importshieldedviewingkey')[0].params).toEqual([ZKEY, 120]);
  }, 20000);

  it('Memos: the inbox, unread first marked, read in its pane, as text', async () => {
    render(Memos);
    await loaded();
    await waitFor(() => expect(screen.getAllByRole('option').length).toBe(3));
    expect(screen.getAllByLabelText('Unread').length).toBe(3);
    await fireEvent.click(screen.getAllByRole('option')[0]);
    expect(isRead(`${id('1')}:0`)).toBe(true);
    await waitFor(() => expect(screen.getAllByLabelText('Unread').length).toBe(2));
    // Shown exactly as written, isolated as to direction.
    const reader = screen.getByRole('region', { name: 'The memo' });
    const text = within(reader).getByText('‮evil memo');
    expect(text.getAttribute('dir')).toBe('auto');
    expect(text.getAttribute('translate')).toBe('no');
    await fireEvent.input(screen.getByRole('searchbox', { name: 'Search the memos' }), { target: { value: 'myself' } });
    await waitFor(() => expect(screen.getAllByRole('option').length).toBe(1));
    await fireEvent.click(screen.getByRole('button', { name: /Mark all read/ }));
    await waitFor(() => expect(screen.queryAllByLabelText('Unread').length).toBe(0));
  });

  it('Private activity: the transactions in words with their privacy, a fee bump, the notes', async () => {
    render(ZHistory);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(screen.getByText('Paid privately')).toBeTruthy());
    expect(screen.getByText('Received privately')).toBeTruthy();
    expect(screen.getByText('Unshielded')).toBeTruthy();
    expect(screen.getByText('Shielded')).toBeTruthy();
    await fireEvent.click(screen.getByRole('radio', { name: 'Public' }));
    await waitFor(() => expect(screen.queryByText('Paid privately')).toBeNull());
    expect(screen.getByText('Unshielded')).toBeTruthy();
    await fireEvent.click(screen.getByRole('radio', { name: 'All' }));
    await fireEvent.click(screen.getByText('Paid privately'));
    await waitFor(() => expect(screen.getByText('for the coffee')).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: /Pay a higher fee/ }));
    await answer('yes');
    await waitFor(() => expect(called('bumpshieldedfee').length).toBe(1));
    expect(called('bumpshieldedfee')[0].params).toEqual([id('2')]);

    nav.tab = 'notes';
    await waitFor(() => expect(screen.getByRole('table', { name: 'Notes' })).toBeTruthy());
    expect(within(screen.getByRole('table', { name: 'Notes' })).getAllByRole('row').length).toBe(3);
    expect(screen.getByText('watched')).toBeTruthy();
    await fireEvent.click(screen.getByLabelText(/Show the spent notes/));
    await waitFor(() => expect(within(screen.getByRole('table', { name: 'Notes' })).getAllByRole('row').length).toBe(4));
  });

  it('Melt & cast: the plan, a melt, a cast with its bills, a step cancelled', async () => {
    render(MeltCast);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(screen.getByRole('table', { name: 'The plan' })).toBeTruthy());
    const rows = within(screen.getByRole('table', { name: 'The plan' })).getAllByRole('row');
    expect(rows.length).toBe(4);
    expect(rows[1].textContent).toMatch(/Cast/);
    expect(rows[1].textContent).toMatch(/The wallet is locked/);
    await fireEvent.click(within(rows[1]).getByRole('button', { name: 'Cancel' }));
    await answer('yes');
    await waitFor(() => expect(called('cancelmeltcast')[0]?.params).toEqual(['3']));

    nav.tab = 'melt';
    await waitFor(() => expect(document.querySelector('#zs-melt-spread')).not.toBeNull());
    await fireEvent.input(document.querySelector('#zs-melt-spread')!, { target: { value: '144' } });
    await fireEvent.click(screen.getByRole('button', { name: /Melt my transparent coins/ }));
    await answer('yes');
    await waitFor(() => expect(called('meltcoins')[0]?.params).toEqual([144]));

    nav.tab = 'cast';
    await waitFor(() => expect(document.querySelector('#zs-cast-amount')).not.toBeNull());
    await fireEvent.input(document.querySelector('#zs-cast-amount')!, { target: { value: '1.5' } });
    await waitFor(() => expect(screen.getByText(/Comes out as 4 bill/)).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: /^Cast$/ }));
    await answer('yes');
    await waitFor(() => expect(called('castcoins')[0]?.params).toEqual(['1.5', 1]));
  });

  it('the pool, the proof under way, and the cards of the home', async () => {
    const pool = render(Pool);
    await waitFor(() => expect(screen.getByText('Notes made')).toBeTruthy());
    await waitFor(() => expect(screen.getByText(/index of the whole tree is synced/)).toBeTruthy());
    pool.unmount();

    // A proof: shown while the node works, gone once it answers; its time kept for the next estimate.
    render(ProvingPanel);
    let release: (v: unknown) => void = () => {};
    const running = prove('pay', 'Proving the payment', () => new Promise((r) => (release = r)));
    await waitFor(() => expect(screen.getByRole('status').textContent).toMatch(/Proving the payment/));
    expect(screen.getByRole('progressbar', { name: 'Proving' })).toBeTruthy();
    release({ txid: id('a') });
    await running;
    expect(proving.now).toBeNull();
    await waitFor(() => expect(screen.queryByRole('status')).toBeNull());
    expect(JSON.parse(localStorage.getItem('zside.proofs') ?? '{}').pay.length).toBe(1);

    render(LocalCards);
    await loaded();
    await waitFor(() => expect(screen.getByRole('heading', { name: 'Private' })).toBeTruthy());
    expect(screen.getByRole('heading', { name: 'Memos' })).toBeTruthy();
    expect(screen.getByRole('heading', { name: 'Melt & cast' })).toBeTruthy();
    expect(screen.getByText('to myself')).toBeTruthy();
  });

  it('a locked wallet whose account is not set up is asked to unlock once', async () => {
    const realFake = fakeNode;
    setTransport(async (r) => {
      if (r.method === 'getshieldedbalance') throw Object.assign(new Error('The shielded account is not set up yet'), { code: -13 });
      return realFake(r);
    });
    render(Private);
    await waitFor(() => expect(screen.getByRole('button', { name: 'Unlock to set up' })).toBeTruthy());
  });
});
