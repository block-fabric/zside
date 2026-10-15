<script lang="ts">
  // The dashboard's navigation on the left: every page, in sections, names and icons, with Home
  // first; on a narrow window a drawer (open). At its top a card of the wallet shown (its name, its
  // lock, the unit); the pages in groups; at its foot the theme, the text
  // size, then Close wallet and Quit, apart. The current page is a filled pill.
  import { t } from '../lib/i18n.svelte';
  import { CHAIN } from '../chain/config';
  import Brand from './Brand.svelte';
  import Icon from './Icon.svelte';
  import MenuButton from './MenuButton.svelte';
  import WalletPicker from './WalletPicker.svelte';
  import { buildCommands, pageAvailable, pageDisabled } from '../lib/commands.svelte';
  import { NAV_SECTIONS, pageIcon } from '../lib/layout';
  import { go, nav, PAGES, type PageId } from '../lib/nav.svelte';
  import { prefs, savePrefs, statusTip, wallets } from '../lib/state.svelte';
  import { pendingNow } from '../lib/settingsdraft.svelte';
  import { clampFontAdjust, THEMES } from '../lib/themes';
  import { isThemeChoice } from '../lib/customthemes';
  import { frame } from '../lib/windowframe.svelte';

  let { open = $bindable(false) }: { open?: boolean } = $props();

  const sections = $derived([
    { title: '', pages: ['home'] as PageId[] },
    ...NAV_SECTIONS.map((s) => ({ title: s.title, pages: s.pages.filter(pageAvailable) })),
  ]);
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

<nav class="sidebar" class:open aria-label="Pages">
  {#if !frame.integrated}
    <!-- The name of the app is in the integrated title bar, when there is one. -->
    <Brand onclick={() => visit('home')} />
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
      {#if lock}<div class="wc-foot"><span class="badge {lock.tone}" title={t(lock.title)}><Icon name={lock.icon} size={12} /> {t(lock.label)}</span></div>{/if}
    </section>
  {/if}

  <div class="scroll">
    {#each sections as section (section.title)}
      <div class="section" role="group" aria-label={t(section.title || 'Home')}>
        {#if section.title}<div class="caption caps">{t(section.title)}</div>{/if}
        {#each section.pages as id (id)}
          {@const page = PAGES[id]}
          <button
            type="button"
            class="entry"
            class:current={nav.page === id}
            aria-current={nav.page === id ? 'page' : undefined}
            disabled={pageDisabled(id)}
            title={page.summary}
            onclick={() => visit(id)}
            onpointerenter={() => (statusTip.text = page.summary)}
            onpointerleave={() => (statusTip.text = '')}
          >
            <Icon name={pageIcon(id)} size={19} />
            <span class="grow">{t(page.title)}</span>
            {#if !page.ready}<span class="soon" title="Being rebuilt">•</span>{/if}
            {#if id === 'settings' && pendingNow() > 0}<span class="pending" title="Changes of the settings wait for Apply" aria-label="Changes wait for Apply"></span>{/if}
          </button>
        {/each}
      </div>
    {/each}
  </div>

  <div class="foot">
    <div class="row">
      <select class="look" aria-label="Theme" title="Theme" value={prefs.theme} onchange={(e) => { const v = (e.currentTarget as HTMLSelectElement).value; if (isThemeChoice(v, prefs.customThemes)) savePrefs({ theme: v }); }}>
        {#each THEMES as theme (theme.id)}
          <option value={theme.id}>{t(theme.name)}</option>
        {/each}
        {#if prefs.customThemes.length > 0}
          <optgroup label="Your themes">
            {#each prefs.customThemes as theme (theme.id)}
              <option value={theme.id} translate="no">{theme.name}</option>
            {/each}
          </optgroup>
        {/if}
      </select>
    </div>
    <div class="row">
      <button type="button" class="btn ghost small" title="Smaller text (Ctrl+-)" aria-label="Smaller text" onclick={() => savePrefs({ fontAdjust: clampFontAdjust(prefs.fontAdjust - 1) })}>A−</button>
      <button type="button" class="btn ghost small" title="Larger text (Ctrl++)" aria-label="Larger text" onclick={() => savePrefs({ fontAdjust: clampFontAdjust(prefs.fontAdjust + 1) })}>A+</button>
    </div>
    <div class="leave">
      {#each leave as c (c.id)}
        <button
          type="button"
          class="entry leave-entry"
          disabled={c.disabled}
          title={c.shortcut ? `${c.label} (${c.shortcut})` : c.label}
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
    width: 15rem;
    flex: none;
    padding: 0.7rem 0.6rem 0.6rem;
    min-height: 0;
  }
  .scroll {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    width: 100%;
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
  .look {
    /* As wide as its longest theme ("Matrix Green", "Classic Dark"), on the next line when the row has no room
       for it beside the menus (audit port-1 #11: it was cut to "Matrix Gr"). */
    flex: 1 0 auto;
    min-width: 0;
    max-width: 100%;
    width: auto;
    min-height: 1.9rem;
    padding-block: 0.15rem;
    font-size: 0.88rem;
  }
  .row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.2rem;
  }
  .scrim {
    display: none;
  }
  @media (max-width: 760px) {
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
