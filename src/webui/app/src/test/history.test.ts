// The history of a wallet is loaded whole once, then kept up to date in parts (audit H2): new blocks
// and new transactions never send the whole history again.
import { afterEach, describe, expect, it } from 'vitest';
import { dispatch, setTransport, type BridgeRequest } from '../lib/bridge';
import { node, wallets } from '../lib/state.svelte';
import { filterRecords, parseRecords, withStatus, type TxRecord } from '../lib/wallet';
import { current, historyState, mergeRecords, refreshRecords, relabel, resetWalletData } from '../lib/walletdata.svelte';

const MATURITY = 101;

/** A record as the host sends it (RecordJson in walletapi.cpp). */
function raw(txid: string, o: Partial<{ type: string; credit: number; debit: number; status: string; depth: number; maturesIn: number; counts: boolean; height: number; state: string; coinbase: boolean; abandoned: boolean; trusted: boolean; address: string; label: string; time: number }> = {}): unknown[] {
  return [
    txid,
    0,
    o.type ?? 'recv',
    o.address ?? 'rchn1qaddress',
    o.label ?? '',
    o.time ?? 1_700_000_000,
    o.credit ?? 100_000,
    o.debit ?? 0,
    o.status ?? 'confirmed',
    o.depth ?? 1,
    o.maturesIn ?? 0,
    o.counts ?? true,
    'key',
    o.height ?? 10,
    o.state ?? 'c',
    o.coinbase ?? false,
    o.abandoned ?? false,
    o.trusted ?? true,
  ];
}

function record(txid: string, o: Parameters<typeof raw>[1] = {}): TxRecord {
  return parseRecords([raw(txid, o)])[0];
}

afterEach(() => {
  setTransport(null);
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
});

describe('the status of a record, from the height of the wallet (TransactionRecord::updateStatus)', () => {
  it('counts confirmations from the block of the transaction', () => {
    const r = record('a'.repeat(64), { height: 10, state: 'c' });
    expect(withStatus(r, 10, MATURITY)).toMatchObject({ status: 'confirming', depth: 1, countsForBalance: true });
    expect(withStatus(r, 14, MATURITY)).toMatchObject({ status: 'confirming', depth: 5 });
    expect(withStatus(r, 15, MATURITY)).toMatchObject({ status: 'confirmed', depth: 6 });
    // Nothing changed: the same object (the table redraws nothing).
    const confirmed = withStatus(r, 20, MATURITY);
    expect(withStatus(confirmed, 20, MATURITY)).toBe(confirmed);
  });

  it('matures coinbases, and knows the ones that did not make it', () => {
    const mined = record('b'.repeat(64), { type: 'mined', height: 101, state: 'c', coinbase: true });
    expect(withStatus(mined, 101, MATURITY)).toMatchObject({ status: 'immature', depth: 1, maturesIn: 100, countsForBalance: false });
    expect(withStatus(mined, 200, MATURITY)).toMatchObject({ status: 'immature', depth: 100, maturesIn: 1 });
    expect(withStatus(mined, 201, MATURITY)).toMatchObject({ status: 'confirmed', depth: 101, maturesIn: 0, countsForBalance: true });
    const orphan = record('c'.repeat(64), { type: 'mined', height: -1, state: 'u', coinbase: true, trusted: false });
    expect(withStatus(orphan, 300, MATURITY)).toMatchObject({ status: 'notaccepted', depth: 0, countsForBalance: false });
  });

  it('keeps what the host said of a transaction out of a block, and counts conflicts', () => {
    const mempool = record('d'.repeat(64), { height: -1, state: 'u', trusted: false, counts: false, status: 'unconfirmed', depth: 0 });
    expect(withStatus(mempool, 50, MATURITY)).toMatchObject({ status: 'unconfirmed', depth: 0, countsForBalance: false });
    const abandoned = record('e'.repeat(64), { height: -1, state: 'u', abandoned: true, trusted: false });
    expect(withStatus(abandoned, 50, MATURITY).status).toBe('abandoned');
    const conflicted = record('f'.repeat(64), { height: 40, state: 'x', trusted: false });
    expect(withStatus(conflicted, 50, MATURITY)).toMatchObject({ status: 'conflicted', depth: -11, countsForBalance: false });
  });

  it('leaves a record the host gave no state for as it is', () => {
    const old = parseRecords([['a'.repeat(64), 0, 'recv', 'x', '', 1, 5, 0, 'confirmed', 7, 0, true, 'k1']])[0];
    expect(old.state).toBeNull();
    expect(withStatus(old, 100, MATURITY)).toBe(old);
  });
});

