<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  // A payment request: the QR code, the link, the address (ReceiveRequestDialog, QRImageWidget).
  import SafeText from './SafeText.svelte';
  import Amount from './Amount.svelte';
  import CopyButton from './CopyButton.svelte';
  import Dialog from './Dialog.svelte';
  import Icon from './Icon.svelte';
  import QrCode from './QrCode.svelte';
  import { errorText, inform } from '../lib/dialogs.svelte';
  import { FILTERS, saveBase64As, safeFileName } from '../lib/files';
  import { qrPngBase64 } from '../lib/qr';
  import { toast } from '../lib/state.svelte';
  import { formatPaymentUri } from '../lib/uri';
  import { walletApi } from '../lib/wallet';

  let {
    request = $bindable(null),
    wallet,
    externalSigner = false,
  }: { request?: { address: string; label: string; amount: bigint; message: string } | null; wallet: string; externalSigner?: boolean } = $props();

  let open = $state(false);
  $effect(() => {
    open = request !== null;
  });

  const uri = $derived(request ? formatPaymentUri({ address: request.address, amount: request.amount > 0n ? request.amount : undefined, label: request.label || undefined, message: request.message || undefined }) : '');

  async function saveImage() {
    if (!request) return;
    const png = qrPngBase64(uri, request.address);
    if (!png) {
      toast('error', t('Save QR code'), t('The picture could not be made'));
      return;
    }
    const path = await saveBase64As({ title: 'Save QR Code', name: safeFileName(`${request.label || request.address}.png`), filters: [FILTERS.png] }, png).catch((e) => {
      toast('error', t('Save QR code'), errorText(e));
      return null;
    });
    if (path) toast('success', t('Saved'), path);
  }

  async function verify() {
    if (!request) return;
    try {
      await walletApi(wallet).displayAddress(request.address);
    } catch (e) {
      await inform(t('Signer error'), errorText(e), 'warning');
    }
  }
</script>

<Dialog bind:open title={request ? `Request payment to ${request.label || request.address}` : ''} wide onclose={() => (request = null)}>
  {#if request}
    <div class="request">
      <div class="qr">
        <QrCode text={uri} size={240} label="QR code of the payment request" />
        <span class="mono caption break">{request.address}</span>
      </div>
      <dl class="facts">
        <dt>Payment information</dt>
        <dd></dd>
        <dt>URI</dt>
        <dd class="with-copy"><span class="mono break uri">{uri}</span><CopyButton text={uri} label="Copy URI" /></dd>
        <dt>Address</dt>
        <dd class="with-copy"><span class="mono break">{request.address}</span><CopyButton text={request.address} label="Copy address" /></dd>
        {#if request.amount > 0n}
          <dt>Amount</dt>
          <dd><Amount sats={request.amount} /></dd>
        {/if}
        {#if request.label}
          <dt>Label</dt>
          <dd class="break"><SafeText text={request.label} /></dd>
        {/if}
        {#if request.message}
          <dt>Message</dt>
          <dd class="break"><SafeText text={request.message} /></dd>
        {/if}
        {#if wallet}
          <dt>Wallet</dt>
          <dd class="break">{wallet}</dd>
        {/if}
      </dl>
    </div>
  {/if}
  {#snippet actions()}
    {#if externalSigner}
      <button type="button" class="btn" onclick={verify} title="Verify this address on e.g. a hardware wallet screen">Verify</button>
    {/if}
    <CopyButton text={uri} label="Copy URI" iconOnly={false} small={false} />
    <CopyButton text={request?.address ?? ''} label="Copy address" iconOnly={false} small={false} />
    <button type="button" class="btn" onclick={saveImage}><Icon name="download" size={16} /> Save image…</button>
    <button type="button" class="btn primary" onclick={() => (request = null)}>Close</button>
  {/snippet}
</Dialog>

<style>
  .request {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr);
    gap: 1.4rem;
    align-items: start;
  }
  .qr {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 0.4rem;
    padding: 0.6rem;
    border-radius: var(--radius-m);
    background: #ffffff;
  }
  .caption {
    max-width: 240px;
    color: #000000;
    font-size: 0.72rem;
    text-align: center;
  }
  .with-copy {
    display: flex;
    align-items: center;
    gap: 0.3rem;
  }
  .uri {
    font-size: 0.85rem;
  }
  @media (max-width: 760px) {
    .request {
      grid-template-columns: minmax(0, 1fr);
    }
  }
</style>
