<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  import { CHAIN } from '../chain/config';
  import SafeText from '../components/SafeText.svelte';
  // Sign and verify messages (SignVerifyMessageDialog).
  import AddressPicker from '../components/AddressPicker.svelte';
  import CopyButton from '../components/CopyButton.svelte';
  import Icon from '../components/Icon.svelte';
  import { BridgeError } from '../lib/bridge';
  import { withUnlock } from '../lib/dialogs.svelte';
  import { ownTab, setOwnTab } from '../lib/nav.svelte';
  import { host } from '../lib/rpc';
  import { wallets } from '../lib/state.svelte';
  import { messages, walletApi } from '../lib/wallet';

  // A tab of a page that holds it (Tools, Advanced transactions): without a title of its own.
  let { embedded = false }: { embedded?: boolean } = $props();

  const wallet = $derived(wallets.current);
  const tab = $derived(ownTab('messages').startsWith('verify') ? 'verify' : 'sign');

  // Sign
  let signAddress = $state('');
  let signMessage = $state('');
  let signature = $state('');
  let signStatus = $state({ text: '', ok: false });
  let signBusy = $state(false);
  let signPicker = $state(false);

  // Verify
  let verifyAddress = $state('');
  let verifyMessage = $state('');
  let verifySignature = $state('');
  let verifyStatus = $state({ text: '', ok: false, field: '' });

  // An address given with the tab ("sign:<address>", from the address book).
  $effect(() => {
    const tab = ownTab('messages');
    const i = tab.indexOf(':');
    if (i < 0) return;
    if (tab.startsWith('sign')) signAddress = tab.slice(i + 1);
    else verifyAddress = tab.slice(i + 1);
    setOwnTab('messages', tab.slice(0, i));
  });

  async function sign() {
    if (wallet === null || signBusy) return;
    // Clear the old signature first, so that no old one shows with an error.
    signature = '';
    signStatus = { text: '', ok: false };
    signBusy = true;
    try {
      const api = walletApi(wallet);
      const r = await withUnlock(wallet, (passphrase) => api.signMessage(signAddress.trim(), signMessage, passphrase));
      if (!r) {
        signStatus = { text: t('Wallet unlock was cancelled.'), ok: false };
      } else {
        signature = r.signature;
        signStatus = { text: t('Message signed.'), ok: true };
      }
    } catch (e) {
      signStatus = { text: e instanceof BridgeError || e instanceof Error ? e.message : String(e), ok: false };
    } finally {
      signBusy = false;
    }
  }

  async function verify() {
    verifyStatus = { text: '', ok: false, field: '' };
    try {
      const r = await messages.verify(verifyAddress.trim(), verifySignature.trim(), verifyMessage);
      verifyStatus = { text: r.message, ok: r.ok, field: r.field ?? '' };
    } catch (e) {
      verifyStatus = { text: e instanceof Error ? e.message : String(e), ok: false, field: '' };
    }
  }

  async function paste(target: 'sign' | 'verify') {
    const text = (await host.paste().catch(() => null))?.trim();
    if (!text) return;
    if (target === 'sign') signAddress = text;
    else verifyAddress = text;
  }
</script>

