import { afterEach, describe, expect, it } from 'vitest';
import { setTransport, type BridgeRequest } from './bridge';
import { answer, askPassphrase, dialogs, withUnlock } from './dialogs.svelte';
import { base64ToBytes, bytesToBase64, safeFileName } from './files';
import {
  addressText,
  amountText,
  balanceHistory,
  confirmationProgress,
  dateRange,
  filterRecords,
  isoLocal,
  nearestConfTarget,
  parseRecords,
  plainText,
  psbtFileName,
  statusText,
  toCsv,
  csvCell,
  transactionsCsv,
  typeText,
  walletApi,
  walletDisplayName,
  WalletError,
} from './wallet';

afterEach(() => {
  setTransport(null);
  dialogs.queue.splice(0);
});

// Records as the host sends them (RecordJson in walletapi.cpp).
const raw: unknown[][] = [
  ['aa'.repeat(32), 0, 'mined', 'rchn1qminer', 'mining', 1_700_000_000, 5_000_000_000, 0, 'confirmed', 120, 0, true, 'k1'],
  ['bb'.repeat(32), 1, 'send', 'rchn1qbob', '', 1_700_100_000, 0, -1_000_010_000, 'confirming', 2, 0, true, 'k2'],
  ['cc'.repeat(32), 0, 'recv', 'rchn1qme', 'salary', 1_700_200_000, 250_000_000, 0, 'unconfirmed', 0, 0, false, 'k3'],
  ['dd'.repeat(32), 0, 'mined', 'rchn1qminer', 'mining', 1_700_300_000, 5_000_000_000, 0, 'immature', 3, 98, false, 'k4'],
  ['ee'.repeat(32), 0, 'other', '', '', 1_700_400_000, 0, -50_000, 'conflicted', -1, 0, false, 'k5'],
  ['ff'.repeat(32), 2, 'send', 'rchn1qcarol', 'Carol "C"', 1_700_500_000, 0, -20_000, 'abandoned', 0, 0, false, 'k6'],
];

