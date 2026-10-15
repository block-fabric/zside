<script lang="ts">
  // The page of this chain of its own that is shown (chain/local.ts lists them). The template has none: a
  // sidechain imports its pages here and adds a branch per page, as views/PageView.svelte does for the kit:
  //
  //   import Names from './pages/Names.svelte';
  //   ...
  //   {#if page === 'names'}<Names />{/if}
  let { page }: { page: string } = $props();
</script>

{#if page === ''}
  <!-- No page of its own. -->
{/if}
