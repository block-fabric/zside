<script lang="ts">
  // The main window once the node runs, in the layout chosen in Settings → Appearance:
  // workspace tabs or dashboard (doc/webui-layouts.md). Whatever the
  // layout: the same pages, the command palette (Ctrl+K), the notices of new transactions.
  import { untrack } from 'svelte';
  import { pageTitleSource } from '../lib/uimode.svelte';
  import CommandPalette from '../components/CommandPalette.svelte';
  import TitleStrip from '../components/TitleStrip.svelte';
  import TxNotifier from '../components/TxNotifier.svelte';
  import LocalBackground from '../chain/LocalBackground.svelte';
  import { closeDetail, detail } from '../lib/detail.svelte';
  import { go, nav } from '../lib/nav.svelte';
  import { prefs, wallets } from '../lib/state.svelte';
  import Dashboard from './layouts/Dashboard.svelte';
  import Workspace from './layouts/Workspace.svelte';

  $effect(() => {
    detail.follow = prefs.layout === 'workspace';
  });

  // A new page: what was selected on the last one goes.
  $effect(() => {
    void nav.page;
    untrack(closeDetail);
  });

  // Every layout starts on Home (unless a page was asked for before: a payment URI opens Send).
  let started = false;
  $effect(() => {
    const page = nav.page;
    // Followed too: the page can be ready before the preferences are loaded (a fast start).
    const loaded = prefs.loaded;
    untrack(() => {
      if (page === 'overview' && !started && loaded) go('home');
      if (loaded) started = true;
    });
  });
</script>

<a class="skip" href="#content">Skip to the page</a>
<div class="frame">
  <!-- The integrated title bar: the workspace's is its top bar; the dashboard has a slim strip. -->
  {#if prefs.layout !== 'workspace'}
    <TitleStrip title={pageTitleSource(nav.page)} detail={wallets.current !== null ? wallets.current || 'default wallet' : ''} />
  {/if}
  <div class="layout">
    {#if prefs.layout === 'workspace'}
      <Workspace />
    {:else}
      <Dashboard />
    {/if}
  </div>
</div>
<CommandPalette />
<TxNotifier />
<LocalBackground />

<style>
  .frame {
    display: flex;
    flex-direction: column;
    height: 100%;
  }
  .layout {
    flex: 1 1 0;
    min-height: 0;
  }
  .skip {
    position: fixed;
    z-index: 60;
    top: -3rem;
    left: 0.5rem;
    padding: 0.4rem 0.8rem;
    border-radius: var(--radius-s);
    background: var(--accent);
    color: var(--on-accent);
  }
  .skip:focus {
    top: 0.5rem;
  }
</style>
