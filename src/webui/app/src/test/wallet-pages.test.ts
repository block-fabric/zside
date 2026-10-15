import { CHAIN } from '../chain/config';
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import AmountInputHarness from './AmountInputHarness.svelte';
import CoinSummary from '../components/CoinSummary.svelte';
import DialogHost from '../components/DialogHost.svelte';
import TxStatusIcon from '../components/TxStatusIcon.svelte';
import Send from '../pages/Send.svelte';
import Transactions from '../pages/Transactions.svelte';
import { isSingleKeyAddress } from '../lib/address';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { ask, askPassphrase, dialogs } from '../lib/dialogs.svelte';
import { nav } from '../lib/nav.svelte';
import { app, prefs, wallets } from '../lib/state.svelte';
import { resetWalletData } from '../lib/walletdata.svelte';
import { regtestP2tr, regtestP2wpkh } from './bech32';

beforeAll(() => {
  // jsdom has no modal dialogs.
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
  prefs.feeMinimized = false;
  prefs.feeMode = 'smart';
  prefs.coinControl = false;
  prefs.psbtControls = false;
});

afterEach(() => {
  setTransport(null);
  dialogs.queue.splice(0);
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
});

const INFO = {
  name: 'w',
  encrypted: false,
  locked: false,
  privateKeysDisabled: false,
  externalSigner: false,
  hd: true,
  canGetAddresses: true,
  taproot: true,
  defaultAddressType: 'bech32',
  confirmTarget: 6,
  requiredFeePerK: 1000,
  maxTxFee: 10_000_000,
  multiwallet: false,
  balance: 5_000_000_000,
  unconfirmed: 0,
  immature: 0,
};

function useWallet() {
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
  wallets.current = 'w';
  wallets.balances.w = { trusted: 5_000_000_000n, untrustedPending: 0n, immature: 0n };
}

describe('addresses for messages', () => {
  it('are one key: P2PKH or P2WPKH', () => {
    expect(isSingleKeyAddress(regtestP2wpkh, 'regtest')).toBe(true);
    expect(isSingleKeyAddress(regtestP2tr, 'regtest')).toBe(false);
    expect(isSingleKeyAddress('mfWxJ45yp2SFn7UciZyNpvDKrzbhyfKrY8', 'regtest')).toBe(true);
    expect(isSingleKeyAddress('2MsFDzHRUAMpjHxKyoEHU3aMCMsVtMqs1PV', 'regtest')).toBe(false);
    expect(isSingleKeyAddress(regtestP2wpkh, 'main')).toBe(false);
  });
});

describe('wallet components', () => {
  it('shows the status of a transaction as a ring', () => {
    const { container } = render(TxStatusIcon, { record: { status: 'confirming', depth: 3, maturesIn: 0 } });
    expect(container.querySelector('svg')?.getAttribute('aria-label')).toBe('Confirming (3 of 6 recommended confirmations)');
    expect(container.querySelector('.fill')?.getAttribute('stroke-dasharray')).toMatch(/^[\d.]+ [\d.]+$/);
  });

  it('shows the coin control estimate', () => {
    const { container } = render(CoinSummary, {
      summary: { quantity: 2, amount: 300_000_000n, fee: 2_000n, afterFee: 299_998_000n, bytes: 211, change: -5n, changeEnabled: true, insufficient: true, feeVary: 9.5, spent: [] },
    });
    expect(container.textContent).toContain('Quantity2');
    expect(container.textContent).toContain('≈211');
    expect(container.textContent).toContain('3.00000000');
    expect(container.textContent).toContain('(insufficient funds)');
    render(CoinSummary, { summary: null });
    expect(screen.getByText('Coins are chosen automatically.')).toBeTruthy();
  });

  it('shows an amount set from outside the input (use available balance)', async () => {
    const { component } = render(AmountInputHarness);
    const input = screen.getByLabelText('Amount') as HTMLInputElement;
    await fireEvent.input(input, { target: { value: '1.5' } });
    await waitFor(() => expect(screen.getByTestId('value').textContent).toBe('150000000'));
    (component as unknown as { set: (v: bigint | null) => void }).set(42n);
    await waitFor(() => expect(input.value).toBe('0.00000042'));
    await fireEvent.input(input, { target: { value: '1,2' } });
    await waitFor(() => expect(screen.getByTestId('value').textContent).toBe('120000000'));
  });
});

