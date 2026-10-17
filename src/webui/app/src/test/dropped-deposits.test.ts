// A deposit to a sidechain the mempool dropped (audit 7-A #1): the history says so ("Dropped — not sent", never counted),
// the Sidechains page lists it with "Make it again", and the deposit made again names the one it replaces; the desktop
// confirms it (rpc.confirmed), never the page alone.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { afterEach, beforeAll, describe, expect, it } from 'vitest';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { depositAgain, makeDepositAgain } from '../lib/deposits.svelte';
import { nav } from '../lib/nav.svelte';
import { parseRecords, statusText, withStatus } from '../lib/wallet';

beforeAll(() => {
  const proto = HTMLDialogElement.prototype as unknown as Record<string, unknown>;
  proto.showModal ??= function (this: HTMLDialogElement) {
    this.setAttribute('open', '');
  };
  proto.close ??= function (this: HTMLDialogElement) {
    this.removeAttribute('open');
  };
});

afterEach(() => setTransport(null));

const TXID = 'dd'.repeat(32);
// A record as the host sends it (RecordJson): sent, 1.5 CHN, the state "d" of a dropped deposit.
const RAW = [TXID, 0, 'sendOther', 's2_x_y', '', 1_700_000_000, 0, -150_001_000, 'dropped', 0, 0, false, 'k', -1, 'd', false, false, false];

describe('a dropped deposit', () => {
  it('is "Dropped — not sent" in the history, and never counted, whatever the blocks', () => {
    const [r] = parseRecords([RAW]);
    expect(r.state).toBe('d');
    expect(r.status).toBe('dropped');
    expect(statusText(r)).toBe('Dropped — not sent');
    const later = withStatus(r, 500, 101);
    expect(later.status).toBe('dropped');
    expect(later.countsForBalance).toBe(false);
  });

  it('is made again with what the host read of it, through the confirmation of the desktop', async () => {
    const calls: BridgeRequest[] = [];
    setTransport(async (r) => {
      calls.push(r);
      if (r.method === 'wallet.deposit.info') return { txid: TXID, slot: 2, destination: 's2_x_y', amount: 150_000_000, dropped: true };
      return null;
    });
    await makeDepositAgain('w', TXID);
    expect(calls[0]).toMatchObject({ kind: 'host', method: 'wallet.deposit.info', params: { wallet: 'w', txid: TXID } });
    expect(depositAgain.next).toEqual({ replaces: TXID, slot: 2, destination: 's2_x_y', amount: 150_000_000n });
    expect(nav.page).toBe('sidechains');
    expect(nav.focus).toEqual({ page: 'sidechains', key: '2' });
    depositAgain.next = null;
  });

  it('the deposit made again names the one it replaces (the host shows it and checks it)', () => {
    // The form passes "replaces" as the fourth parameter of createsidechaindeposit, which the host shows (confirm.cpp).
    const page = readFileSync(join(__dirname, '../pages/Sidechains.svelte'), 'utf8');
    expect(page).toMatch(/'createsidechaindeposit', replaces \? \[selected\.slot, dest, satsToCoins\(sats\), replaces\]/);
    expect(page).toMatch(/Make it again/);
    const confirm = readFileSync(join(__dirname, '../../../confirm.cpp'), 'utf8');
    expect(confirm).toMatch(/"Makes again"/);
  });
});
