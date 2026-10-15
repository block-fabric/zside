<script lang="ts">
  // Under a list made of the account's history: the history read holds the newest transactions only, so a
  // line says so, with a button that reads more of it (audit port-2 #10: older memos were left out without a
  // word). Nothing when the whole history is there.
  import Icon from '../../components/Icon.svelte';
  import { formatCount } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { historyCut, showOlder, zsView } from './store.svelte';

  let { what }: { what: 'memos' | 'transactions' } = $props();
</script>

{#if historyCut()}
  <p class="zs-older muted small" role="status">
    <Icon name="info" size={14} />
    {#if what === 'memos'}{t('The memos of the newest %1 shielded transactions are listed; older ones are not.', formatCount(zsView.historyCount))}{:else}{t('The newest %1 shielded transactions are listed; older ones are not.', formatCount(zsView.historyCount))}{/if}
    {#if zsView.historyCount !== 0}<button type="button" class="btn ghost small" onclick={showOlder}>Show older</button>{/if}
  </p>
{/if}

<style>
  .zs-older {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem 0.6rem;
    margin: 0.2rem 0 0.6rem;
  }
</style>