describe('dialogs a page waits for', () => {
  it('asks for a new passphrase twice, and says when they differ', async () => {
    render(DialogHost);
    const pending = askPassphrase('encrypt', { wallet: 'w' });
    await waitFor(() => screen.getByLabelText('New passphrase'));
    expect(screen.queryByLabelText('Passphrase')).toBeNull();
    const ok = screen.getByRole('button', { name: 'OK' }) as HTMLButtonElement;
    expect(ok.disabled).toBe(true);
    await fireEvent.input(screen.getByLabelText('New passphrase'), { target: { value: 'one two three' } });
    await fireEvent.input(screen.getByLabelText('Repeat new passphrase'), { target: { value: 'one two thre' } });
    await fireEvent.submit(document.getElementById('passphrase-form')!);
    await waitFor(() => screen.getByText('The supplied passphrases do not match.'));
    await fireEvent.input(screen.getByLabelText('Repeat new passphrase'), { target: { value: 'one two three' } });
    // Caps lock is noticed as keys are typed.
    await fireEvent.keyDown(screen.getByLabelText('Repeat new passphrase'), { key: 'A', getModifierState: () => true });
    await fireEvent.submit(document.getElementById('passphrase-form')!);
    await expect(pending).resolves.toEqual({ passphrase: 'one two three', next: 'one two three' });
  });

  it('waits before the send button of a confirmation can be pressed', async () => {
    render(DialogHost);
    const pending = ask({ title: 'Confirm send coins', message: 'Do you want to create this transaction?', choices: [{ id: 'cancel', label: 'Cancel' }, { id: 'send', label: 'Send', kind: 'primary', delay: 1 }] });
    const send = await waitFor(() => screen.getByRole('button', { name: /^Send/ }) as HTMLButtonElement);
    expect(send.disabled).toBe(true);
    expect(send.textContent).toContain('(1)');
    await waitFor(() => expect((screen.getByRole('button', { name: /^Send/ }) as HTMLButtonElement).disabled).toBe(false), { timeout: 2500 });
    await fireEvent.click(screen.getByRole('button', { name: /^Send/ }));
    await expect(pending).resolves.toEqual({ choice: 'send', checked: false });
  });
});

