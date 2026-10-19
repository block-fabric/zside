// The notices of new transactions (user feedback 2026-10-19): a transaction that pays the wallet itself is
// listed by the node as a send and a receive of (almost) nothing, as the Transactions page merges them
// (mergeByTxid). One notice per transaction, with its net: never a "received 0.00000" beside the send.
import { render, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import TxNotifier from '../components/TxNotifier.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { app, node, prefs, toasts, wallets } from '../lib/state.svelte';
import { noticeRecordOf, parseRecords } from '../lib/wallet';
import { resetWalletData } from '../lib/walletdata.svelte';

const TX = 'ab'.repeat(32);
const OTHER = 'cd'.repeat(32);

/** A record as the host sends it (RecordJson in walletapi.cpp). */
function raw(txid: string, idx: number, type: string, credit: number, debit: number, address = 'rchn1qaddress'): unknown[] {
  return [txid, idx, type, address, '', 1_700_000_000, credit, debit, 'unconfirmed', 0, 0, true, 'key', -1, 'u', false, false, true];
}

let calls: BridgeRequest[] = [];
let records: unknown[][] = [];
beforeEach(() => {
  app.info = { name: 'Chains', version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp/x', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 60 };
  prefs.unit = 'CHN';
  prefs.mask = false;
  node.tip = { height: 130, time: 1_700_000_000, hash: '00'.repeat(32), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false } as (typeof wallets.list)[number]];
  wallets.current = 'w';
  wallets.newTxs = [];
  toasts.list = [];
  calls = [];
  setTransport(async (r) => {
    calls.push(r);
    if (r.method === 'wallet.transactions') {
      const txids = new Set((r.params as { txids?: string[] }).txids ?? []);
      return { height: 130, hash: '00'.repeat(32), maturity: 100, records: records.filter((x) => txids.has(x[0] as string)) };
    }
    return null;
  });
});
afterEach(() => {
  setTransport(null);
  wallets.list = [];
  wallets.current = null;
  wallets.newTxs = [];
  toasts.list = [];
  resetWalletData();
});

function arrive(...txids: string[]) {
  for (const txid of txids) {
    wallets.newTxs.push({ wallet: 'w', txid });
    wallets.newTxTotal++;
  }
}

describe('the notices of new transactions', () => {
  it('merge the halves of a transaction to the wallet itself: one "Sent", with the net, no "received 0"', async () => {
    // An asset sent: CHN out (the fee and what it paid), and an output of the wallet's own of 0 (the asset's change).
    records = [raw(TX, 0, 'send', 0, -12_000, 'rchn1qrecipient'), raw(TX, 1, 'recv', 0, 0, 'rchn1qmine')];
    render(TxNotifier);
    arrive(TX);
    await waitFor(() => expect(toasts.list.length).toBeGreaterThan(0), { timeout: 3000 });
    await new Promise((r) => setTimeout(r, 300));
    expect(toasts.list.map((x) => x.title)).toEqual(['Sent transaction']);
    expect(toasts.list[0].message).toContain('-0.00012');
    const notify = calls.filter((c) => c.method === 'app.notify');
    expect(notify.length).toBe(1);
  });

  it('keep one notice for each of several transactions, and an income as an income', async () => {
    records = [raw(TX, 0, 'send', 0, -12_000), raw(TX, 1, 'recv', 0, 0), raw(OTHER, 0, 'recv', 50_000, 0)];
    render(TxNotifier);
    arrive(TX, OTHER);
    await waitFor(() => expect(toasts.list.length).toBe(2), { timeout: 3000 });
    await new Promise((r) => setTimeout(r, 300));
    expect(toasts.list.map((x) => x.title).sort()).toEqual(['Incoming transaction', 'Sent transaction']);
  });

  it('call a move of the wallet to itself (net of only the fee) a send, never an income', () => {
    const recs = parseRecords([raw(TX, 0, 'send', 0, -500_000), raw(TX, 1, 'recv', 499_000, 0)]);
    const r = noticeRecordOf(recs, TX)!;
    expect(r.amount).toBe(-1_000n);
    expect(r.type).toBe('send');
    expect(noticeRecordOf(recs, OTHER)).toBeUndefined();
  });
});
