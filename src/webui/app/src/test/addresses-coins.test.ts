// The receiving addresses as a tab of Receive, and "Choose coins" on the Send page (user decisions,
// 2026-10-15: no Addresses page, no sending addresses; coin control in the page).
import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import DialogHost from '../components/DialogHost.svelte';
import CoinPanel from '../components/CoinPanel.svelte';
import CoinPanelHarness from './CoinPanelHarness.svelte';
import Receive from '../pages/Receive.svelte';
import Send from '../pages/Send.svelte';
import { addressTypeOf } from '../lib/address';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { dialogs } from '../lib/dialogs.svelte';
import { go } from '../lib/nav.svelte';
import { app, prefs, wallets } from '../lib/state.svelte';
import { parseRecords, receivingAddresses, type AddressEntry, type Outpoint } from '../lib/wallet';
import { resetWalletData } from '../lib/walletdata.svelte';
import { regtestP2tr, regtestP2wpkh } from './bech32';

const INFO = { name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, externalSigner: false, hd: true, canGetAddresses: true, taproot: true, defaultAddressType: 'bech32', confirmTarget: 6, requiredFeePerK: 1000, maxTxFee: 10_000_000, multiwallet: false, balance: 50, unconfirmed: 0, immature: 0 };

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
  prefs.coinControl = false;
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
  wallets.current = 'w';
  wallets.balances.w = { trusted: 5_000_000_000n, untrustedPending: 0n, immature: 0n };
});

afterEach(() => {
  setTransport(null);
  dialogs.queue.splice(0);
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
  go('overview');
});

describe('receiving addresses', () => {
  it('tell their type, whether used, what they received and when last', () => {
    const net = 'regtest';
    expect(addressTypeOf(regtestP2wpkh, net)).toBe('bech32');
    expect(addressTypeOf(regtestP2tr, net)).toBe('bech32m');
    const entries: AddressEntry[] = [
      { address: regtestP2wpkh, label: 'Salary', purpose: 'receive', mine: true },
      { address: regtestP2tr, label: '', purpose: 'receive', mine: true },
      { address: 'rchn1qsomeoneelse', label: 'Shop', purpose: 'send', mine: false },
    ];
    const records = parseRecords([
      ['aa'.repeat(32), 0, 'recv', regtestP2wpkh, 'Salary', 1_700_000_000, 250_000_000, 0, 'confirmed', 7, 0, true, 'k1'],
      ['bb'.repeat(32), 1, 'recv', regtestP2wpkh, 'Salary', 1_700_100_000, 50_000_000, 0, 'confirmed', 3, 0, true, 'k2'],
      ['cc'.repeat(32), 0, 'recv', regtestP2wpkh, 'Salary', 1_700_200_000, 70_000_000, 0, 'conflicted', 0, 0, false, 'k3'],
    ]);
    const rows = receivingAddresses(entries, records, (a) => addressTypeOf(a, net));
    // The sending address is not one of them.
    expect(rows.map((r) => r.address)).toEqual([regtestP2wpkh, regtestP2tr]);
    expect(rows[0]).toMatchObject({ type: 'bech32', used: true, received: 300_000_000n, lastUsed: 1_700_100_000 });
    expect(rows[1]).toMatchObject({ type: 'bech32m', used: false, received: 0n, lastUsed: 0 });
  });

  it('are a tab of Receive: listed, generated with a label and a type, copied, labelled again', async () => {
    const calls: BridgeRequest[] = [];
    let book: AddressEntry[] = [{ address: regtestP2wpkh, label: 'Salary', purpose: 'receive', mine: true }];
    setTransport(async (r) => {
      calls.push(r);
      const p = (r.params ?? {}) as Record<string, unknown>;
      switch (r.method) {
        case 'wallet.info':
          return INFO;
        case 'wallet.requests':
          return [];
        case 'wallet.addresses':
          return book;
        case 'wallet.transactions':
          return { height: 10, hash: 'h', records: [['aa'.repeat(32), 0, 'recv', regtestP2wpkh, 'Salary', 1_700_000_000, 250_000_000, 0, 'confirmed', 7, 0, true, 'k1']] };
        case 'wallet.newAddress':
          book = [...book, { address: regtestP2tr, label: String(p.label), purpose: 'receive', mine: true }];
          return regtestP2tr;
        case 'wallet.addressBook.edit':
          book = book.map((e) => (e.address === p.address ? { ...e, label: String(p.label) } : e));
          return { address: p.address, changed: true };
        case 'clipboard.write':
          return true;
        default:
          return null;
      }
    });
    go('receive', 'addresses');
    render(Receive);
    render(DialogHost);
    const grid = await screen.findByRole('grid', { name: 'Receiving addresses' });
    await waitFor(() => expect(grid.textContent).toContain('Salary'));
    await waitFor(() => expect(grid.textContent).toContain('2.50000000'));
    expect(grid.textContent).toContain('Used');
    expect(grid.textContent).toContain('Bech32 (SegWit)');

    // A new address, with its label and its type.
    await fireEvent.input(document.getElementById('addr-new-label')!, { target: { value: 'Savings' } });
    await fireEvent.change(document.getElementById('addr-new-type')!, { target: { value: 'bech32m' } });
    await fireEvent.click(screen.getByRole('button', { name: /Generate new address/ }));
    await waitFor(() => expect(calls.find((c) => c.method === 'wallet.newAddress')?.params).toMatchObject({ wallet: 'w', label: 'Savings', type: 'bech32m' }));
    await waitFor(() => expect(grid.textContent).toContain('Savings'));
    expect(grid.textContent).toContain('Unused');
    expect(grid.textContent).toContain('Bech32m (Taproot)');

    // The new one is selected: copied, then labelled again.
    await fireEvent.click(screen.getByRole('button', { name: /^Copy$/ }));
    await waitFor(() => expect(calls.find((c) => c.method === 'clipboard.write')?.params).toEqual({ text: regtestP2tr }));
    await fireEvent.click(screen.getByRole('button', { name: /Edit label/ }));
    const dialog = await screen.findByRole('dialog', { name: 'Edit receiving address' });
    await fireEvent.input(within(dialog).getByLabelText('Label'), { target: { value: 'Rainy day' } });
    await fireEvent.click(within(dialog).getByRole('button', { name: 'OK' }));
    await waitFor(() => expect(calls.find((c) => c.method === 'wallet.addressBook.edit')?.params).toMatchObject({ address: regtestP2tr, label: 'Rainy day' }));
    await waitFor(() => expect(grid.textContent).toContain('Rainy day'));

    // Its QR code.
    await fireEvent.click(screen.getByRole('button', { name: /QR code/ }));
    await waitFor(() => expect(document.querySelector('dialog[open] svg.qr')).toBeTruthy());
  }, 15000);
});

