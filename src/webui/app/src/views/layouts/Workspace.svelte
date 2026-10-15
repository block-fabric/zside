<script lang="ts">
  // Workspace tabs: Home and five areas across the top (Wallet, Chain, Sidechains, Tools, Node), each
  // with its sub-tabs; the actions of the old menu bar in the "Menus" menu and the palette; the detail
  // panel (or Home's Send/Receive side panel) at the right of the page; the status bar.
  import { t } from '../../lib/i18n.svelte';
  import Brand from '../../components/Brand.svelte';
  import DetailPanel from '../../components/DetailPanel.svelte';
  import Icon from '../../components/Icon.svelte';
  import MenuButton from '../../components/MenuButton.svelte';
  import PaletteButton from '../../components/PaletteButton.svelte';
  import QuickPanel from '../../components/QuickPanel.svelte';
  import StatusBar from '../../components/StatusBar.svelte';
  import TopActions from '../../components/TopActions.svelte';
  import WalletPicker from '../../components/WalletPicker.svelte';
  import WindowControls from '../../components/WindowControls.svelte';
  import { detail } from '../../lib/detail.svelte';
  import { closeQuick } from '../../lib/quick.svelte';
  import { buildCommands, menuSections, pageAvailable, pageDisabled } from '../../lib/commands.svelte';
  import { AREAS, areaOf, isAt, placeLabel, type Area, type Place } from '../../lib/layout';
  import { go, nav, PAGES } from '../../lib/nav.svelte';
  import { frame, titleBarDoubleClick, titleBarMouseDown } from '../../lib/windowframe.svelte';
  import SyncOverlay from '../../components/SyncOverlay.svelte';
  import PageView from '../PageView.svelte';

  // The top bar is the integrated title bar too: its empty part moves the window, the buttons of
  // the window at its right.
  let top: HTMLElement | undefined = $state();

  const area = $derived(areaOf(nav.page));
  const tabs = $derived(area.tabs.filter((t) => pageAvailable(t.page)));
  const sections = $derived(menuSections(buildCommands()));

  // One panel at the right, as in the dashboard: a row opened in the detail panel closes Home's
  // Send/Receive; so does a new page.
  $effect(() => {
    if (detail.item) closeQuick();
  });
  $effect(() => {
    if (nav.page !== 'home') closeQuick();
  });

  /** The last place of each area, to come back to it. */
  const last: Partial<Record<Area['id'], Place>> = {};
  $effect(() => {
    last[areaOf(nav.page).id] = { page: nav.page, tab: nav.tab };
  });

  function enter(a: Area) {
    const place = last[a.id] ?? a.tabs.find((t) => !pageDisabled(t.page)) ?? a.tabs[0];
    go(place.page, place.tab ?? '');
  }

  function areaKey(e: KeyboardEvent, index: number) {
    if (e.key !== 'ArrowRight' && e.key !== 'ArrowLeft') return;
    e.preventDefault();
    const next = (index + (e.key === 'ArrowRight' ? 1 : -1) + AREAS.length) % AREAS.length;
    const buttons = (e.currentTarget as HTMLElement).parentElement?.querySelectorAll<HTMLElement>('.area');
    buttons?.[next]?.focus();
  }
</script>

