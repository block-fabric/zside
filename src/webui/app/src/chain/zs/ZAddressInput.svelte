<script lang="ts">
  // Typing or pasting an address of zSide that may be shielded (zo1…) or transparent (zs1…): checked at
  // once offline, then by the node for a transparent one; what it is, private or public, said under it.
  import Icon from '../../components/Icon.svelte';
  import { checkAddress } from '../../lib/address';
  import { t } from '../../lib/i18n.svelte';
  import { host, rpc } from '../../lib/rpc';
  import { app } from '../../lib/state.svelte';
  import Privacy from './Privacy.svelte';
  import { isShieldedAddress, looksShielded, shieldedHrp } from './model';

  let {
    value = $bindable(''),
    valid = $bindable(false),
    kind = $bindable<'shielded' | 'transparent' | ''>(''),
    id,
    label = 'Pay to',
    accept = 'any',
    required = true,
    disabled = false,
  }: {
    value?: string;
    valid?: boolean;
    kind?: 'shielded' | 'transparent' | '';
    id: string;
    label?: string;
    /** Which addresses it takes. */
    accept?: 'any' | 'shielded' | 'transparent';
    required?: boolean;
    disabled?: boolean;
  } = $props();

  const network = $derived(app.info?.network ?? 'main');
  let error = $state('');
  let touched = $state(false);
  let generation = 0;

  $effect(() => {
    const text = value.trim();
    const net = network;
    const mine = ++generation;
    valid = false;
    kind = '';
    if (!text) {
      error = required && touched ? t('Enter an address') : '';
      return;
    }
    if (looksShielded(text)) {
      if (!isShieldedAddress(text, net)) {
        error = text.toLowerCase().startsWith(`${shieldedHrp(net)}1`) ? t('This address has a typing error in it') : t('This address is for another network');
        return;
      }
      if (accept === 'transparent') {
        error = t('A shielded address: pay it privately instead');
        return;
      }
      error = '';
      kind = 'shielded';
      valid = true;
      return;
    }
    if (accept === 'shielded') {
      error = t('Enter a shielded address (%1)', `${shieldedHrp(net)}1…`);
      return;
    }
    const timer = setTimeout(async () => {
      const local = await checkAddress(text, net);
      if (mine !== generation) return;
      if (!local.valid) {
        error = t(local.error);
        return;
      }
      try {
        const result = await rpc.call('validateaddress', [text]);
        if (mine !== generation) return;
        error = result.isvalid ? '' : result.error || t('The node does not take this address');
        valid = result.isvalid;
      } catch {
        if (mine !== generation) return;
        error = '';
        valid = true;
      }
      if (valid) kind = 'transparent';
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

  const placeholder = $derived(
    accept === 'shielded' ? t('A shielded address (%1)', `${shieldedHrp(network)}1…`) : accept === 'transparent' ? t('A transparent address') : t('A shielded address (%1), or a transparent one', `${shieldedHrp(network)}1…`),
  );
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
      {placeholder}
      bind:value
      onblur={() => (touched = true)}
      aria-invalid={error ? 'true' : undefined}
      aria-describedby={error ? `${id}-error` : undefined}
      {disabled}
    />
    <button type="button" class="btn icon" title="Paste address from clipboard" aria-label="Paste address from clipboard" {disabled} onclick={paste}>
      <Icon name="paste" />
    </button>
  </div>
  {#if error}
    <span class="error" id="{id}-error">{error}</span>
  {:else if valid && kind}
    <span class="hint kind"><Privacy kind={kind === 'shielded' ? 'private' : 'public'} /> {kind === 'shielded' ? t('A shielded address') : t('A transparent address')}</span>
  {/if}
</div>

<style>
  .kind {
    display: inline-flex;
    align-items: center;
    gap: 0.45rem;
  }
</style>
