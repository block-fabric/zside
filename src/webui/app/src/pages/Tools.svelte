<script lang="ts">
  // Tools (user decision, 2026-10-15): Timestamp a file, Proof of funds and the crypto tools (hash
  // calculator, Merkle tree, address decoder) are the tabs of one page. Their old page ids lead here
  // (PageInfo.tabOf); each keeps its own tabs after "/" in nav.tab ("crypto/merkle").
  import { t } from '../lib/i18n.svelte';
  import TabRow from '../components/TabRow.svelte';
  import { SUBTABS, subTabOf } from '../lib/subtabs';
  import NoWallet from '../components/NoWallet.svelte';
  import { nav, outerTab, setOwnTab, type PageId, PAGES } from '../lib/nav.svelte';
  import { wallets, walletsEnabled } from '../lib/state.svelte';
  import Crypto from './Crypto.svelte';
  import Proof from './Proof.svelte';
  import Timestamp from './Timestamp.svelte';

  const TABS = [
    { id: 'timestamp', label: 'Timestamp' },
    { id: 'proof', label: 'Proof of funds' },
    { id: 'crypto', label: 'Crypto tools' },
  ];
  const tab = $derived(TABS.some((tb) => tb.id === outerTab()) ? outerTab() : 'timestamp');
  // The sub-tabs of the page shown in this tab, if it has some (SUBTABS).
  const subRow = $derived.by(() => {
    const page = tab as PageId;
    const sub = SUBTABS[page];
    return sub ? { label: sub.label, tabs: sub.tabs, current: subTabOf(page), select: (id: string) => setOwnTab(page, id) } : null;
  });
</script>

<div class="page tools">
  <header class="page-head"><h1>Tools</h1></header>
  <!-- The tabs of the tab shown follow on the same row, after an arrow. -->
  <TabRow label="Tools" tabs={TABS} current={tab} select={(id) => (nav.tab = id)} sub={subRow} />
  {#key tab}
    {#if tab === 'timestamp'}
      <Timestamp embedded />
    {:else if tab === 'proof'}
      <!-- The proof is made with a wallet: without one (or with -disablewallet), the tab says why. -->
      {#if !walletsEnabled() || wallets.list.length === 0}<NoWallet page={PAGES.proof} />{:else}<Proof embedded />{/if}
    {:else}
      <Crypto embedded />
    {/if}
  {/key}
</div>

<style>
  .tools {
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
  }
</style>