describe('the history kept up to date', () => {
  it('asks only for the transactions that changed, and the ones out of a block', async () => {
    const calls: BridgeRequest[] = [];
    let height = 10;
    const all = [raw('a'.repeat(64), { height: 9 }), raw('b'.repeat(64), { height: -1, state: 'u', trusted: true, status: 'unconfirmed', depth: 0 })];
    for (let i = 0; i < 1000; i++) all.push(raw(i.toString(16).padStart(64, '0'), { height: 5 }));
    setTransport(async (r) => {
      calls.push(r);
      const p = r.params as { txids?: string[] };
      if (r.method !== 'wallet.transactions') return null;
      if (!p.txids) return { height, hash: 'h', maturity: MATURITY, records: all };
      return {
        height,
        hash: 'h',
        maturity: MATURITY,
        records: p.txids.filter((t) => t !== 'c'.repeat(64)).map((t) => (t === 'b'.repeat(64) ? raw(t, { height: 11, state: 'c' }) : raw(t, { height: 11 }))),
        gone: [],
      };
    });
    wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
    wallets.current = 'w';
    await refreshRecords();
    expect(current.records.length).toBe(1002);
    expect(historyState()).toMatchObject({ wallet: 'w', height: 10, maturity: MATURITY });
    expect(calls.length).toBe(1);

    // A new block with a new transaction in it.
    height = 11;
    dispatch([{ type: 'wallet.tx', data: { name: 'w', txid: 'f'.repeat(64), change: 'new' } }]);
    node.tip = { height: 11, time: 0, hash: 'tip11', progress: 1, state: 'post_init', ibd: false, loadingBlocks: false } as typeof node.tip;
    await refreshRecords();
    expect(calls.length).toBe(2);
    const asked = (calls[1].params as { txids: string[] }).txids.sort();
    // The new one, and the one that was out of a block: not the thousand others.
    expect(asked).toEqual(['b'.repeat(64), 'f'.repeat(64)]);
    expect(current.records.length).toBe(1003);
    const a = current.records.find((r) => r.txid === 'a'.repeat(64));
    expect(a?.depth).toBe(3);
    expect(current.records.find((r) => r.txid === 'b'.repeat(64))?.status).toBe('confirming');

    // The same tip again: nothing asked.
    await refreshRecords();
    expect(calls.length).toBe(2);
  });

  it('merges by transaction, and shows a new label at once', () => {
    const records = [record('a'.repeat(64), { address: 'A' }), record('b'.repeat(64), { address: 'B' })];
    const merged = mergeRecords(records, [record('b'.repeat(64), { address: 'B', credit: 7 })], new Set(['b'.repeat(64), 'c'.repeat(64)]), 20, MATURITY);
    expect(merged.map((r) => [r.txid[0], Number(r.credit)])).toEqual([
      ['a', 100_000],
      ['b', 7],
    ]);
    const labelled = relabel(merged, 'A', 'rent');
    expect(labelled[0].label).toBe('rent');
    expect(labelled[1]).toBe(merged[1]);
    expect(relabel(labelled, 'nobody', 'x')).toBe(labelled);
  });

  it('searches a long history without making its text again for each key', () => {
    const many = Array.from({ length: 50_000 }, (_, i) => record(i.toString(16).padStart(64, '0'), { label: i % 2 ? 'Salary' : 'rent', address: `rchn1q${i}` }));
    const filter = { type: 'all' as const, search: 'salary', minAmount: 0n, showInactive: true };
    const t0 = performance.now();
    expect(filterRecords(many, filter).length).toBe(25_000);
    const first = performance.now() - t0;
    const t1 = performance.now();
    for (const s of ['s', 'sa', 'sal', 'sala', 'salar']) filterRecords(many, { ...filter, search: s });
    const five = performance.now() - t1;
    // Five more keys cost less than the first search, which made the text.
    expect(five).toBeLessThan(first * 5);
  });
});