<div class="workspace">
  <!-- svelte-ignore a11y_no_static_element_interactions (the drag of the window; its buttons are the keyboard way) -->
  <header
    class="top"
    class:titlebar={frame.integrated}
    bind:this={top}
    onmousedown={(e) => top && titleBarMouseDown(e, top)}
    ondblclick={(e) => top && titleBarDoubleClick(e, top)}
  >
    <Brand onclick={() => go('home')} />
    <nav class="areas" aria-label="Areas">
      {#each AREAS as a, i (a.id)}
        <button type="button" class="area" aria-label={t(a.label)} title={t(a.label)} aria-current={area.id === a.id ? 'true' : undefined} onclick={() => enter(a)} onkeydown={(e) => areaKey(e, i)}>
          <Icon name={a.icon} size={18} />
          <span class="label">{t(a.label)}</span>
        </button>
      {/each}
    </nav>
    <div class="tools">
      <WalletPicker pill />
      <PaletteButton />
      <MenuButton label="Menus of chains-qt: wallet, banking, chains, tools, window, node" text="Menus" icon="menu" {sections} />
      <TopActions />
    </div>
    {#if frame.integrated}<WindowControls />{/if}
  </header>

  <nav class="subtabs" aria-label="{area.label} pages">
    {#each tabs as tb (tb.page + (tb.tab ?? ''))}
      {@const current = isAt(tb, nav.page, nav.tab)}
      <button
        type="button"
        aria-current={current ? 'page' : undefined}
        disabled={pageDisabled(tb.page)}
        title={PAGES[tb.page].shortcut ? `${t(PAGES[tb.page].summary)} (${PAGES[tb.page].shortcut})` : t(PAGES[tb.page].summary)}
        onclick={() => go(tb.page, tb.tab ?? '')}
      >
        {t(tb.label ?? PAGES[tb.page].title)}
        {#if !PAGES[tb.page].ready}<span class="soon" aria-label="(being rebuilt)">•</span>{/if}
      </button>
    {/each}
  </nav>

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

<style>
  .workspace {
    display: flex;
    flex-direction: column;
    height: 100%;
  }
  .top {
    display: flex;
    align-items: center;
    gap: 1rem;
    min-height: 3.4rem;
    padding: 0.35rem 0.75rem;
    background: color-mix(in srgb, var(--bg) 70%, transparent);
    border-bottom: 1px solid var(--line);
  }
  .areas {
    display: flex;
    align-items: center;
    gap: 0.2rem;
    flex: none;
    padding: 0.22rem;
    border: 1px solid var(--line);
    border-radius: 999px;
    background: var(--surface-2);
  }
  .area {
    display: inline-flex;
    align-items: center;
    gap: 0.45rem;
    min-height: 2.15rem;
    padding: 0 0.95rem;
    border: 0;
    border-radius: 999px;
    background: none;
    color: var(--text-muted);
    font: inherit;
    font-weight: 600;
    white-space: nowrap;
    cursor: pointer;
  }
  .area:hover {
    color: var(--text);
    background: var(--raised);
  }
  .area[aria-current='true'] {
    color: var(--on-accent);
    background: var(--accent);
    box-shadow: 0 6px 18px -8px var(--accent);
  }
  .area[aria-current='true']:hover {
    color: var(--on-accent);
    background: var(--accent);
    box-shadow: var(--glow);
  }
  .top.titlebar {
    padding-right: 0;
    user-select: none;
    -webkit-user-select: none;
  }
  .top.titlebar > :global(.window-controls) {
    margin: -0.3rem 0;
  }
  .tools {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    margin-left: auto;
    min-width: 0;
  }
  .subtabs {
    display: flex;
    gap: 0.15rem;
    padding: 0.35rem 1.1rem 0;
    overflow-x: auto;
  }
  .subtabs button {
    position: relative;
    padding: 0.45rem 0.85rem;
    border: 0;
    border-bottom: 2px solid transparent;
    background: none;
    color: var(--text-muted);
    font: inherit;
    font-size: 0.93rem;
    font-weight: 550;
    white-space: nowrap;
    cursor: pointer;
  }
  .subtabs button:hover:not(:disabled) {
    color: var(--text);
  }
  .subtabs button[aria-current='page'] {
    color: var(--text);
    border-bottom-color: var(--accent);
  }
  .subtabs button:disabled {
    opacity: 0.45;
    cursor: default;
  }
  .soon {
    margin-left: 0.25rem;
    color: var(--text-faint);
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
  /* The node window's own tabs are the sub-tabs of the Node area here. */
  .content[data-page='node'] :global(.page > .tabs) {
    display: none;
  }
  .tools :global(.wallet),
  .tools :global(.palette-button) {
    flex: none;
  }
  /* Home and five areas take room: on a smaller window only the current area keeps its name (the
     others their icon, named in their tooltip and for screen readers). */
  @media (min-width: 761px) and (max-width: 1380px) {
    .top.titlebar .area:not([aria-current='true']) .label {
      display: none;
    }
    .top.titlebar .area:not([aria-current='true']) {
      padding: 0 0.7rem;
    }
  }
  @media (min-width: 761px) and (max-width: 1240px) {
    .area:not([aria-current='true']) .label {
      display: none;
    }
    .area:not([aria-current='true']) {
      padding: 0 0.7rem;
    }
  }
  /* The buttons of the window take room: the palette field gives it first. */
  @media (max-width: 1320px) {
    .top.titlebar .tools :global(.palette-button) {
      min-width: 0;
    }
    .top.titlebar .tools :global(.palette-button .grow),
    .top.titlebar .tools :global(.palette-button kbd) {
      display: none;
    }
  }
  @media (max-width: 1100px) {
    .tools :global(.palette-button) {
      min-width: 0;
    }
    .tools :global(.palette-button .grow),
    .tools :global(.palette-button kbd) {
      display: none;
    }
  }
  @media (max-width: 760px) {
    .top {
      flex-wrap: wrap;
      gap: 0.4rem;
      padding: 0.3rem 0.5rem 0;
    }
    .top > :global(.brand .name),
    .top > :global(.brand .net) {
      display: none;
    }
    .areas {
      border-radius: var(--radius-l);
      order: 3;
      width: 100%;
      justify-content: space-between;
    }
    .area {
      border-radius: var(--radius-m);
      flex: 1;
      justify-content: center;
      padding: 0.45rem 0.3rem;
      flex-direction: column;
      gap: 0.1rem;
      font-size: 0.8rem;
    }
    .subtabs {
      padding: 0 0.3rem;
    }
  }
</style>
