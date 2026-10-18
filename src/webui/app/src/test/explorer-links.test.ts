// The links of the block explorer of a sidechain: blocks, transactions and addresses open their views, Back returns
// along them; the addresses of the open wallet are marked "yours" (asked of the wallet in one call for those shown,
// kept); the address view shows what the wallet knows of its own addresses and, of any other, only what the address
// itself says (no scan of the chain). The peg of a block: deposits paid by its coinbase (the transaction of the mainchain
// copied, never linked), withdrawals and refunds linked to their transactions; the block of the mainchain that merged
// mined it, copied with a word on where to see it.
import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import Explorer from '../pages/Explorer.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { depositTarget, matchDeposits, mayBeAddress, scriptKindText, type AddressActivity, type BlockPeg } from '../lib/explorer';
import { nav } from '../lib/nav.svelte';
import { ownerOf, resetOwned } from '../lib/owned.svelte';
import { side } from '../lib/sidestate.svelte';
import { app, node, prefs, wallets } from '../lib/state.svelte';
import { CHAIN, SIDECHAIN } from '../chain/config';
import { encode, program } from './bech32';

const HRP = CHAIN.networks.regtest.hrp;
const hashOf = (h: number) => h.toString(16).padStart(64, '0');
const MINE = encode(HRP, [0, ...program(20)], false);
const OTHER = encode(HRP, [0, ...Array.from({ length: 32 }, (_, i) => (i * 7 + 3) % 32)], false);
const CB = 'cb'.repeat(32);
const PREV = 'ab'.repeat(32);
const SPENDER = 'ef'.repeat(32);
const WITHDRAW = 'ad'.repeat(32);
const MAIN_DEPOSIT = 'de'.repeat(32);
const MAIN_BLOCK = 'cb'.repeat(16) + 'cc'.repeat(16);
const PEG: BlockPeg = {
  coinbase: CB,
  payouts: [
    [1, 100_000_000, MINE],
    [2, 50_000_000, OTHER],
  ],
  bundle: null,
  withdrawals: [[WITHDRAW, 0, 99_999_000, 1000, '0014' + '42'.repeat(20), OTHER]],
  refunds: [],
  more: false,
};
const ACTIVITY: AddressActivity = {
  mine: true,
  label: 'Savings',
  received: 10_000_000_000,
  spent: 2_500_000_000,
  balance: 7_500_000_000,
  immature: 0,
  txCount: 2,
  coinCount: 1,
  history: [
    [SPENDER, 31, 1_700_000_100, 0, 2_500_000_000],
    [CB, 30, 1_700_000_000, 5_000_000_000, 0],
  ],
  coins: [[CB, 1, 5_000_000_000, 30, false]],
};