describe('the send page', () => {
  it('prepares, confirms and sends, with the passphrase of a locked wallet', async () => {
    useWallet();
    const calls: BridgeRequest[] = [];
    setTransport(async (r) => {
      calls.push(r);
      const p = (r.params ?? {}) as Record<string, unknown>;
      switch (r.method) {
        case 'wallet.info':
          return INFO;
        case 'wallet.fee':
          return { feePerK: 2000, fallback: false, target: 6, requiredFeePerK: 1000, reason: 'Fee Rate Estimator' };
        case 'validateaddress':
          return { isvalid: true };
        case 'wallet.send.prepare':
          if (!p.passphrase) throw { code: -13, message: 'This operation needs your wallet passphrase to unlock the wallet.' };
          return { token: 't1', fee: 282, vsize: 141, total: 100_000_282, recipients: [{ address: regtestP2wpkh, label: 'Shop', amount: 100_000_000 }], privateKeysDisabled: false, externalSigner: false, txid: 'ab'.repeat(32) };
        case 'wallet.send.commit':
          return { complete: true, txid: 'ab'.repeat(32) };
        default:
          return null;
      }
    });
    render(Send);
    render(DialogHost);
    await waitFor(() => screen.getByText(/0\.00002000 CHN\/kvB/));
    expect(screen.getByText(/Estimated to begin confirmation within 6 blocks/)).toBeTruthy();
    const payTo = screen.getByLabelText('Pay to');
    await fireEvent.input(payTo, { target: { value: regtestP2wpkh } });
    await fireEvent.input(screen.getByLabelText('Label'), { target: { value: 'Shop' } });
    await fireEvent.input(screen.getByLabelText('Amount'), { target: { value: '1' } });
    await waitFor(() => screen.getByText('Valid address'));
    await fireEvent.click(screen.getByRole('button', { name: /^Send$/ }));
    // Locked: the passphrase first.
    await waitFor(() => screen.getByLabelText('Passphrase'));
    await fireEvent.input(screen.getByLabelText('Passphrase'), { target: { value: 'pw' } });
    await fireEvent.submit(document.getElementById('passphrase-form')!);
    // Then the confirmation, with the fee and the total.
    await waitFor(() => screen.getByText('Do you want to create this transaction?'));
    expect(document.body.textContent).toContain('Transaction fee (0.141 kvB): 0.00000282 CHN');
    expect(document.body.textContent).toContain('Total Amount: 1.00000282 CHN');
    expect(document.body.textContent).toContain(`1.00000000 CHN to 'Shop' (${regtestP2wpkh})`);
    const prepare = calls.find((c) => c.method === 'wallet.send.prepare' && (c.params as Record<string, unknown>).passphrase);
    expect(prepare?.params).toMatchObject({ wallet: 'w', confTarget: 6, recipients: [{ address: regtestP2wpkh, label: 'Shop', amount: 100_000_000, subtractFee: false }] });
    await waitFor(() => expect((screen.getByRole('button', { name: /^Send$/, hidden: false }) as HTMLButtonElement).disabled).toBe(false), { timeout: 5000 });
    const confirmSend = [...document.querySelectorAll<HTMLButtonElement>('button[data-choice="send"]')][0];
    await fireEvent.click(confirmSend);
    await waitFor(() => expect(calls.some((c) => c.method === 'wallet.send.commit')).toBe(true));
    await waitFor(() => expect(nav.highlightTx).toBe('ab'.repeat(32)));
    // The form is cleared after sending.
    await waitFor(() => expect((screen.getByLabelText('Pay to') as HTMLInputElement).value).toBe(''));
  }, 15000);

  it('fills a recipient from a payment URI', async () => {
    useWallet();
    setTransport(async (r) => (r.method === 'wallet.info' ? INFO : r.method === 'validateaddress' ? { isvalid: true } : null));
    nav.pendingUri = `${CHAIN.uriScheme}:${regtestP2wpkh}?amount=0.25&label=Coffee&message=Thanks`;
    nav.uriSeq++;
    render(Send);
    await waitFor(() => expect((screen.getByLabelText('Pay to') as HTMLInputElement).value).toBe(regtestP2wpkh));
    expect((screen.getByLabelText('Label') as HTMLInputElement).value).toBe('Coffee');
    expect((screen.getByLabelText('Amount') as HTMLInputElement).value).toBe('0.25');
    expect(screen.getByText('Thanks')).toBeTruthy();
  });
});

describe('the transactions page', () => {
  it('lists, filters, and hides itself while values are masked', async () => {
    useWallet();
    setTransport(async (r) => {
      if (r.method === 'wallet.transactions') {
        return {
          height: 10,
          hash: 'h',
          records: [
            ['aa'.repeat(32), 0, 'recv', regtestP2wpkh, 'salary', 1_700_000_000, 250_000_000, 0, 'confirmed', 7, 0, true, 'k1'],
            ['bb'.repeat(32), 0, 'send', regtestP2tr, '', 1_700_100_000, 0, -100_000_000, 'unconfirmed', 0, 0, false, 'k2'],
          ],
        };
      }
      if (r.method === 'wallet.addresses') return [];
      return null;
    });
    render(Transactions);
    await waitFor(() => screen.getByText('salary'));
    expect(screen.getByText('[-1.00000000]')).toBeTruthy();
    await fireEvent.change(screen.getByLabelText('Type'), { target: { value: 'sent' } });
    await waitFor(() => expect(screen.queryByText('salary')).toBeNull());
    prefs.mask = true;
    await waitFor(() => screen.getByText('The history is hidden while values are masked.'));
  });
});