describe('choose coins on the Send page', () => {
  const COINS = [
    {
      address: regtestP2wpkh,
      label: 'Salary',
      coins: [
        { txid: 'aa'.repeat(32), vout: 0, address: regtestP2wpkh, label: 'Salary', change: false, sats: 100_000_000, time: 1_700_000_000, depth: 12, locked: false },
        { txid: 'bb'.repeat(32), vout: 1, address: regtestP2wpkh, label: 'Salary', change: false, sats: 30_000_000, time: 1_700_100_000, depth: 0, locked: false },
        { txid: 'cc'.repeat(32), vout: 2, address: regtestP2wpkh, label: 'Salary', change: false, sats: 500_000_000, time: 1_699_000_000, depth: 99, locked: true },
      ],
    },
  ];

  function transport(calls: BridgeRequest[], locked: Set<string>) {
    return async (r: BridgeRequest) => {
      calls.push(r);
      const p = (r.params ?? {}) as Record<string, unknown>;
      switch (r.method) {
        case 'wallet.info':
          return INFO;
        case 'wallet.fee':
          return { feePerK: 2000, fallback: false, target: 6, requiredFeePerK: 1000, reason: '' };
        case 'wallet.coins':
          return COINS.map((g) => ({ ...g, coins: g.coins.map((c) => ({ ...c, locked: locked.has(`${c.txid}:${c.vout}`) })) }));
        case 'wallet.coins.lock':
          if (p.lock) locked.add(`${p.txid}:${p.vout}`);
          else locked.delete(`${p.txid}:${p.vout}`);
          return true;
        case 'wallet.coinControl.summary': {
          const sel = p.selected as Outpoint[];
          const amount = COINS[0].coins.filter((c) => sel.some((o) => o.txid === c.txid && o.vout === c.vout)).reduce((s, c) => s + c.sats, 0);
          const want = (p.amounts as number[]).reduce((s, a) => s + a, 0);
          return { quantity: sel.length, amount, fee: 300, afterFee: amount - 300, bytes: 150, change: amount - want - 300, changeEnabled: true, insufficient: amount < want + 300, feeVary: 1, spent: [] };
        }
        default:
          return null;
      }
    };
  }

  it('is a toggle of the page: off, the wallet chooses; on, the coins in the page', async () => {
    const calls: BridgeRequest[] = [];
    setTransport(transport(calls, new Set(['cc'.repeat(32) + ':2'])));
    render(Send);
    const toggle = screen.getByRole('switch', { name: 'Choose coins' });
    expect((toggle as HTMLInputElement).checked).toBe(false);
    expect(screen.getByText(/Coins are chosen automatically/)).toBeTruthy();
    await fireEvent.click(toggle);
    expect(prefs.coinControl).toBe(true);
    const grid = await screen.findByRole('grid', { name: 'Coins of the wallet' });
    await waitFor(() => expect(within(grid).getAllByRole('row').length).toBe(4));
    // Not a dialog.
    expect(document.querySelector('dialog[open]')).toBeNull();
    await fireEvent.click(toggle);
    expect(screen.queryByRole('grid', { name: 'Coins of the wallet' })).toBeNull();
  });

  it('selects coins, totals them against the amount, warns when they are not enough, locks and unlocks', async () => {
    const calls: BridgeRequest[] = [];
    const locked = new Set(['cc'.repeat(32) + ':2']);
    setTransport(transport(calls, locked));
    render(CoinPanelHarness, { props: { amounts: [120_000_000n] } });
    const chosen = (): Outpoint[] => JSON.parse(screen.getByTestId('selected').textContent || '[]');
    const grid = await screen.findByRole('grid', { name: 'Coins of the wallet' });
    await waitFor(() => expect(within(grid).getAllByRole('checkbox').length).toBe(2));
    // The locked coin has no box to tick.
    expect(within(grid).getAllByLabelText('Locked').length).toBe(1);
    expect(screen.getByText('Locked', { selector: '.total span' })).toBeTruthy();

    // One coin of 1 CHN for 1.2 CHN: not enough.
    const boxes = within(grid).getAllByRole('checkbox') as HTMLInputElement[];
    const first = boxes.find((b) => b.closest('[role="row"]')!.textContent!.includes('1.00000000'))!;
    await fireEvent.click(first);
    await waitFor(() => expect(chosen()).toEqual([{ txid: 'aa'.repeat(32), vout: 0 }]));
    const alert = await screen.findByRole('alert');
    expect(alert.textContent).toContain('Not enough');
    expect(alert.textContent).toContain('1.00000000 CHN');
    expect(alert.textContent).toContain('1.20000000 CHN');
    expect(screen.getByText(/Only these 1 coin\(s\) are spent/)).toBeTruthy();

    // All of them (the free ones): 1.3 CHN, enough.
    await fireEvent.click(screen.getByRole('button', { name: 'Select all' }));
    await waitFor(() => expect(chosen().length).toBe(2));
    await waitFor(() => expect(screen.queryByRole('alert')).toBeNull());
    expect(document.querySelector('.totals')!.textContent).toContain('1.30000000');
    await waitFor(() => expect(calls.some((c) => c.method === 'wallet.coinControl.summary' && (c.params as { selected: unknown[] }).selected.length === 2)).toBe(true));

    // Lock one chosen: no longer chosen, and not enough again.
    const lockButtons = within(grid).getAllByRole('button', { name: 'Lock coin' });
    const lockFirst = lockButtons.find((b) => b.closest('[role="row"]')!.textContent!.includes('1.00000000'))!;
    await fireEvent.click(lockFirst);
    await waitFor(() => expect(calls.find((c) => c.method === 'wallet.coins.lock')?.params).toMatchObject({ txid: 'aa'.repeat(32), vout: 0, lock: true }));
    await waitFor(() => expect(chosen()).toEqual([{ txid: 'bb'.repeat(32), vout: 1 }]));
    await waitFor(() => expect(screen.getByRole('alert').textContent).toContain('Not enough'));

    // Unlock the coin of 5 CHN and take it: enough.
    const unlock = within(grid).getAllByRole('button', { name: 'Unlock coin' }).find((b) => b.closest('[role="row"]')!.textContent!.includes('5.00000000'))!;
    await fireEvent.click(unlock);
    await waitFor(() => expect(locked.has('cc'.repeat(32) + ':2')).toBe(false));
    await waitFor(() => expect(within(grid).getAllByRole('checkbox').length).toBe(2));
    const big = (within(grid).getAllByRole('checkbox') as HTMLInputElement[]).find((b) => b.closest('[role="row"]')!.textContent!.includes('5.00000000'))!;
    await fireEvent.click(big);
    await waitFor(() => expect(chosen().length).toBe(2));
    await waitFor(() => expect(screen.queryByRole('alert')).toBeNull());

    // None.
    await fireEvent.click(screen.getByRole('button', { name: 'Select none' }));
    expect(chosen()).toEqual([]);
    expect(screen.getByText(/No coin chosen yet/)).toBeTruthy();
  }, 15000);

  it('filters and sorts the coins', async () => {
    setTransport(transport([], new Set(['cc'.repeat(32) + ':2'])));
    render(CoinPanel, { props: { wallet: 'w', selected: [], amounts: [0n], subtractFee: false, fee: { confTarget: 6 } } });
    const grid = await screen.findByRole('grid', { name: 'Coins of the wallet' });
    await waitFor(() => expect(within(grid).getAllByRole('row').length).toBe(4));
    // Biggest first.
    const amounts = () => within(grid).getAllByRole('row').slice(1).map((r) => r.querySelectorAll('[role="gridcell"]')[1].textContent!.trim());
    expect(amounts()).toEqual(['5.00000000', '1.00000000', '0.30000000']);
    await fireEvent.change(screen.getByLabelText('Coins shown'), { target: { value: 'unconfirmed' } });
    await waitFor(() => expect(amounts()).toEqual(['0.30000000']));
    await fireEvent.change(screen.getByLabelText('Coins shown'), { target: { value: 'locked' } });
    await waitFor(() => expect(amounts()).toEqual(['5.00000000']));
    await fireEvent.change(screen.getByLabelText('Coins shown'), { target: { value: 'all' } });
    await fireEvent.input(screen.getByLabelText('Search the coins'), { target: { value: 'bbbb' } });
    await waitFor(() => expect(amounts()).toEqual(['0.30000000']));
  });
});