describe('transaction records', () => {
  const records = parseRecords(raw);

  it('reads the arrays of the host', () => {
    expect(records).toHaveLength(6);
    expect(records[1]).toMatchObject({ txid: 'bb'.repeat(32), idx: 1, type: 'send', debit: -1_000_010_000n, amount: -1_000_010_000n, depth: 2, countsForBalance: true });
    expect(records[0].key).not.toBe(records[3].key);
  });

  it('writes status, type, address and amount as chains-qt', () => {
    expect(statusText(records[0])).toBe('Confirmed (120 confirmations)');
    expect(statusText(records[1])).toBe('Confirming (2 of 6 recommended confirmations)');
    expect(statusText(records[2])).toBe('Unconfirmed');
    expect(statusText(records[3])).toBe('Immature (3 confirmations, will be available after 101)');
    expect(statusText(records[4])).toBe('Conflicted');
    expect(statusText(records[5])).toBe('Abandoned');
    expect(typeText('recv')).toBe('Received with');
    expect(typeText('sendOther')).toBe('Sent to');
    expect(typeText('mined')).toBe('Mined');
    expect(typeText('other')).toBe('');
    expect(addressText(records[0])).toBe('mining');
    expect(addressText(records[0], true)).toBe('mining (rchn1qminer)');
    expect(addressText(records[1])).toBe('rchn1qbob');
    expect(addressText(records[4])).toBe('(n/a)');
    // Amounts that do not count yet are in brackets.
    expect(amountText(records[2], 'CHN')).toBe('[2.50000000]');
    expect(amountText(records[2], 'CHN', false)).toBe('2.50000000');
    expect(amountText(records[1], 'sat')).toBe('-1\u2009000\u2009010\u2009000');
    expect(confirmationProgress(records[1])).toBeCloseTo(2 / 6);
    expect(confirmationProgress(records[3])).toBeCloseTo(3 / 101);
    expect(confirmationProgress(records[0])).toBe(1);
  });

  it('filters by type, date, text and amount (TransactionFilterProxy)', () => {
    const all = { type: 'all' as const, search: '', minAmount: 0n, showInactive: true };
    expect(filterRecords(records, all)).toHaveLength(6);
    expect(filterRecords(records, { ...all, showInactive: false })).toHaveLength(5);
    expect(filterRecords(records, { ...all, type: 'mined' }).map((r) => r.status)).toEqual(['confirmed', 'immature']);
    expect(filterRecords(records, { ...all, type: 'received' })).toHaveLength(1);
    expect(filterRecords(records, { ...all, type: 'sent' })).toHaveLength(2);
    expect(filterRecords(records, { ...all, type: 'other' })).toHaveLength(1);
    expect(filterRecords(records, { ...all, search: 'SALARY' })).toHaveLength(1);
    expect(filterRecords(records, { ...all, search: 'bbbb' })).toHaveLength(1);
    expect(filterRecords(records, { ...all, search: 'qcarol' })).toHaveLength(1);
    expect(filterRecords(records, { ...all, minAmount: 1_000_000_000n }).map((r) => r.type)).toEqual(['mined', 'send', 'mined']);
    expect(filterRecords(records, { ...all, from: 1_700_150_000, to: 1_700_300_000 })).toHaveLength(1);
  });

  it('gives the date ranges of the date filter', () => {
    // Wednesday 2026-10-14, 15:00.
    const now = new Date(2026, 9, 14, 15, 0, 0);
    const secs = (y: number, m: number, d: number) => Math.floor(new Date(y, m, d).getTime() / 1000);
    expect(dateRange('all', now)).toEqual({});
    expect(dateRange('today', now)).toEqual({ from: secs(2026, 9, 14) });
    expect(dateRange('week', now)).toEqual({ from: secs(2026, 9, 12) });
    expect(dateRange('month', now)).toEqual({ from: secs(2026, 9, 1) });
    expect(dateRange('lastMonth', now)).toEqual({ from: secs(2026, 8, 1), to: secs(2026, 9, 1) });
    expect(dateRange('year', now)).toEqual({ from: secs(2026, 0, 1) });
    expect(dateRange('range', now, new Date(2026, 0, 5), new Date(2026, 0, 7))).toEqual({ from: secs(2026, 0, 5), to: secs(2026, 0, 8) });
    // January: last month is December of the year before.
    expect(dateRange('lastMonth', new Date(2026, 0, 3))).toEqual({ from: secs(2025, 11, 1), to: secs(2026, 0, 1) });
  });

  it('exports CSV as CSVModelWriter: every value quoted, quotes doubled', () => {
    expect(toCsv(['a', 'b'], [['x', 'say "hi"']])).toBe('"a","b"\n"x","say ""hi"""\n');
    // Formulas from other people's labels are not run by a spreadsheet; numbers stay numbers (audit port-2 #12).
    expect(toCsv(['Label', 'Amount'], [["=cmd|' /C calc'!A0", '-0.5'], ['@SUM(A1)', '+1'], ['+x', '-1e5'], ['\tx', 'a-b']])).toBe(
      `"Label","Amount"\n"'=cmd|' /C calc'!A0","-0.5"\n"'@SUM(A1)","+1"\n"'+x","-1e5"\n"'\tx","a-b"\n`,
    );
    expect(csvCell("'=cmd")).toBe('=cmd');
    expect(csvCell("'plain")).toBe("'plain");
    const csv = transactionsCsv([records[5], records[1]], 'CHN').split('\n');
    expect(csv[0]).toBe('"Confirmed","Date","Type","Label","Address","Amount (CHN)","ID"');
    expect(csv[1]).toBe(`"false","${isoLocal(1_700_500_000)}","Sent to","Carol ""C""","rchn1qcarol","-0.00020000","${'ff'.repeat(32)}"`);
    expect(csv[2].startsWith('"true",')).toBe(true);
    expect(csv[3]).toBe('');
  });

  it('copies full details as one line', () => {
    const line = plainText(records[2], 'CHN');
    expect(line).toMatch(/^\d+\/\d+\/\d\d \d\d:\d\d Unconfirmed\. Received with \(salary\) rchn1qme 2\.50000000$/);
    expect(plainText(records[1], 'CHN')).toContain('(no label) rchn1qbob -10.00010000');
  });

  it('builds the balance history, without what does not count', () => {
    const h = balanceHistory(records);
    expect(h.map((p) => p.balance)).toEqual([5_000_000_000n, 3_999_990_000n, 4_249_990_000n, 9_249_990_000n]);
    expect(h.map((p) => p.time)).toEqual([1_700_000_000, 1_700_100_000, 1_700_200_000, 1_700_300_000]);
  });
});

describe('send page helpers', () => {
  it('picks the confirmation target as chains-qt', () => {
    expect(nearestConfTarget(1)).toBe(2);
    expect(nearestConfTarget(6)).toBe(6);
    expect(nearestConfTarget(7)).toBe(12);
    expect(nearestConfTarget(5000)).toBe(1008);
  });

  it('names PSBT files and wallets', () => {
    expect(psbtFileName([{ address: 'rchn1qa', label: 'Rent', amount: 150_000_000n }, { address: 'rchn1qb', amount: 1n }], 'CHN')).toBe('Rent-1.50000000 CHN - rchn1qb-0.00000001 CHN.psbt');
    expect(safeFileName('a/b:c*?.psbt')).toBe('a_b_c__.psbt');
    expect(walletDisplayName('')).toBe('[default wallet]');
    expect(walletDisplayName('w')).toBe('w');
  });

  it('moves bytes through base64', () => {
    const bytes = new Uint8Array([0x70, 0x73, 0x62, 0x74, 0xff, 0, 1, 2, 3, 250]);
    const text = bytesToBase64(bytes);
    expect(text).toBe('cHNidP8AAQID+g==');
    expect([...base64ToBytes(text)]).toEqual([...bytes]);
    expect([...base64ToBytes(bytesToBase64(new Uint8Array([1, 2])))]).toEqual([1, 2]);
  });
});

