<script lang="ts">
  // Typing or pasting an address of Chains: checked at once offline, then by the node.
  import Icon from './Icon.svelte';
  import { CHAIN } from '../chain/config';
  import { t } from '../lib/i18n.svelte';
  import { checkAddress, exampleAddress } from '../lib/address';
  import { host, rpc } from '../lib/rpc';
  import { app } from '../lib/state.svelte';

  let {
    value = $bindable(''),
    valid = $bindable(false),
    id,
    label = 'Pay to',
    required = true,
    onbook = undefined,
    disabled = false,
    hint = '',
    of = 'own',
  }: {
    value?: string;
    valid?: boolean;
    id: string;
    label?: string;
    required?: boolean;
    onbook?: () => void;
    disabled?: boolean;
    hint?: string;
    /** An address of this chain, or (a sidechain's withdrawals) of its mainchain, which only the offline check knows. */
    of?: 'own' | 'mainchain';
  } = $props();
  const chainName = $derived(of === 'mainchain' ? (CHAIN.mainchain?.name ?? CHAIN.name) : CHAIN.name);

  let error = $state('');
  let checking = $state(false);
  let touched = $state(false);
  const network = $derived(app.info?.network ?? 'main');
  let generation = 0;

  $effect(() => {
    const text = value.trim();
    const net = network;
    const mine = ++generation;
    valid = false;
    if (!text) {
      error = required && touched ? 'Enter an address' : '';
      return;
    }
    checking = true;
    const timer = setTimeout(async () => {
      const local = await checkAddress(text, net, of);
      if (mine !== generation) return;
      if (!local.valid) {
        error = local.error;
        checking = false;
        return;
      }
      if (of === 'mainchain') {
        // The node of this chain does not know the addresses of the mainchain: the mainchain node checks it
        // when the withdrawal is made.
        error = '';
        valid = true;
        checking = false;
        return;
      }
      try {
        // The node has the last word (scripts it knows, the network it runs on).
        const result = await rpc.call('validateaddress', [text]);
        if (mine !== generation) return;
        error = result.isvalid ? '' : result.error || 'The node does not take this address';
        valid = result.isvalid;
      } catch {
        if (mine !== generation) return;
        // The node is not there (yet): the offline check stands.
        error = '';
        valid = true;
      }
      checking = false;
    }, 180);
    return () => clearTimeout(timer);
  });

  async function paste() {
    try {
      const text = await host.paste();
      if (text) value = text.trim();
      touched = true;
    } catch {
      // Nothing to paste.
    }
  }
</script>

<div class="field">
  <label for={id}>{label}</label>
  <div class="row">
    <input
      {id}
      type="text"
      class="mono grow"
      autocomplete="off"
      spellcheck="false"
      placeholder={t('Enter an address of %1 (%2)', chainName, exampleAddress(network, of))}
      bind:value
      onblur={() => (touched = true)}
      aria-invalid={error ? 'true' : undefined}
      aria-describedby={error ? `${id}-error` : undefined}
      {disabled}
    />
    {#if onbook}
      <button type="button" class="btn icon" title="Choose previously used address" aria-label="Choose previously used address" {disabled} onclick={onbook}>
        <Icon name="book" />
      </button>
    {/if}
    <button type="button" class="btn icon" title="Paste address from clipboard" aria-label="Paste address from clipboard" {disabled} onclick={paste}>
      <Icon name="paste" />
    </button>
  </div>
  {#if error}
    <span class="error" id="{id}-error">{error}</span>
  {:else if valid}
    <span class="hint ok"><Icon name="check" size={14} /> Valid address</span>
  {:else if checking}
    <span class="hint">Checking…</span>
  {:else if hint}
    <span class="hint">{hint}</span>
  {/if}
</div>

<style>
  .ok {
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
    color: var(--positive);
  }
</style>
