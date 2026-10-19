<script lang="ts">
  // The dashboard's navigation on the left: every page, in sections, names and icons, with Home
  // first; on a narrow window a drawer (open). At its top a card of the wallet shown (its name, its
  // lock, the unit); the pages in groups; at its foot the theme, the text
  // size, then Close wallet and Quit, apart. The current page is a filled pill.
  //
  // It folds into a rail of icons (user feedback 2026-10-19): the button at its foot, Ctrl+B, or the
  // palette; remembered in the preferences (prefs.sidebarCollapsed). Folded, every item says its name and
  // what it is for in its tooltip. A narrow window keeps the drawer, never folded.
  import { t } from '../lib/i18n.svelte';
  import { CHAIN } from '../chain/config';
  import Brand from './Brand.svelte';
  import Icon from './Icon.svelte';
  import MenuButton from './MenuButton.svelte';
  import WalletPicker from './WalletPicker.svelte';
  import { buildCommands, pageAvailable, pageDisabled } from '../lib/commands.svelte';
  import { navSections, pageIcon } from '../lib/layout';
  import { go, nav, PAGES, type PageId } from '../lib/nav.svelte';
  import { prefs, savePrefs, statusTip, wallets } from '../lib/state.svelte';
  import { pendingNow } from '../lib/settingsdraft.svelte';
  import { clampFontAdjust, THEMES } from '../lib/themes';
  import ThemePicker from './ThemePicker.svelte';
  import ModeSwitch from './ModeSwitch.svelte';
  import { pageShown, pageSummary, pageTitle, simpleMode } from '../lib/uimode.svelte';
  import { isThemeChoice } from '../lib/customthemes';
  import { frame } from '../lib/windowframe.svelte';

  let { open = $bindable(false) }: { open?: boolean } = $props();

  const sections = $derived(navSections().map((s) => ({ title: s.title, pages: s.pages.filter((id) => pageAvailable(id) && pageShown(id)) })).filter((s) => s.pages.length > 0));
  // Folded into a rail of icons (never the drawer of a narrow window: the CSS keeps it whole there).
  const collapsed = $derived(prefs.sidebarCollapsed && !open);
  const simple = $derived(simpleMode());
  const all = $derived(buildCommands());

  const shownWallet = $derived(wallets.list.find((w) => w.name === wallets.current) ?? null);
  const shownName = $derived(wallets.current ? wallets.current : t('default wallet'));
  const lock = $derived.by(() => {
    const w = shownWallet;
    if (!w) return null;
    if (w.privateKeysDisabled) return { label: 'Watch-only', title: 'Watch-only: no private keys', icon: 'eye' as const, tone: '' };
    if (!w.encrypted) return { label: 'Not encrypted', title: 'The wallet is not encrypted', icon: 'unlock' as const, tone: 'warning' };
    return w.locked
      ? { label: 'Locked', title: 'Encrypted, and locked', icon: 'lock' as const, tone: 'accent' }
      : { label: 'Unlocked', title: 'Encrypted, and unlocked for now', icon: 'unlock' as const, tone: 'warning' };
  });
  const leave = $derived(all.filter((c) => c.id === 'wallet.close' || c.id === 'node.exit'));

  function visit(id: PageId) {
    go(id);
    open = false;
  }
</script>