describe('the wallet client', () => {
  it('sends amounts as satoshis and reads them as bigint', async () => {
    const seen: BridgeRequest[] = [];
    setTransport(async (r) => {
      seen.push(r);
      if (r.method === 'wallet.send.prepare') {
        return { token: 't1', fee: 141, vsize: 141, total: 150_000_141, recipients: [{ address: 'rchn1qx', label: 'X', amount: 150_000_000 }], privateKeysDisabled: false, externalSigner: false, txid: 'ab' };
      }
      return null;
    });
    const p = await walletApi('w').prepare([{ address: 'rchn1qx', label: 'X', amount: 150_000_000n, subtractFee: false }], { selected: [{ txid: 'aa', vout: 1 }] }, { confTarget: 6, feeRate: 1000 }, 'pw');
    expect(p.fee).toBe(141n);
    expect(p.recipients[0].amount).toBe(150_000_000n);
    expect(seen[0]).toMatchObject({ kind: 'host', method: 'wallet.send.prepare' });
    expect(seen[0].params).toMatchObject({ wallet: 'w', passphrase: 'pw', confTarget: 6, feeRate: 1000, selected: [{ txid: 'aa', vout: 1 }], recipients: [{ address: 'rchn1qx', amount: 150_000_000 }] });
  });
});

describe('unlocking on demand', () => {
  it('runs once without asking when the wallet is not locked', async () => {
    const tries: (string | undefined)[] = [];
    await expect(withUnlock('w', async (p) => (tries.push(p), 'done'))).resolves.toBe('done');
    expect(tries).toEqual([undefined]);
    expect(dialogs.queue).toHaveLength(0);
    // Other errors go to the caller.
    await expect(withUnlock('w', async () => Promise.reject({ code: -6, message: 'refused' }))).rejects.toMatchObject({ code: -6 });
  });

  it('works with the errors of the bridge', async () => {
    let calls = 0;
    setTransport(async (r) => {
      calls++;
      const pass = (r.params as Record<string, unknown>).passphrase;
      if (!pass) throw { code: -13, message: 'This operation needs your wallet passphrase to unlock the wallet.' };
      if (pass !== 'right') throw { code: -14, message: 'The passphrase entered for the wallet decryption was incorrect.' };
      return { signature: 'sig' };
    });
    const pending = withUnlock('w', (passphrase) => walletApi('w').signMessage('rchn1q', 'hi', passphrase));
    await vi_waitFor(() => dialogs.queue.length === 1);
    expect(dialogs.queue[0]).toMatchObject({ kind: 'passphrase', mode: 'unlock', wallet: 'w' });
    answer(dialogs.queue[0].id, { passphrase: 'wrong' });
    await vi_waitFor(() => dialogs.queue.length === 1 && 'error' in dialogs.queue[0] && !!dialogs.queue[0].error);
    expect(dialogs.queue[0]).toMatchObject({ error: 'The passphrase entered for the wallet decryption was incorrect.' });
    answer(dialogs.queue[0].id, { passphrase: 'right' });
    await expect(pending).resolves.toEqual({ signature: 'sig' });
    expect(calls).toBe(3);
  });

  it('gives null when the user cancels', async () => {
    setTransport(async () => {
      throw { code: -13, message: 'locked' };
    });
    const pending = withUnlock('w', (passphrase) => walletApi('w').signMessage('a', 'b', passphrase));
    await vi_waitFor(() => dialogs.queue.length === 1);
    answer(dialogs.queue[0].id, null);
    await expect(pending).resolves.toBeNull();
  });

  it('asks for passphrases of every kind', async () => {
    const p = askPassphrase('change', { wallet: 'w' });
    expect(dialogs.queue[0]).toMatchObject({ kind: 'passphrase', mode: 'change', title: 'Change passphrase' });
    answer(dialogs.queue[0].id, { passphrase: 'old', next: 'new' });
    await expect(p).resolves.toEqual({ passphrase: 'old', next: 'new' });
  });
});

async function vi_waitFor(fn: () => boolean, ms = 2000): Promise<void> {
  const until = Date.now() + ms;
  while (!fn()) {
    if (Date.now() > until) throw new Error('timed out');
    await new Promise((r) => setTimeout(r, 5));
  }
}

