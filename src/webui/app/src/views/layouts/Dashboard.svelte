<script lang="ts">
  // Dashboard: a home of live cards (views/home/Home.svelte), every page in the sidebar, a light
  // bar above the page (where you are, the palette, the wallet, the notifications, the light/dark
  // switch), the detail panel or the Send/Receive side panel, the status bar.
  import { t } from '../../lib/i18n.svelte';
  import DetailPanel from '../../components/DetailPanel.svelte';
  import Icon from '../../components/Icon.svelte';
  import PaletteButton from '../../components/PaletteButton.svelte';
  import QuickPanel from '../../components/QuickPanel.svelte';
  import Sidebar from '../../components/Sidebar.svelte';
  import StatusBar from '../../components/StatusBar.svelte';
  import TopActions from '../../components/TopActions.svelte';
  import { detail } from '../../lib/detail.svelte';
  import { closeQuick } from '../../lib/quick.svelte';
  import { navSectionOf } from '../../lib/layout';
  import { go, nav } from '../../lib/nav.svelte';
  import { pageTitle } from '../../lib/uimode.svelte';
  import SyncOverlay from '../../components/SyncOverlay.svelte';
  import PageView from '../PageView.svelte';

  let drawer = $state(false);
  const section = $derived(navSectionOf(nav.page));

  // One panel at the right: a row opened in the detail panel closes Send/Receive; so does a new page.
  $effect(() => {
    if (detail.item) closeQuick();
  });
  $effect(() => {
    if (nav.page !== 'home') closeQuick();
  });
</script>

<div class="dashboard">
  <Sidebar bind:open={drawer} />
  <div class="main">
    <header class="top">
      <button type="button" class="btn ghost icon small drawer" aria-label="Pages" aria-expanded={drawer} onclick={() => (drawer = true)}><Icon name="menu" size={18} /></button>
      <nav class="crumbs" aria-label="Where you are">
        {#if nav.page === 'home'}
          <span class="here" aria-current="page">Home</span>
        {:else}
          <button type="button" class="link" onclick={() => go('home')}><Icon name="home" size={15} /> Home</button>
          <span class="sep" aria-hidden="true">/</span>
          {#if section}<span class="muted">{t(section)}</span><span class="sep" aria-hidden="true">/</span>{/if}
          <span class="here" aria-current="page">{pageTitle(nav.page)}</span>
        {/if}
      </nav>
      <PaletteButton />
      <!-- The wallet is switched in the sidebar's card (in the drawer on a narrow window). -->
      <TopActions />
    </header>
    <div class="body">
      <!-- Before the page: the dialogs a page opens come after it, above it. -->
      <SyncOverlay />
      <main class="content" id="content" tabindex="-1" data-page={nav.page}>
        <PageView />
      </main>
      <DetailPanel />
      <QuickPanel />
    </div>
    <StatusBar />
  </div>
</div>

<style>
  .dashboard {
    display: flex;
    height: 100%;
  }
  .main {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
  }
  .top {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    min-height: 3.6rem;
    padding: 0.5rem 1rem 0.3rem 1.5rem;
  }
  .drawer {
    display: none;
  }
  .crumbs {
    flex: 1;
    min-width: 0;
    display: flex;
    align-items: center;
    gap: 0.45rem;
    white-space: nowrap;
    overflow: hidden;
  }
  .link {
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
    padding: 0.15rem 0.35rem;
    border: 0;
    border-radius: var(--radius-s);
    background: none;
    color: var(--text-muted);
    font: inherit;
    cursor: pointer;
  }
  .link:hover {
    background: var(--raised);
    color: var(--text);
  }
  .sep {
    color: var(--text-faint);
  }
  .here {
    font-size: 1.15rem;
    font-weight: 650;
    letter-spacing: -0.01em;
  }
  .body {
    position: relative;
    flex: 1;
    min-height: 0;
    display: flex;
  }
  .content {
    flex: 1;
    min-width: 0;
    overflow: auto;
    outline: none;
  }
  @media (max-width: 760px) {
    .drawer {
      display: inline-flex;
    }
    .top :global(.palette-button) {
      min-width: 0;
    }
    .top :global(.palette-button .grow),
    .top :global(.palette-button kbd) {
      display: none;
    }
    .top :global(.wallet) {
      display: none;
    }
  }
  @media (max-width: 1100px) {
    .top :global(.palette-button) {
      min-width: 0;
    }
    .top :global(.palette-button .grow),
    .top :global(.palette-button kbd) {
      display: none;
    }
  }
</style>
