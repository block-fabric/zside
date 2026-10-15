<script lang="ts">
  // What a page that needs a wallet shows without one: how to get one, or that this node runs without
  // wallets (-disablewallet), as chains-qt then shows no wallet pages at all.
  import { t } from '../lib/i18n.svelte';
  import { CHAIN } from '../chain/config';
  import Icon from './Icon.svelte';
  import { go, type PageInfo } from '../lib/nav.svelte';
  import { walletsEnabled } from '../lib/state.svelte';

  let { page }: { page: PageInfo } = $props();
</script>

<div class="page no-wallet">
  <header class="page-head"><h1>{t(page.title)}</h1></header>
  <div class="panel body">
    <Icon name={page.icon} size={40} />
    <div>
      <p class="summary">{t(page.summary)}.</p>
      {#if walletsEnabled()}
        <p class="muted">This page needs a wallet, and none is open. Create a new wallet, or open one of the wallets of this node.</p>
        <div class="actions">
          <button type="button" class="btn primary" onclick={() => go('wallets', 'create')}>Create wallet…</button>
          <button type="button" class="btn" onclick={() => go('wallets', 'open')}>Open wallet…</button>
        </div>
      {:else}
        <p class="muted">This node runs without wallets (the option -disablewallet is set). Start {CHAIN.name} without it to use wallets.</p>
      {/if}
    </div>
  </div>
</div>

<style>
  .body {
    display: flex;
    gap: 1.2rem;
    align-items: flex-start;
    margin-top: 1rem;
    padding: 1.5rem;
    color: var(--accent-fg);
  }
  .body > div {
    color: var(--text);
  }
  .summary {
    font-size: 1.1rem;
    font-weight: 550;
  }
  .actions {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
    margin-top: 0.9rem;
  }
</style>
