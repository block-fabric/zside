// Many transactions of the wallet at once (user feedback 2026-10-19, item 10: a script made a thousand assets and the
// UI skipped frames): the pages read the wallet again once per burst, not once per transaction. And a chain that hides
// some transactions (an ignored asset's) folds them into one line of the Transactions page.
import { waitFor } from '@testing-library/svelte';
import { afterEach, beforeAll, describe, expect, it, vi } from 'vitest';
import { dispatch } from '../lib/bridge';
import { BURST_MS, subscribe, wallets } from '../lib/state.svelte';

vi.mock('../chain/txinfo.svelte', async (importOriginal) => {
  const real = await importOriginal<Record<string, unknown>>();
  return { ...real, chainTxHidden: (txid: string) => txid.startsWith('dd') };
});

beforeAll(() => subscribe());
afterEach(() => {
  wallets.list = [];
  wallets.current = null;
});

describe('a burst of transactions of the wallet', () => {
  it('is one change for the pages, made within a moment', async () => {
    wallets.current = 'w';
    const before = wallets.txVersion;
    const t0 = Date.now();
    for (let i = 0; i < 500; i++) dispatch([{ type: 'wallet.tx', data: { name: 'w', txid: i.toString(16).padStart(64, '0'), change: 'new' } }]);
    // The notices still count every one.
    expect(wallets.newTxs.length).toBeGreaterThan(0);
    expect(wallets.txVersion).toBe(before);
    await waitFor(() => expect(wallets.txVersion).toBe(before + 1));
    expect(Date.now() - t0).toBeGreaterThanOrEqual(BURST_MS - 20);
    // Another later is another change.
    dispatch([{ type: 'wallet.tx', data: { name: 'w', txid: 'ff'.repeat(32), change: 'new' } }]);
    await waitFor(() => expect(wallets.txVersion).toBe(before + 2));
    // Another wallet's change no change of the one shown.
    dispatch([{ type: 'wallet.tx', data: { name: 'other', txid: 'ee'.repeat(32), change: 'new' } }]);
    await new Promise((r) => setTimeout(r, BURST_MS + 50));
    expect(wallets.txVersion).toBe(before + 2);
  });

  it('changes the addresses once too', async () => {
    wallets.current = 'w';
    const before = wallets.addressVersion;
    for (let i = 0; i < 100; i++) dispatch([{ type: 'wallet.addressbook', data: { name: 'w' } }]);
    await waitFor(() => expect(wallets.addressVersion).toBe(before + 1));
  });
});

describe('the transactions a chain hides', () => {
  it('are folded into one line of the Transactions page, which shows them', async () => {
    const { chainTxHidden } = await import('../lib/chaintx');
    expect(chainTxHidden('dd'.repeat(32))).toBe(true);
    expect(chainTxHidden('aa'.repeat(32))).toBe(false);
    const src = (await import('../pages/Transactions.svelte?raw')).default as string;
    expect(src).toMatch(/hiddenCount && !showHidden \? merged\.filter\(\(r\) => !chainTxHidden\(r\.txid\)\)/);
    expect(src).toMatch(/data-testid="hidden-transactions"/);
    const notifier = (await import('../components/TxNotifier.svelte?raw')).default as string;
    expect(notifier).toMatch(/if \(chainTx && chainTxHidden\(txid\)\) continue;/);
  });
});
