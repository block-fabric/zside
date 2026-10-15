import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import DialogHost from '../components/DialogHost.svelte';
import Proof from '../pages/Proof.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { dialogs } from '../lib/dialogs.svelte';
import { nav } from '../lib/nav.svelte';
import { app, prefs, wallets } from '../lib/state.svelte';
import { checkProofCoins, formatProof, parseProof, PROOF_BEGIN, PROOF_END, PROOF_SIGNATURES, proofMessage } from '../lib/tools';
import { resetWalletData } from '../lib/walletdata.svelte';
import { regtestP2tr, regtestP2wpkh } from './bech32';

// Proof of funds with a choice of what to prove (user decision, 2026-10-15): by address, or by coin (the
// coins listed in the proof and signed with the statement); proofs of chains-qt unchanged.

const A = regtestP2wpkh;
const LEGACY = 'mfWxJ45yp2SFn7UciZyNpvDKrzbhyfKrY8';
/** A pretend signature (one word, as a real one): of this address and this message. */
const sig = (address: string, message: string) => `sig_${address}_${encodeURIComponent(message)}`;
const coin = (txid: string, vout: number, sats: bigint) => ({ txid, vout, sats });

describe('the format of a proof', () => {
  it('without coins: the proof of chains-qt, signed and parsed as before', () => {
    const text = formatProof('These funds are mine.', [{ address: A, signature: 'SIG' }]);
    expect(text).toBe(`${PROOF_BEGIN}\nThese funds are mine.\n${PROOF_SIGNATURES}\n${A} SIG\n${PROOF_END}\n`);
    const parsed = parseProof(text)!;
    expect(parsed.statement).toBe('These funds are mine.');
    expect(parsed.message).toBe('These funds are mine.');
    expect(parsed.coins).toEqual([]);
    expect(parsed.entries).toEqual([{ address: A, signature: 'SIG' }]);
  });

  it('with coins: listed after the statement, and signed with it', () => {
    const coins = [coin('aa'.repeat(32), 0, 100_000_000n), coin('bb'.repeat(32), 3, 12_345n)];
    expect(proofMessage('Mine.', coins)).toBe(`Mine.\n${'aa'.repeat(32)}:0 1.00000000\n${'bb'.repeat(32)}:3 0.00012345`);
    const text = formatProof('Mine.', [{ address: A, signature: 'SIG' }], coins);
    expect(text).toContain(`Mine.\n-----COINS-----\n${'aa'.repeat(32)}:0 1.00000000\n`);
    const parsed = parseProof(text)!;
    expect(parsed.coins).toEqual(coins);
    expect(parsed.message).toBe(proofMessage('Mine.', coins));
    // A coin line that is not one: not a proof.
    expect(parseProof(text.replace(':3 ', ':x '))).toBeNull();
    expect(parseProof(text.replace('-----COINS-----\n', ''))).toBeNull();
  });

  it('the coins against the chain now: unspent, on a signed address, of the amount listed', () => {
    const coins = [coin('aa'.repeat(32), 0, 100_000_000n), coin('bb'.repeat(32), 1, 5n), coin('cc'.repeat(32), 2, 7n), coin('dd'.repeat(32), 3, 9n)];
    const check = checkProofCoins(
      coins,
      [{ value: 1, scriptPubKey: { address: A } }, null, { value: 0.00000007, scriptPubKey: { address: LEGACY } }, { value: 0.00000008, scriptPubKey: { address: A } }],
      new Set([A]),
    );
    expect(check.good).toEqual([coins[0]]);
    expect(check.total).toBe(100_000_000n);
    expect(check.spent).toEqual([coins[1]]);
    expect(check.unsigned).toEqual([{ ...coins[2], address: LEGACY }]);
    expect(check.wrongAmount).toEqual([{ ...coins[3], now: 8n }]);
  });
});

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
  app.info = { name: 'Chains', version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 600 };
  prefs.unit = 'CHN';
  prefs.mask = false;
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
  wallets.current = 'w';
});

afterEach(() => {
  setTransport(null);
  dialogs.queue.splice(0);
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
});

const COINS = [
  {
    address: A,
    label: 'Savings',
    coins: [
      { txid: 'aa'.repeat(32), vout: 0, address: A, label: 'Savings', change: false, sats: 100_000_000, time: 1_700_000_000, depth: 5, locked: false },
      { txid: 'bb'.repeat(32), vout: 1, address: A, label: 'Savings', change: false, sats: 20_000_000, time: 1_700_000_100, depth: 2, locked: true },
    ],
  },
  {
    address: regtestP2tr,
    label: '',
    coins: [{ txid: 'cc'.repeat(32), vout: 0, address: regtestP2tr, label: '', change: false, sats: 300_000_000, time: 1_700_000_200, depth: 9, locked: false }],
  },
  {
    address: LEGACY,
    label: 'Old',
    coins: [{ txid: 'dd'.repeat(32), vout: 4, address: LEGACY, label: 'Old', change: false, sats: 50_000_000, time: 1_700_000_300, depth: 1, locked: false }],
  },
];

