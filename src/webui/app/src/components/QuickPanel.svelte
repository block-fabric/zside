<script lang="ts">
  // Send and Receive at the right of the dashboard's home (lib/quick.svelte.ts): one recipient and
  // a big Send button; a new address with its QR code. The Send and Receive pages stay for the
  // rest (several recipients, coin control, PSBT, the requests made before). Escape closes it.
  import { t } from '../lib/i18n.svelte';
  import SafeText from './SafeText.svelte';
  import AddressInput from './AddressInput.svelte';
  import Amount from './Amount.svelte';
  import AmountInput from './AmountInput.svelte';
  import CopyButton from './CopyButton.svelte';
  import Icon from './Icon.svelte';
  import QrCode from './QrCode.svelte';
  import { errorText, inform, withUnlock } from '../lib/dialogs.svelte';
  import { go } from '../lib/nav.svelte';
  import { closeQuick, quick, quickCanSend, quickFeeChoice, quickSend, type QuickMode } from '../lib/quick.svelte';
  import { prefs, toast, wallets } from '../lib/state.svelte';
  import { formatAmount } from '../lib/units';
  import { formatPaymentUri } from '../lib/uri';
  import { walletApi, walletDisplayName } from '../lib/wallet';
  import { current, refreshInfo } from '../lib/walletdata.svelte';

  let panel: HTMLElement | undefined = $state();
  const wallet = $derived(wallets.current);
  const balance = $derived(wallet !== null ? wallets.balances[wallet]?.trusted : undefined);

  // --- Send ---
  let address = $state('');
  let valid = $state(false);
  let label = $state('');
  let amount: bigint | null = $state(null);
  let unit = $state(prefs.unit);
  let subtractFee = $state(prefs.subtractFeeDefault);
  let busy = $state(false);
  let formKey = $state(0);
  const fee = $derived(quickFeeChoice());

  $effect(() => {
    if (wallet !== null && quick.mode) refreshInfo();
  });

  // Moved focus into the panel when it opens (or changes), for the keyboard.
  $effect(() => {
    if (quick.mode && panel) queueMicrotask(() => panel?.querySelector<HTMLElement>('input, textarea')?.focus());
  });

  async function useAvailable() {
    if (wallet === null) return;
    try {
      const all = await walletApi(wallet).available();
      amount = all > 0n ? all : 0n;
      subtractFee = all > 0n;
    } catch (e) {
      toast('error', t('Use available balance'), errorText(e));
    }
  }

  async function send() {
    if (busy) return;
    if (!valid) {
      document.getElementById('quick-pay-to')?.focus();
      toast('warning', t('Send Coins'), address ? t('The recipient address is not valid. Please recheck.') : t('Enter an address to pay to.'));
      return;
    }
    if (amount === null || amount <= 0n) {
      document.getElementById('quick-amount')?.focus();
      toast('warning', t('Send Coins'), t('The amount to pay must be larger than 0.'));
      return;
    }
    busy = true;
    const txid = await quickSend({ address: address.trim(), label, amount, subtractFee });
    busy = false;
    if (txid) {
      address = '';
      label = '';
      amount = null;
      formKey++;
    }
  }

  // --- Receive ---
  let rLabel = $state('');
  let rAmount: bigint | null = $state(null);
  let made = $state<{ address: string; label: string; amount: bigint } | null>(null);
  const uri = $derived(made ? formatPaymentUri({ address: made.address, amount: made.amount || undefined, label: made.label || undefined }) : '');

  async function newAddress() {
    if (wallet === null || busy) return;
    const api = walletApi(wallet);
    busy = true;
    try {
      const type = current.info?.defaultAddressType || 'bech32';
      const a = await withUnlock(wallet, (passphrase) => api.newAddress(rLabel, type, passphrase));
      if (a === null) {
        await inform(t('Receive'), t('Could not unlock wallet.'), 'error');
        return;
      }
      made = { address: a, label: rLabel, amount: rAmount ?? 0n };
      // Kept with the requests of the Receive page.
      await api.addRequest({ ...made, message: '' });
    } catch (e) {
      await inform(t('Receive'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  function full(page: 'send' | 'receive') {
    closeQuick();
    go(page);
  }

  const MODES: { id: QuickMode; label: string; icon: 'send' | 'receive' }[] = [
    { id: 'send', label: 'Send', icon: 'send' },
    { id: 'receive', label: 'Receive', icon: 'receive' },
  ];
</script>

<svelte:window
  onkeydown={(e) => {
    if (e.key === 'Escape' && quick.mode && !document.querySelector('dialog[open]')) closeQuick();
  }}
/>

{#if quick.mode}
  <aside class="quick-panel" aria-label={quick.mode === 'send' ? 'Send' : 'Receive'} bind:this={panel}>
    <header>
      <div class="modes" role="tablist" aria-label="Send or receive">
        {#each MODES as m (m.id)}
          <button type="button" role="tab" aria-selected={quick.mode === m.id} onclick={() => (quick.mode = m.id)}><Icon name={m.icon} size={16} /> {t(m.label)}</button>
        {/each}
      </div>
      <button type="button" class="btn round" aria-label="Close" title="Close (Esc)" onclick={closeQuick}><Icon name="x" size={16} /></button>
    </header>

    {#if wallet === null}
      <div class="body"><p class="muted">No wallet is loaded.</p><button type="button" class="btn" onclick={() => full('send')}>Wallets</button></div>
    {:else if quick.mode === 'send'}
      <form
        class="body"
        onsubmit={(e) => {
          e.preventDefault();
          send();
        }}
      >
        <section class="box from">
          <div class="line">
            <span class="caps">From</span>
            <span class="pill"><Icon name="wallet" size={14} /> <SafeText text={walletDisplayName(wallet)} /></span>
          </div>
          <div class="line">
            <span class="muted">Available</span>
            <span class="avail"><Amount sats={balance} /> <button type="button" class="max" onclick={useAvailable} title="Use available balance">Max</button></span>
          </div>
          {#key formKey}
            <div class="big-amount"><AmountInput id="quick-amount" label="Amount" bind:value={amount} bind:unit /></div>
          {/key}
          <label class="check small"><input type="checkbox" bind:checked={subtractFee} /> <span>Subtract fee from amount</span></label>
        </section>

        <section class="box to">
          <span class="caps">To</span>
          {#key formKey}
            <AddressInput id="quick-pay-to" label="Pay to" bind:value={address} bind:valid />
          {/key}
          <div class="field">
            <label for="quick-label">Label</label>
            <input id="quick-label" type="text" bind:value={label} placeholder="Optional: kept in the address book" autocomplete="off" />
          </div>
        </section>

        <p class="fee faint">
          <span>Fee: {fee.feeRate ? `${formatAmount(BigInt(fee.feeRate), prefs.unit, { withUnit: true })}/kvB (custom)` : `smart, within ${fee.confTarget} blocks`}</span>
          <button type="button" class="link" onclick={() => full('send')}>Change</button>
        </p>

        {#if quickCanSend()}
          <button type="submit" class="btn cta" disabled={busy}><span>{busy ? 'Preparing…' : 'Send'}</span><Icon name="arrow" size={24} /></button>
        {:else}
          <p class="muted">This wallet does not sign here (watch-only or an external signer): use the Send page.</p>
          <button type="button" class="btn cta" onclick={() => full('send')}><span>Open the Send page</span><Icon name="arrow" size={24} /></button>
        {/if}
        <button type="button" class="btn ghost more" onclick={() => full('send')}>Several recipients, coin control, PSBT: the Send page</button>
      </form>
    {:else}
      <form
        class="body"
        onsubmit={(e) => {
          e.preventDefault();
          newAddress();
        }}
      >
        {#if made}
          <section class="box made" aria-label="New address">
            <div class="qr"><QrCode text={uri} size={196} label="QR code of the payment request" /></div>
            <div class="addr mono break">{made.address}</div>
            <div class="row center">
              <CopyButton text={made.address} label="Copy address" iconOnly={false} />
              <CopyButton text={uri} label="Copy URI" iconOnly={false} />
            </div>
            {#if made.label || made.amount}
              <p class="muted small center"><SafeText text={made.label} />{made.label && made.amount ? ' · ' : ''}{#if made.amount}<Amount sats={made.amount} />{/if}</p>
            {/if}
          </section>
        {/if}
        <section class="box">
          <span class="caps">{made ? 'Another address' : 'A new address'}</span>
          <div class="field">
            <label for="quick-r-label">Label</label>
            <input id="quick-r-label" type="text" bind:value={rLabel} placeholder="Who pays, or what for" autocomplete="off" />
          </div>
          <AmountInput id="quick-r-amount" label="Amount (optional)" bind:value={rAmount} />
        </section>
        <button type="submit" class="btn cta" disabled={busy}><span>{busy ? 'Creating…' : 'Create address'}</span><Icon name="arrow" size={24} /></button>
        <button type="button" class="btn ghost more" onclick={() => full('receive')}>Requests made before, address types: the Receive page</button>
      </form>
    {/if}
  </aside>
{/if}

<style>
  .quick-panel {
    display: flex;
    flex-direction: column;
    width: 25.5rem;
    flex: none;
    min-height: 0;
    margin: 0.75rem 0.75rem 0.75rem 0;
    border: 1px solid var(--line);
    border-radius: var(--radius-xl);
    background: linear-gradient(180deg, var(--card-top), transparent 12rem), var(--surface);
    box-shadow: var(--shadow-card);
    overflow: hidden;
  }
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
    padding: 0.9rem 0.9rem 0.4rem 1.1rem;
  }
  .modes {
    display: flex;
    gap: 0.2rem;
    padding: 0.2rem;
    border-radius: 999px;
    background: var(--surface-2);
    border: 1px solid var(--line);
  }
  .modes button {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    padding: 0.4rem 1rem;
    border: 0;
    border-radius: 999px;
    background: none;
    color: var(--text-muted);
    font: inherit;
    font-size: 1.05rem;
    font-weight: 600;
    cursor: pointer;
  }
  .modes button[aria-selected='true'] {
    background: var(--raised);
    color: var(--text);
  }
  .body {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
    padding: 0.6rem 1.1rem 1.1rem;
  }
  .box {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
    padding: 1rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-l);
    background: var(--surface-2);
  }
  .line {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
  }
  .avail {
    display: inline-flex;
    align-items: center;
    gap: 0.45rem;
    font-weight: 600;
  }
  .max {
    padding: 0.05rem 0.5rem;
    border: 0;
    border-radius: 999px;
    background: var(--raised);
    color: var(--text);
    font: inherit;
    font-size: 0.72rem;
    font-weight: 700;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    cursor: pointer;
  }
  .max:hover {
    background: var(--accent-soft);
  }
  /* The amount, big: the number one types is what matters here. */
  .big-amount :global(input) {
    min-height: 3.2rem;
    font-size: 1.6rem;
    font-weight: 600;
    font-family: var(--font-num);
    letter-spacing: -0.01em;
  }
  .fee {
    display: flex;
    justify-content: space-between;
    gap: 0.5rem;
    margin: 0 0.2rem;
    font-size: 0.86rem;
  }
  .link {
    padding: 0;
    border: 0;
    background: none;
    color: var(--link);
    font: inherit;
    cursor: pointer;
  }
  .more {
    font-size: 0.86rem;
    color: var(--text-muted);
    white-space: normal;
  }
  .small {
    font-size: 0.88rem;
  }
  .made {
    align-items: center;
  }
  .qr {
    padding: 0.6rem;
    border-radius: var(--radius-m);
    background: #ffffff;
  }
  .addr {
    text-align: center;
    font-size: 0.86rem;
  }
  .center {
    justify-content: center;
    text-align: center;
  }
  @media (max-width: 900px) {
    .quick-panel {
      position: fixed;
      z-index: 35;
      top: 0;
      right: 0;
      bottom: 0;
      width: min(25.5rem, 100vw);
      margin: 0;
      border-radius: 0;
      box-shadow: var(--shadow);
    }
  }
</style>
