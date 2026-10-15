<script lang="ts">
  // The pages of zSide (chain/local.ts lists them): the one shown.
  import MeltCast from './pages/MeltCast.svelte';
  import Memos from './pages/Memos.svelte';
  import Pool from './pages/Pool.svelte';
  import Private from './pages/Private.svelte';
  import ZHistory from './pages/ZHistory.svelte';
  let { page }: { page: string } = $props();
</script>

{#if page === 'private'}<Private />
{:else if page === 'memos'}<Memos />
{:else if page === 'zhistory'}<ZHistory />
{:else if page === 'meltcast'}<MeltCast />
{:else if page === 'pool'}<Pool />
{/if}