function transport(calls: BridgeRequest[], chain: Record<string, { value: number; scriptPubKey: { address: string } } | null> = {}) {
  return async (r: BridgeRequest) => {
    calls.push(r);
    const p = (r.params ?? {}) as Record<string, unknown>;
    switch (r.method) {
      case 'wallet.coins':
        return COINS;
      case 'wallet.withKeys':
        // Only single-key addresses sign (not the taproot one); the signature names what was signed.
        return (p.calls as { params: [string, string] }[]).map(({ params: [address, message] }) =>
          address === regtestP2tr ? { error: { code: -3, message: 'Address does not refer to key' } } : { result: sig(address, message) },
        );
      case 'listunspent':
        return COINS.flatMap((g) => g.coins.map((c) => ({ address: c.address })));
      case 'verifymessage': {
        const [address, signature, message] = r.params as string[];
        return signature === sig(address, message);
      }
      case 'scantxoutset':
        return { height: 200, total_amount: 1.5, unspents: [{}, {}] };
      case 'gettxout': {
        const [txid, vout] = r.params as [string, number];
        return chain[`${txid}:${vout}`] ?? null;
      }
      default:
        return null;
    }
  };
}

async function proofOf(calls: BridgeRequest[]): Promise<string> {
  await waitFor(() => expect((document.getElementById('proof-output') as HTMLTextAreaElement).value).toContain('-----END'));
  void calls;
  return (document.getElementById('proof-output') as HTMLTextAreaElement).value;
}

