<script lang="ts">
  // The banner at the top of every page of a sidechain when its node cannot follow the mainchain node (down),
  // or that node is still syncing (softer), for more than a few seconds (lib/mainlink.svelte.ts decides):
  // why in plain words, since when, and how to fix it. It goes by itself when the link is back.
  import { t, tn } from '../lib/i18n.svelte';
  import { CHAIN } from '../chain/config';
  import Icon from './Icon.svelte';
  import SafeText from './SafeText.svelte';
  import { niceTimeOffset } from '../lib/format';
  import { mainLink, reasonText } from '../lib/mainlink.svelte';
  import { go } from '../lib/nav.svelte';
  import { app } from '../lib/state.svelte';
  import { simpleMode } from '../lib/uimode.svelte';

  const mainName = CHAIN.mainchain?.name ?? '';
  let now = $state(Date.now());
  $effect(() => {
    if (!mainLink.banner) return;
    now = Date.now();
    const timer = setInterval(() => (now = Date.now()), 10_000);
    return () => clearInterval(timer);
  });

  const info = $derived(mainLink.info);
  const kind = $derived(mainLink.banner === 'syncing' ? 'not-synced' : (info?.error_kind ?? 'other'));
  const simple = $derived(simpleMode());
  const lastOk = $derived(info?.last_ok ? niceTimeOffset(now / 1000 - info.last_ok) : '');
  const behind = $derived(info?.behind_blocks ?? 0);
  /** The RPC port of the mainchain node on this network, by default. */
  const mainNetworks = CHAIN.mainchain?.networks as Record<string, { rpc?: number } | undefined> | undefined;
  const defaultPort = $derived(mainNetworks?.[app.info?.network ?? '']?.rpc ?? 0);
  const detailsId = 'mainchain-alert-fix';

  function settings() {
    go('settings', 'mainchain');
  }
</script>

{#if mainLink.banner}
  <div class="mainchain-alert {mainLink.banner}" role={mainLink.banner === 'down' ? 'alert' : 'status'} data-kind={kind}>
    <div class="row">
      <span class="ic" aria-hidden="true"><Icon name={mainLink.banner === 'down' ? 'offline' : 'history'} size={18} /></span>
      <div class="words">
        {#if mainLink.banner === 'down'}
          {#if simple}
            <strong class="title">{t('Cannot reach %1', mainName)}</strong>
            <span class="text">{t('This wallet is paused until %1 is back.', mainName)}</span>
          {:else}
            <strong class="title">{t('Cannot reach your %1 node (%2): %3', mainName, info?.node ?? '', reasonText(info?.error_kind))}</strong>
            <span class="text">
              {t('This wallet cannot sync, nor confirm deposits and withdrawals, until it is back.')}
              {#if lastOk}{t('Last reached %1 ago.', lastOk)}{:else}{t('Not reached since this app started.')}{/if}
            </span>
          {/if}
        {:else if simple}
          <strong class="title">{t('%1 is still catching up', mainName)}</strong>
          <span class="text">{t('This wallet waits for it.')}</span>
        {:else}
          <strong class="title">{behind > 0 ? t('%1 is still syncing: %2 behind', mainName, tn('%n block(s)', behind)) : t('%1 is still syncing', mainName)}</strong>
          <span class="text">{t('New blocks, deposits and withdrawals wait until it catches up.')}</span>
        {/if}
      </div>
      <button type="button" class="btn ghost small fix" aria-expanded={mainLink.expanded} aria-controls={detailsId} onclick={() => (mainLink.expanded = !mainLink.expanded)}>
        {t('How to fix')}
        <span class="chev" class:open={mainLink.expanded}><Icon name="chevron" size={14} /></span>
      </button>
    </div>
    {#if mainLink.expanded}
      <div class="fix-body" id={detailsId}>
        <ul>
          {#if kind === 'auth'}
            <li>{t('The user name and password (or the cookie file) set here must be those of %1: its rpcuser and rpcpassword, or its .cookie file.', mainName)}</li>
            <li>{t('If %1 runs on another computer, its rpcallowip must allow this one.', mainName)}</li>
          {:else if kind === 'wrong-network'}
            <li>{t('%1 runs on %2 and this wallet on %3: start it on the same network, or set the port of the right one.', mainName, info?.mainchain_network ?? '?', app.info?.network ?? '?')}</li>
          {:else if kind === 'not-synced'}
            <li>{t('Leave %1 running: it catches up by itself, and this wallet follows as soon as it has.', mainName)}</li>
            <li>{t('If it does not move, check that %1 has peers.', mainName)}</li>
          {:else}
            <li>{t('Is %1 running? Start it (the %1 app, or its node) and leave it running.', mainName)}</li>
            <li>{t('Its RPC server must be on: server=1 in its configuration file, or start it with -server.')}</li>
            <li>{t('The address and port set here must be those of its RPC server (by default port %1 on this network).', defaultPort)}</li>
            {#if kind === 'timeout'}<li>{t('It may be busy (starting, reindexing): give it a minute.')}</li>{/if}
          {/if}
        </ul>
        {#if !simple && info?.error}<p class="raw"><SafeText text={info.error} /></p>{/if}
        <button type="button" class="btn small" onclick={settings}><Icon name="settings" size={14} /> {t('Open the mainchain settings')}</button>
      </div>
    {/if}
  </div>
{/if}

<style>
  .mainchain-alert {
    flex: none;
    padding: 0.5rem 1rem;
    border-bottom: 1px solid var(--line-strong);
    color: var(--text);
    animation: alert-in 0.2s ease-out;
  }
  .mainchain-alert.down {
    background: var(--danger-soft);
    border-bottom-color: color-mix(in srgb, var(--negative) 45%, transparent);
  }
  .mainchain-alert.syncing {
    background: var(--warning-soft);
    border-bottom-color: color-mix(in srgb, var(--warning) 40%, transparent);
  }
  .row {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    min-height: 2.25rem;
  }
  .ic {
    display: inline-grid;
    flex: none;
    place-items: center;
  }
  .down .ic,
  .down .title {
    color: var(--negative);
  }
  .syncing .ic,
  .syncing .title {
    color: var(--warning);
  }
  .words {
    display: flex;
    flex: 1;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 0.15rem 0.6rem;
    min-width: 0;
  }
  .title {
    font-weight: 650;
    overflow-wrap: anywhere;
  }
  .text {
    color: var(--text-muted);
    font-size: 0.9rem;
  }
  .fix {
    flex: none;
    white-space: nowrap;
  }
  .chev {
    display: inline-grid;
    transition: transform 0.15s;
  }
  .chev.open {
    transform: rotate(180deg);
  }
  .fix-body {
    padding: 0.35rem 0 0.25rem 2.05rem;
    font-size: 0.9rem;
  }
  .fix-body ul {
    margin: 0 0 0.5rem;
    padding-left: 1.1rem;
  }
  .fix-body li {
    margin: 0.15rem 0;
  }
  .raw {
    margin: 0 0 0.5rem;
    color: var(--text-faint);
    font-size: 0.82rem;
    overflow-wrap: anywhere;
  }
  @keyframes alert-in {
    from {
      opacity: 0;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .mainchain-alert,
    .chev {
      animation: none;
      transition: none;
    }
  }
</style>