<div class="messages" class:page={!embedded}>
  {#if !embedded}<header class="page-head"><h1>Signatures - Sign / Verify a Message</h1></header>{/if}
  <!-- Inside another page (Tools, Advanced transactions), its tabs are on that page's row. -->
  {#if !embedded}
  <div class="tabs" role="tablist" aria-label="Sign or verify">
    <button type="button" role="tab" aria-selected={tab === 'sign'} onclick={() => (setOwnTab('messages', 'sign'))}>Sign Message</button>
    <button type="button" role="tab" aria-selected={tab === 'verify'} onclick={() => (setOwnTab('messages', 'verify'))}>Verify Message</button>
  </div>
  {/if}

  {#if tab === 'sign'}
    <section class="panel">
      <form
        class="panel-body stack"
        onsubmit={(e) => {
          e.preventDefault();
          sign();
        }}
      >
        <p class="muted">
          You can sign messages/agreements with your legacy (P2PKH) and native segwit (P2WPKH) addresses to prove you can receive coins sent to them. Be careful not to sign anything vague or random, as phishing
          attacks may try to trick you into signing your identity over to them. Only sign fully-detailed statements you agree to.
        </p>
        <div class="field">
          <label for="sign-address">Address</label>
          <div class="row">
            <input id="sign-address" type="text" class="mono grow" placeholder={t('The %1 address to sign the message with', CHAIN.name)} spellcheck="false" autocomplete="off" bind:value={signAddress} aria-invalid={signStatus.text && !signStatus.ok && signStatus.text.includes('address') ? 'true' : undefined} />
            <button type="button" class="btn icon" title="Choose previously used address" aria-label="Choose previously used address" onclick={() => (signPicker = true)}><Icon name="book" /></button>
            <button type="button" class="btn icon" title="Paste address from clipboard" aria-label="Paste address from clipboard" onclick={() => paste('sign')}><Icon name="paste" /></button>
          </div>
        </div>
        <div class="field">
          <label for="sign-message">Message</label>
          <textarea id="sign-message" rows="6" placeholder="Enter the message you want to sign here" bind:value={signMessage}></textarea>
        </div>
        <div class="field">
          <label for="signature-out">Signature</label>
          <div class="row">
            <input id="signature-out" type="text" class="mono grow" readonly value={signature} onfocus={(e) => (e.currentTarget as HTMLInputElement).select()} />
            <CopyButton text={signature} label="Copy the current signature to the system clipboard" small={false} />
          </div>
        </div>
        <div class="row">
          <button type="submit" class="btn primary" disabled={signBusy || wallet === null}><Icon name="edit" size={16} /> Sign Message</button>
          <button
            type="button"
            class="btn"
            onclick={() => {
              signAddress = signMessage = signature = '';
              signStatus = { text: '', ok: false };
            }}>Clear All</button
          >
          {#if signStatus.text}<span class="status" class:ok={signStatus.ok} class:bad={!signStatus.ok} role="status"><SafeText text={signStatus.text} /></span>{/if}
        </div>
      </form>
    </section>
  {:else}
    <section class="panel">
      <form
        class="panel-body stack"
        onsubmit={(e) => {
          e.preventDefault();
          verify();
        }}
      >
        <p class="muted">
          Enter the receiver's address, message (ensure you copy line breaks, spaces, tabs, etc. exactly) and signature below to verify the message. Be careful not to read more into the signature than what is in the
          signed message itself, to avoid being tricked by a man-in-the-middle attack. Note that this only proves the signing party receives with the address, it cannot prove sendership of any transaction!
        </p>
        <div class="field">
          <label for="verify-address">Address</label>
          <div class="row">
            <input id="verify-address" type="text" class="mono grow" placeholder={t('The %1 address the message was signed with', CHAIN.name)} spellcheck="false" autocomplete="off" bind:value={verifyAddress} aria-invalid={verifyStatus.field === 'address' ? 'true' : undefined} />
            <button type="button" class="btn icon" title="Paste address from clipboard" aria-label="Paste address from clipboard" onclick={() => paste('verify')}><Icon name="paste" /></button>
          </div>
        </div>
        <div class="field">
          <label for="verify-message">Message</label>
          <textarea id="verify-message" rows="6" placeholder="The signed message to verify" bind:value={verifyMessage}></textarea>
        </div>
        <div class="field">
          <label for="verify-signature">Signature</label>
          <input id="verify-signature" type="text" class="mono" placeholder="The signature given when the message was signed" spellcheck="false" autocomplete="off" bind:value={verifySignature} aria-invalid={verifyStatus.field === 'signature' ? 'true' : undefined} />
        </div>
        <div class="row">
          <button type="submit" class="btn primary"><Icon name="check" size={16} /> Verify Message</button>
          <button
            type="button"
            class="btn"
            onclick={() => {
              verifyAddress = verifyMessage = verifySignature = '';
              verifyStatus = { text: '', ok: false, field: '' };
            }}>Clear All</button
          >
          {#if verifyStatus.text}<span class="status" class:ok={verifyStatus.ok} class:bad={!verifyStatus.ok} role="status"><SafeText text={verifyStatus.text} /></span>{/if}
        </div>
      </form>
    </section>
  {/if}
</div>

<!-- No sending address book (user decision, 2026-10-15): the address to verify is typed or pasted. -->
<AddressPicker bind:open={signPicker} purpose="receive" singleKey onchoose={(a) => (signAddress = a.address)} />

<style>
  .messages {
    max-width: 60rem;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.85rem;
    padding-top: 1rem;
  }
  .status {
    font-weight: 600;
  }
  .status.ok {
    color: var(--positive);
  }
  .status.bad {
    color: var(--negative);
  }
</style>