describe('the Proof of funds page', () => {
  it('by address: the chosen addresses sign, the total is shown first', async () => {
    const calls: BridgeRequest[] = [];
    setTransport(transport(calls));
    nav.page = 'proof';
    nav.tab = '';
    render(Proof);
    render(DialogHost);
    await fireEvent.click(screen.getByRole('switch', { name: 'Choose what to prove' }));
    const list = await screen.findByRole('list', { name: 'Addresses holding coins' });
    await waitFor(() => expect(within(list).getAllByRole('checkbox').length).toBe(3));
    // Search, then choose the legacy one.
    await fireEvent.input(screen.getByLabelText('Search the addresses'), { target: { value: 'old' } });
    await waitFor(() => expect(within(list).getAllByRole('checkbox').length).toBe(1));
    await fireEvent.click(within(list).getByRole('checkbox'));
    await fireEvent.input(screen.getByLabelText('Search the addresses'), { target: { value: '' } });
    await waitFor(() => expect(document.querySelector('.plan')!.textContent).toContain('1 address(es)'));
    expect(document.querySelector('.plan')!.textContent).toContain('0.50000000');
    // Select all, then none, then the savings address alone.
    await fireEvent.click(screen.getByRole('button', { name: 'Select all' }));
    await waitFor(() => expect(document.querySelector('.plan')!.textContent).toContain('3 address(es)'));
    await fireEvent.click(screen.getByRole('button', { name: 'Select none' }));
    await fireEvent.click(within(list).getByRole('checkbox', { name: new RegExp(A) }));
    await waitFor(() => expect(document.querySelector('.plan')!.textContent).toContain('1.20000000'));
    await fireEvent.click(screen.getByRole('button', { name: 'Create proof' }));
    const text = await proofOf(calls);
    const parsed = parseProof(text)!;
    expect(parsed.coins).toEqual([]);
    expect(parsed.entries).toEqual([{ address: A, signature: sig(A, parsed.statement) }]);
  });

  it('by coin: the coins listed and signed; a coin on an address that cannot sign is left out, with a warning', async () => {
    const calls: BridgeRequest[] = [];
    setTransport(transport(calls));
    nav.page = 'proof';
    nav.tab = '';
    render(Proof);
    render(DialogHost);
    await fireEvent.click(screen.getByRole('switch', { name: 'Choose what to prove' }));
    await fireEvent.click(screen.getByRole('tab', { name: 'By coin' }));
    const grid = await screen.findByRole('grid', { name: 'Coins to prove' });
    // A locked coin can be proven.
    await waitFor(() => expect(within(grid).getAllByRole('checkbox').length).toBe(4));
    await fireEvent.click(screen.getByRole('button', { name: 'Select all' }));
    await waitFor(() => expect(document.querySelector('.plan')!.textContent).toContain('4 coin(s), on 3 address(es)'));
    expect(document.querySelector('.plan')!.textContent).toContain('4.70000000');
    await fireEvent.click(screen.getByRole('button', { name: 'Create proof' }));
    const text = await proofOf(calls);
    const parsed = parseProof(text)!;
    // The taproot coin is left out; the others listed, in order, signed with the statement.
    expect(parsed.coins.map((c) => `${c.txid.slice(0, 2)}:${c.vout}`)).toEqual(['aa:0', 'bb:1', 'dd:4']);
    expect(parsed.entries.map((e) => ('address' in e ? e.address : ''))).toEqual([A, LEGACY].sort());
    for (const e of parsed.entries) if ('address' in e) expect(e.signature).toBe(sig(e.address, parsed.message));
    await waitFor(() => expect(document.body.textContent).toContain('1 chosen coin(s) are left out of the proof'));
  });

  it('the check: valid coins, a spent one reported, a coin on an address that did not sign rejected; old proofs as before', async () => {
    const coins = [coin('aa'.repeat(32), 0, 100_000_000n), coin('bb'.repeat(32), 1, 20_000_000n), coin('cc'.repeat(32), 0, 300_000_000n)];
    const message = proofMessage('Mine.', coins);
    const text = formatProof('Mine.', [{ address: A, signature: sig(A, message) }], coins);
    const calls: BridgeRequest[] = [];
    setTransport(
      transport(calls, {
        [`${'aa'.repeat(32)}:0`]: { value: 1, scriptPubKey: { address: A } },
        [`${'cc'.repeat(32)}:0`]: { value: 3, scriptPubKey: { address: regtestP2tr } },
      }),
    );
    nav.page = 'proof';
    nav.tab = 'verify';
    render(Proof);
    await fireEvent.input(screen.getByLabelText(/Paste a proof/), { target: { value: text } });
    await fireEvent.click(screen.getByRole('button', { name: 'Check proof' }));
    const verdict = await waitFor(() => {
      const v = document.getElementById('proof-verdict');
      if (!v || !v.textContent!.includes('Coins listed')) throw new Error('not yet');
      return v;
    });
    expect(verdict.textContent).toContain('Coins listed in the proof: 3. Unspent now, on addresses that signed: 1, holding 1.00000000 CHN.');
    expect(verdict.textContent).toContain(`SPENT or not on the chain: ${'bb'.repeat(32)}:1`);
    expect(verdict.textContent).toContain(`NOT ON A SIGNED ADDRESS: ${'cc'.repeat(32)}:0`);
    expect(verdict.classList.contains('partly')).toBe(true);
    expect(calls.filter((c) => c.method === 'gettxout').map((c) => c.params)).toEqual(coins.map((c) => [c.txid, c.vout, false]));

    // A proof of chains-qt: the statement alone is checked, no coin is looked up.
    calls.length = 0;
    const old = formatProof('Mine.', [{ address: A, signature: sig(A, 'Mine.') }]);
    await fireEvent.input(screen.getByLabelText(/Paste a proof/), { target: { value: old } });
    await fireEvent.click(screen.getByRole('button', { name: 'Check proof' }));
    await waitFor(() => expect(document.getElementById('proof-verdict')!.classList.contains('valid')).toBe(true));
    expect(document.getElementById('proof-verdict')!.textContent).toContain('VALID. The holder of 1 address(es)');
    expect(document.getElementById('proof-verdict')!.textContent).not.toContain('Coins listed');
    expect(calls.some((c) => c.method === 'gettxout')).toBe(false);
  });
});

// A coin listed twice must not be counted twice (security review of f96baf9f74).
describe('a coin listed twice', () => {
  const txid = 'ab'.repeat(32);
  const coin = { txid, vout: 0, sats: 100_000_000n };
  it('a proof listing the same coin twice is not a proof', () => {
    const text = formatProof('I hold this', [{ address: 'addr', signature: 'sig' }], [coin, coin]);
    expect(parseProof(text)).toBeNull();
    expect(parseProof(formatProof('I hold this', [{ address: 'addr', signature: 'sig' }], [coin]))?.coins.length).toBe(1);
  });
  it('the check counts each coin once whatever lists it again', () => {
    const now = { value: 1, scriptPubKey: { address: 'addr' } };
    const check = checkProofCoins([coin, coin, coin], [now, now, now], new Set(['addr']));
    expect(check.good.length).toBe(1);
    expect(check.total).toBe(100_000_000n);
  });
});