describe.runIf(SIDECHAIN)('the links of the explorer', () => {
  let calls: BridgeRequest[] = [];
  /** The transaction a view read (with the coins it spends: verbosity 2); the ribbon reads coinbases for their tags. */
  const viewRead = () => calls.find((c) => c.method === 'getrawtransaction' && (c.params as unknown[])[1] === 2)?.params;
  beforeEach(() => {
    app.info = { name: CHAIN.name, version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp/x', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 600 };
    prefs.unit = 'CHN';
    prefs.mask = false;
    prefs.uiMode = 'advanced';
    prefs.explorerCount = 25;
    prefs.explorerPageSize = 10;
    nav.page = 'explorer';
    nav.tab = '';
    nav.focus = null;
    wallets.current = 'w';
    resetOwned();
    node.tip = { height: 31, time: 1_700_000_000, hash: hashOf(31), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
    // The mainchain block that merged mined block 30, and the deposit it recorded (as the app follows the mainchain).
    side.main.events = [{ height: 120, hash: MAIN_BLOCK, time: 1_700_000_000, bmm: hashOf(30), deposits: [{ destination: `s1_${MINE}_abcdef`, amount: 1, txid: MAIN_DEPOSIT, burnindex: 0 }], bundles: [], proposed: [], pending: [] }];
    calls = [];
    setTransport(async (r) => {
      calls.push(r);
      const p = r.params as Record<string, unknown> & unknown[];
      switch (r.method) {
        case 'explorer.blocks': {
          const { count, from } = r.params as { count: number; from?: number };
          const top = Math.min(from ?? 31, 31);
          return Array.from({ length: Math.max(0, Math.min(count, top + 1)) }, (_, i) => top - i).map((h) => [h, hashOf(h), 1_700_000_000 + h * 60, 1, 1]);
        }
        case 'explorer.peg':
          return PEG;
        case 'getblock':
          return { height: 30, time: 1_700_000_000, nTx: 2, size: 250, weight: 1000, difficulty: 1, hash: hashOf(30), tx: [CB, WITHDRAW], merkleroot: '00', confirmations: 2, bits: '207fffff', nonce: 1, version: 1, mediantime: 1_700_000_000, chainwork: '00', previousblockhash: hashOf(29) };
        case 'getblockhash':
          return hashOf(p[0] as number);
        case 'getrawtransaction':
          return {
            txid: p[0],
            hash: p[0],
            size: 200,
            vsize: 150,
            weight: 600,
            locktime: 0,
            version: 2,
            vin: [{ txid: PREV, vout: 1, sequence: 0, prevout: { height: 12, value: 1, scriptPubKey: { type: 'witness_v0_keyhash', address: OTHER } } }],
            vout: [{ value: 50, n: 0, scriptPubKey: { type: 'witness_v0_keyhash', address: MINE, asm: '', hex: '' } }],
            blockhash: p[2] ?? hashOf(30),
            confirmations: 2,
            fee: 0.0001,
          };
        case 'gettxspendingprevout':
          return [{ txid: p[0], vout: 0, spendingtxid: SPENDER }];
        case 'validateaddress':
          return String(p[0]).startsWith(HRP + '1') ? { isvalid: true, address: p[0], scriptPubKey: '0014' + '00'.repeat(20), iswitness: true, witness_version: 0, isscript: false } : { isvalid: false };
        case 'wallet.owned':
          return (r.params as { addresses: string[] }).addresses.filter((a) => a === MINE).map((a) => [a, 'Savings']);
        case 'wallet.address':
          return (r.params as { address: string }).address === MINE ? ACTIVITY : { mine: false };
        case 'getrawmempool':
          return {};
        case 'getblockstats':
          return { subsidy: 50, totalfee: 0, avgfeerate: 0 };
        default:
          throw { code: -32601, message: `No ${r.method}` };
      }
    });
  });
  afterEach(() => {
    setTransport(null);
    wallets.current = null;
    side.main.events = [];
    resetOwned();
  });

  async function openBlock30() {
    render(Explorer);
    const table = await waitFor(() => screen.getByRole('grid', { name: 'Blocks' }));
    // The hash of a row is a link to its block.
    const link = await waitFor(() => within(table).getByRole('button', { name: hashOf(30) }));
    await fireEvent.click(link);
    return waitFor(() => screen.getByRole('region', { name: 'The peg in this block' }));
  }

  it('the peg of a block: deposits marked yours with the transaction of the mainchain copied; Back returns to the block', async () => {
    const pegView = await openBlock30();
    const paid = within(pegView).getByRole('list', { name: 'Paid by the coinbase' });
    await waitFor(() => expect(paid.querySelector('[data-ours]')).not.toBeNull());
    expect(paid.textContent).toContain('yours');
    // The deposit the mainchain recorded to this address: its transaction there, copied, no link.
    expect(paid.textContent).toContain('deposit');
    const mainTx = [...paid.querySelectorAll('span[title]')].find((e) => e.textContent?.startsWith('dede'));
    expect(mainTx?.getAttribute('title')).toBe(`A transaction of ${CHAIN.mainchain!.name}: see it in the ${CHAIN.mainchain!.name} wallet`);
    expect(within(paid).queryByRole('button', { name: /^dede/ })).toBeNull();
    expect(paid.textContent).toContain('deposit or refund');
    // The addresses shown asked in one call, never one each.
    const owned = calls.filter((c) => c.method === 'wallet.owned');
    expect(owned.length).toBe(1);
    expect((owned[0].params as { addresses: string[] }).addresses).toContain(MINE);
    // The block of the mainchain that merged mined it: copied, its tooltip says where to see it.
    const facts = screen.getByRole('region', { name: 'Block 30' });
    const main = [...facts.querySelectorAll('dd')].find((d) => d.textContent?.includes(MAIN_BLOCK));
    expect(main?.getAttribute('title')).toBe(`A block of ${CHAIN.mainchain!.name}: see it in the ${CHAIN.mainchain!.name} wallet`);
    expect(within(main as HTMLElement).queryByRole('button', { name: new RegExp(MAIN_BLOCK) })).toBeNull();
    // The address: what the wallet knows of it.
    await fireEvent.click(within(paid).getByRole('button', { name: MINE }));
    const view = await waitFor(() => screen.getByRole('region', { name: 'Address' }));
    await waitFor(() => expect(view.textContent).toContain('Savings'));
    expect(view.textContent).toContain('SegWit (P2WPKH)');
    expect(within(view).getByRole('table', { name: 'Transactions of the address' }).textContent).toContain('-25.00');
    expect(calls.find((c) => c.method === 'wallet.address')?.params).toMatchObject({ wallet: 'w', address: MINE, skip: 0, count: 10 });
    await fireEvent.click(within(view).getByRole('button', { name: /Back/ }));
    await waitFor(() => screen.getByRole('region', { name: 'The peg in this block' }));
    expect(screen.queryByRole('region', { name: 'Address' })).toBeNull();
  });

  it('a withdrawal links to its transaction, its refund address to its view, its script on the mainchain copied', async () => {
    const pegView = await openBlock30();
    const list = within(pegView).getByRole('list', { name: 'Withdrawals' });
    expect(list.textContent).toContain('0.99999');
    await fireEvent.click(within(list).getByRole('button', { name: /^adadadad/ }));
    const tx = await waitFor(() => screen.getByRole('region', { name: 'Transaction' }));
    expect(viewRead()).toEqual([WITHDRAW, 2, hashOf(30)]);
    expect(tx.textContent).toContain(WITHDRAW.slice(0, 12));
  });

  it('goes block → transaction → address → transaction, with the coins spent and their spenders linked', async () => {
    await openBlock30();
    await fireEvent.click(screen.getByRole('button', { name: new RegExp(`^0 ${CB}`) }));
    const tx = await waitFor(() => screen.getByRole('region', { name: 'Transaction' }));
    expect(viewRead()).toEqual([CB, 2, hashOf(30)]);
    await waitFor(() => expect(tx.textContent).toContain('spent by'));
    expect(within(tx).getAllByRole('button').some((b) => b.textContent?.includes(hashOf(30)))).toBe(true);
    await fireEvent.click(within(tx).getByRole('button', { name: OTHER }));
    const view = await waitFor(() => screen.getByRole('region', { name: 'Address' }));
    await waitFor(() => expect(view.textContent).toContain('Not an address of this wallet. The explorer keeps no record of other addresses.'));
    expect(within(view).queryByRole('table')).toBeNull();
    await fireEvent.click(within(view).getByRole('button', { name: /Back/ }));
    const again = await waitFor(() => screen.getByRole('region', { name: 'Transaction' }));
    calls = [];
    await fireEvent.click(within(again).getByRole('button', { name: `${PREV}:1` }));
    await waitFor(() => expect(calls.some((c) => c.method === 'getrawtransaction' && (c.params as unknown[])[0] === PREV)).toBe(true));
    expect(calls.find((c) => c.method === 'getblockhash')?.params).toEqual([12]);
    expect(viewRead()).toEqual([PREV, 2, hashOf(12)]);
  });

  it('searches an address; Simple mode shows its balance and transactions only', async () => {
    prefs.uiMode = 'simple';
    render(Explorer);
    const box = screen.getByRole('searchbox', { name: 'Search' });
    await fireEvent.input(box, { target: { value: ` ${MINE} ` } });
    await fireEvent.submit(box.closest('form')!);
    const view = await waitFor(() => screen.getByRole('region', { name: 'Address' }));
    await waitFor(() => expect(within(view).getByRole('table', { name: 'Transactions of the address' })).toBeTruthy());
    expect(within(view).queryByRole('table', { name: 'Coins of the address' })).toBeNull();
    expect(view.textContent).toContain('Holds now');
    expect(calls.find((c) => c.method === 'validateaddress')?.params).toEqual([MINE]);
    prefs.uiMode = 'advanced';
  });

  it('a transaction of a block from the detail panel opens with its block (no -txindex needed)', async () => {
    render(Explorer);
    nav.tab = `tx:${CB}:${hashOf(30)}`;
    await waitFor(() => screen.getByRole('region', { name: 'Transaction' }));
    expect(viewRead()).toEqual([CB, 2, hashOf(30)]);
  });

  it('asks nothing without a wallet; kinds of addresses in words; deposits matched once each', async () => {
    wallets.current = null;
    expect(ownerOf(MINE)).toBeNull();
    await Promise.resolve();
    await Promise.resolve();
    expect(calls.some((c) => c.method === 'wallet.owned')).toBe(false);
    expect(scriptKindText({ iswitness: true, witness_version: 1 })).toBe('Taproot (P2TR)');
    expect(scriptKindText({ isscript: true })).toBe('Script (P2SH)');
    expect(mayBeAddress(MINE)).toBe(true);
    expect(mayBeAddress('not an address')).toBe(false);
    expect(depositTarget(`s1_${MINE}_abcdef`)).toBe(MINE);
    expect(depositTarget(MINE)).toBe(MINE);
    const d = { txid: 'x', destination: MINE, sats: 5n };
    const m = matchDeposits([[1, 5, MINE], [2, 5, MINE], [3, 6, MINE]], [d, { ...d, txid: 'y' }]);
    expect([...m.entries()]).toEqual([[1, 'x'], [2, 'y']]);
  });

  it('keeps the answers: the same addresses are not asked again; another wallet asks anew', async () => {
    expect(ownerOf(MINE)).toBeNull();
    expect(ownerOf(OTHER)).toBeNull();
    await waitFor(() => expect(ownerOf(MINE)).toEqual({ label: 'Savings' }));
    expect(ownerOf(OTHER)).toBeNull();
    expect(calls.filter((c) => c.method === 'wallet.owned').length).toBe(1);
    wallets.current = 'other';
    expect(ownerOf(MINE)).toBeNull();
    await waitFor(() => expect(calls.filter((c) => c.method === 'wallet.owned').length).toBe(2));
  });
});