{#if open}
  <!-- The drawer of a narrow window: a click beside it closes it. -->
  <button type="button" class="scrim" aria-label="Close the navigation" onclick={() => (open = false)}></button>
{/if}

<svelte:window
  onkeydown={(e) => {
    if (e.key === 'Escape' && open) open = false;
  }}
/>

<nav class="sidebar" class:open class:collapsed aria-label="Pages">
  {#if !frame.integrated}
    <!-- The name of the app is in the integrated title bar, when there is one. -->
    <Brand name={!collapsed} onclick={() => visit('home')} />
  {/if}

  {#if wallets.list.length > 0}
    <section class="wallet-card" aria-label="Wallet shown">
      <div class="wc-top">
        <span class="avatar" aria-hidden="true"><Icon name="wallet" size={17} /></span>
        <div class="wc-name">
          <span class="caps">Wallet</span>
          <!-- The wallet shown, and the switch between wallets (a round pill); a long name ends in "…",
               whole in its tooltip. The unit of amounts is chosen in the status bar. -->
          {#if wallets.list.length > 1}<WalletPicker />{:else}<span class="name" translate="no" title={shownName}>{shownName}</span>{/if}
        </div>
      </div>
      <div class="wc-foot">
        {#if lock}<span class="badge {lock.tone}" title={t(lock.title)}><Icon name={lock.icon} size={12} /> {t(lock.label)}</span>{:else}<span></span>{/if}
        {#if pageAvailable('wallets') && nav.page !== 'wallets'}
          <!-- The page of the wallets, from the card (in Simple mode, where it is not in the list). -->
          <button type="button" class="btn ghost small manage" onclick={() => visit('wallets')}>Manage wallets</button>
        {/if}
      </div>
    </section>
  {/if}

  <div class="scroll">
    {#each sections as section, i (i)}
      <div class="section" role="group" aria-label={t(section.title || (i === 0 ? 'Home' : 'More'))}>
        {#if section.title}<div class="caption caps">{t(section.title)}</div>{:else if i > 0}<div class="caption gap" aria-hidden="true"></div>{/if}
        {#each section.pages as id (id)}
          {@const page = PAGES[id]}
          <button
            type="button"
            class="entry"
            class:current={nav.page === id}
            aria-current={nav.page === id ? 'page' : undefined}
            disabled={pageDisabled(id)}
            class:described={simple}
            title={collapsed ? `${pageTitle(id)}: ${pageSummary(id)}` : pageSummary(id)}
            aria-label={collapsed ? pageTitle(id) : undefined}
            onclick={() => visit(id)}
            onpointerenter={() => (statusTip.text = page.summary)}
            onpointerleave={() => (statusTip.text = '')}
          >
            <Icon name={pageIcon(id)} size={19} />
            <span class="text">
              <span class="grow">{pageTitle(id)}</span>
              {#if simple && page.simpleSummary}<small class="what">{t(page.simpleSummary)}</small>{/if}
            </span>
            {#if !page.ready}<span class="soon" title="Being rebuilt">•</span>{/if}
            {#if id === 'settings' && pendingNow() > 0}<span class="pending" title="Changes of the settings wait for Apply" aria-label="Changes wait for Apply"></span>{/if}
          </button>
        {/each}
      </div>
    {/each}
  </div>

  <div class="foot">
    <!-- Fold the sidebar into a rail of icons, or unfold it (Ctrl+B). -->
    <button
      type="button"
      class="entry fold"
      aria-expanded={!collapsed}
      aria-label={collapsed ? t('Expand the sidebar') : undefined}
      aria-keyshortcuts="Control+B"
      title={collapsed ? t('Expand the sidebar (Ctrl+B)') : t('Collapse the sidebar (Ctrl+B)')}
      onclick={() => savePrefs({ sidebarCollapsed: !prefs.sidebarCollapsed })}
    >
      <Icon name={collapsed ? 'chevron-right' : 'chevron-left'} size={18} />
      <span class="grow">Collapse</span>
    </button>
    <div class="row wide">
      <!-- The app's own list, opening upwards (user decision: the native one opened past the window). -->
      <ThemePicker
        value={prefs.theme}
        groups={[
          { title: '', options: THEMES.map((th) => ({ id: th.id, name: th.name })) },
          { title: 'Your themes', options: prefs.customThemes.map((th) => ({ id: th.id, name: th.name, own: true })) },
        ]}
        onpick={(v) => { if (isThemeChoice(v, prefs.customThemes)) savePrefs({ theme: v }); }}
      />
    </div>
    <div class="row wide">
      <!-- Simple or Advanced (user decision: here, beside the size of the text). -->
      <ModeSwitch />
      <button type="button" class="btn ghost small size" title="Smaller text (Ctrl+-)" aria-label="Smaller text" onclick={() => savePrefs({ fontAdjust: clampFontAdjust(prefs.fontAdjust - 1) })}>A−</button>
      <button type="button" class="btn ghost small size" title="Larger text (Ctrl++)" aria-label="Larger text" onclick={() => savePrefs({ fontAdjust: clampFontAdjust(prefs.fontAdjust + 1) })}>A+</button>
    </div>
    <div class="leave">
      {#each leave as c (c.id)}
        <button
          type="button"
          class="entry leave-entry"
          disabled={c.disabled}
          title={c.shortcut ? `${t(c.label)} (${c.shortcut})` : t(c.label)}
          aria-label={collapsed ? (c.id === 'node.exit' ? t('Quit %1', CHAIN.name) : t('Close wallet')) : undefined}
          onclick={() => c.run()}
        >
          <Icon name={c.id === 'node.exit' ? 'power' : 'logout'} size={18} />
          <span class="grow">{c.id === 'node.exit' ? t('Quit %1', CHAIN.name) : t('Close wallet')}</span>
        </button>
      {/each}
    </div>
  </div>
</nav>

<style>
  .sidebar {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    --side-pad-r: 0.6rem;
    width: 15rem;
    flex: none;
    padding: 0.7rem var(--side-pad-r) 0.6rem 0.6rem;
    min-height: 0;
  }
  /* The list scrolls with its bar flush at the right edge of the sidebar (user feedback 2026-10-19: it
     floated inside, a padding away from the edge): the scroller takes the sidebar's right padding into
     itself, so the items keep their place and the bar sits at the edge, folded or not. */
  .scroll {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    width: auto;
    margin-right: calc(-1 * var(--side-pad-r));
    padding-right: var(--side-pad-r);
  }
  .caption {
    padding: 0.9rem 0.75rem 0.3rem;
  }
  .wallet-card {
    display: flex;
    flex-direction: column;
    gap: 0.55rem;
    margin: 0.15rem 0 0.3rem;
    padding: 0.75rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-l);
    background: linear-gradient(180deg, var(--card-top), transparent), var(--surface);
    box-shadow: var(--shadow-card);
  }
  .wc-top {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
    min-width: 0;
  }
  .avatar {
    display: inline-grid;
    place-items: center;
    width: 2.3rem;
    height: 2.3rem;
    flex: none;
    border-radius: 50%;
    background: var(--accent);
    color: var(--on-accent);
    box-shadow: 0 0 20px -4px var(--accent);
  }
  /* The name takes the room beside the avatar (a narrow sidebar, large text, 125 or 150 % on Windows). */
  .wc-name {
    display: flex;
    flex-direction: column;
    gap: 0.15rem;
    flex: 1 1 5rem;
    min-width: 5rem;
  }
  .wc-name .caps {
    font-size: 0.66rem;
  }
  .wc-name .name {
    font-weight: 650;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  /* The switch between wallets: a round pill, as the other choices of the app. */
  .wc-name :global(select) {
    width: 100%;
    min-width: 0;
    min-height: 1.9rem;
    padding: 0 1.7rem 0 0.75rem;
    border: 1px solid var(--line);
    border-radius: 999px;
    background-color: var(--surface-2);
    font-weight: 650;
    text-overflow: ellipsis;
    white-space: nowrap;
    overflow: hidden;
  }
  .wc-foot {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.4rem;
  }
  .manage {
    flex: none;
    margin-left: auto;
    padding-inline: 0.5rem;
    font-size: 0.78rem;
  }
  .wc-foot .badge {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .leave {
    display: flex;
    flex-direction: column;
    gap: 0.05rem;
    padding-top: 0.35rem;
    border-top: 1px solid var(--line);
  }
  .leave-entry {
    font-size: 0.92rem;
  }
  .section {
    display: flex;
    flex-direction: column;
    gap: 0.08rem;
  }
  .entry {
    position: relative;
    display: flex;
    align-items: center;
    gap: 0.7rem;
    width: 100%;
    padding: 0.5rem 0.75rem;
    border: 0;
    border-radius: var(--radius-m);
    background: none;
    color: var(--text-muted);
    font: inherit;
    font-weight: 550;
    text-align: left;
    cursor: pointer;
  }
  .entry:hover:not(:disabled) {
    background: var(--raised);
    color: var(--text);
  }
  /* The current page: a filled pill of the accent. */
  .entry.current {
    background: var(--accent);
    color: var(--on-accent);
    box-shadow: 0 6px 20px -8px var(--accent);
  }
  .entry.current:hover {
    background: var(--accent);
    color: var(--on-accent);
    box-shadow: var(--glow);
  }
  .entry:disabled {
    opacity: 0.45;
    cursor: default;
  }
  .soon {
    color: var(--text-faint);
  }
  .pending {
    flex: none;
    width: 0.5rem;
    height: 0.5rem;
    border-radius: 50%;
    background: var(--warning);
  }
  .foot {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    padding-top: 0.5rem;
    border-top: 1px solid var(--line);
    width: 100%;
  }
  .row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.2rem;
  }
  /* A− and A+ beside the switch of the mode: narrow, so that both words of the switch fit. */
  .size {
    padding: 0 0.4rem;
  }
  .scrim {
    display: none;
  }
  .text {
    display: flex;
    flex-direction: column;
    flex: 1;
    min-width: 0;
  }
  /* Simple mode: what each page is for, under its name. */
  .entry.described {
    padding-block: 0.4rem;
  }
  .what {
    display: block;
    margin-top: 0.05rem;
    font-size: 0.74rem;
    font-weight: 450;
    line-height: 1.25;
    color: var(--text-faint);
    white-space: normal;
  }
  .entry.current .what {
    color: inherit;
    opacity: 0.85;
  }
  .caption.gap {
    padding: 0.45rem 0 0;
  }
  .fold {
    color: var(--text-faint);
    font-size: 0.88rem;
  }
  /* Folded into a rail of icons (a wide window only: a narrow one keeps the drawer, whole). */
  @media (min-width: 761px) {
    .sidebar {
      transition: width 140ms ease-out;
    }
    .sidebar.collapsed {
      --side-pad-r: 0.45rem;
      width: 4.1rem;
      padding-inline: 0.45rem;
      align-items: stretch;
    }
    .collapsed .entry {
      justify-content: center;
      gap: 0;
      padding: 0.6rem 0;
    }
    .collapsed .grow,
    .collapsed .text,
    .collapsed .wide,
    .collapsed .wc-name,
    .collapsed .wc-foot {
      display: none;
    }
    /* A section is a line between the icons. */
    .collapsed .caption {
      height: 0;
      margin: 0.4rem 0.35rem;
      padding: 0;
      overflow: hidden;
      border-top: 1px solid var(--line);
    }
    .collapsed .wallet-card {
      align-items: center;
      padding: 0.45rem 0;
    }
    .collapsed .pending {
      position: absolute;
      top: 0.35rem;
      right: 0.55rem;
    }
    .collapsed .scroll {
      overflow-x: hidden;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .sidebar {
      transition: none;
    }
  }
  @media (max-width: 760px) {
    .fold {
      display: none;
    }
    .sidebar {
      position: fixed;
      z-index: 36;
      top: 0;
      bottom: 0;
      left: 0;
      width: min(17rem, 85vw);
      transform: translateX(-105%);
      transition: transform 160ms ease-out;
      box-shadow: var(--shadow);
    }
    .sidebar.open {
      transform: none;
    }
    .scrim {
      display: block;
      position: fixed;
      inset: 0;
      z-index: 35;
      border: 0;
      background: var(--overlay);
    }
  }
</style>
