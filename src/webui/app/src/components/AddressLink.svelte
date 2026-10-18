<script lang="ts">
  // An address the explorer shows: a link to its view (onopen) and, when it is the current wallet's, marked "yours"
  // with its label in the tooltip (lib/owned.svelte.ts asks the wallet, for the addresses shown only). The address
  // comes from the chain: shown as it is (SafeText).
  import SafeText from './SafeText.svelte';
  import { shortAddress } from '../lib/explorer';
  import { t } from '../lib/i18n.svelte';
  import { ownerOf } from '../lib/owned.svelte';

  let { address, onopen, short = 0 }: { address: string; onopen?: (address: string) => void; short?: number } = $props();

  const owner = $derived(ownerOf(address));
  const shown = $derived(short ? shortAddress(address, short) : address);
  const tip = $derived(owner ? (owner.label ? t('Yours: %1', owner.label) : t('An address of this wallet')) : address);
</script>

<span class="addr" class:ours={!!owner} data-ours={owner ? '' : undefined}>
  {#if onopen}<button type="button" class="mono linkish" title={tip} onclick={() => onopen(address)}><SafeText text={shown} /></button>
  {:else}<SafeText text={shown} class="mono" title={tip} />{/if}
  {#if owner}<span class="badge accent yours" title={tip}>{t('yours')}</span>{/if}
</span>

<style>
  .addr {
    display: inline-flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 0.35em;
    min-width: 0;
    overflow-wrap: anywhere;
  }
  .addr.ours {
    padding: 0 0.3em;
    border-radius: var(--radius-s);
    background: var(--accent-soft);
    box-shadow: inset 0 0 0 1px color-mix(in srgb, var(--accent) 35%, transparent);
  }
  .linkish {
    padding: 0;
    border: 0;
    background: none;
    color: var(--link);
    text-align: left;
    cursor: pointer;
    overflow-wrap: anywhere;
  }
  .linkish:hover {
    text-decoration: underline;
  }
  .yours {
    font-size: 0.7rem;
    padding: 0 0.4rem;
    white-space: nowrap;
  }
</style>
