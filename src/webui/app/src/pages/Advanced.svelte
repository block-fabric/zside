<script lang="ts">
  // Advanced transactions (user decision, 2026-10-15): Sign and verify, partially signed transactions
  // (PSBT) and the multisig lounge are the tabs of one page, with the wallet. Their old page ids lead here
  // (PageInfo.tabOf); each keeps its own tabs after "/" in nav.tab ("messages/verify", "psbt/file").
  import { t } from '../lib/i18n.svelte';
  import TabRow from '../components/TabRow.svelte';
  import { SUBTABS, subTabOf } from '../lib/subtabs';
  import { nav, outerTab, setOwnTab, type PageId } from '../lib/nav.svelte';
  import Messages from './Messages.svelte';
  import Multisig from './Multisig.svelte';
  import Psbt from './Psbt.svelte';

  const TABS = [
    { id: 'messages', label: 'Sign & verify' },
    { id: 'psbt', label: 'PSBT' },
    { id: 'multisig', label: 'Multisig' },
  ];
  const tab = $derived(TABS.some((tb) => tb.id === outerTab()) ? outerTab() : 'messages');
  // The sub-tabs of the page shown in this tab, if it has some (SUBTABS).
  const subRow = $derived.by(() => {
    const page = tab as PageId;
    const sub = SUBTABS[page];
    return sub ? { label: sub.label, tabs: sub.tabs, current: subTabOf(page), select: (id: string) => setOwnTab(page, id) } : null;
  });
</script>

<div class="page advanced">
  <header class="page-head"><h1>Advanced transactions</h1></header>
  <!-- The tabs of the tab shown follow on the same row, after an arrow. -->
  <TabRow label="Advanced transactions" tabs={TABS} current={tab} select={(id) => (nav.tab = id)} sub={subRow} />
  {#key tab}
    {#if tab === 'messages'}
      <Messages embedded />
    {:else if tab === 'psbt'}
      <Psbt embedded />
    {:else}
      <Multisig embedded />
    {/if}
  {/key}
</div>

<style>
  .advanced {
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
  }
</style>
