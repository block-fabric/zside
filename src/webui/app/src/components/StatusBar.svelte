<script lang="ts">
  import { every } from '../lib/poll';
  import SafeText from './SafeText.svelte';
  import { CHAIN } from '../chain/config';
  // The bar at the bottom: sync, the chain as it grows, peers, the unit of amounts, the lock of the wallet.
  import { t } from '../lib/i18n.svelte';
  import Icon from './Icon.svelte';
  import { css } from '../lib/actions';
  import { formatCount, formatTime, shortHash } from '../lib/format';
  import { host, rpc } from '../lib/rpc';
  import { app, node, prefs, savePrefs, statusTip, toast, wallets } from '../lib/state.svelte';
  import { syncStatus } from '../lib/sync';
  import { UNITS, type Unit } from '../lib/units';
  import { go, nav } from '../lib/nav.svelte';
  import { mainLink } from '../lib/mainlink.svelte';

  const mainName = CHAIN.mainchain?.name ?? '';

  let now = $state(Math.floor(Date.now() / 1000));
  $effect(() => {
    return every(5000, () => (now = Math.floor(Date.now() / 1000)));
  });

  const status = $derived(
    node.tip
      ? syncStatus({
          tipHeight: node.tip.height,
          tipTime: node.tip.time,
          progress: node.tip.progress,
          loadingBlocks: node.tip.loadingBlocks,
          headerHeight: node.header?.height ?? node.tip.height,
          headerTime: node.header?.time ?? node.tip.time,
          presyncHeight: node.presync?.height,
          presyncTime: node.presync?.time,
          connections: node.connections,
          networkActive: node.networkActive,
          spacing: app.info?.blockSpacing ?? 60,
          now,
        })
      : null,
  );
  const wallet = $derived(wallets.list.find((w) => w.name === wallets.current) ?? null);
  const walletProgress = $derived(wallets.current ? wallets.progress[wallets.current] : undefined);
  const busy = $derived(app.progress ?? walletProgress ?? null);

  let netMenu = $state(false);

  // The proxy the node uses to reach IPv4 peers (BitcoinGUI::updateProxyIcon), checked again when the peers change.
  let proxy = $state('');
  $effect(() => {
    void node.connections;
    void node.networkActive;
    rpc
      .call('getnetworkinfo', [])
      .then((n) => {
        const ipv4 = (n as unknown as { networks?: { name: string; proxy: string }[] }).networks?.find((x) => x.name === 'ipv4');
        proxy = ipv4?.proxy ?? '';
      })
      .catch(() => undefined);
  });

  async function toggleNetwork() {
    netMenu = false;
    try {
      await host.setNetworkActive(!node.networkActive);
    } catch (e) {
      toast('error', t('Could not change network activity'), e instanceof Error ? e.message : String(e));
    }
  }
</script>

<footer class="statusbar">
  <!-- svelte-ignore a11y_click_events_have_key_events, a11y_no_static_element_interactions -->
  <div class="sync" title={status?.detail} onclick={() => { if (status && !status.upToDate) nav.dialog = 'sync'; }}>
    {#if statusTip.text}
      <!-- The status tip of the menu item under the pointer (QStatusBar::showMessage of a status tip). -->
      <span class="label tip" role="status">{t(statusTip.text)}</span>
    {:else if busy}
      <span class="label"><SafeText text={busy.title} /></span>
      <progress max="100" value={busy.progress} aria-label={busy.title}></progress>
    {:else if mainLink.banner}
      <!-- The link to the mainchain node (the banner at the top): this chain cannot sync without it. -->
      <button type="button" class="mainlink {mainLink.banner}" title={t('Show how to fix it')} onclick={(e) => { e.stopPropagation(); mainLink.expanded = true; }}>
        <Icon name={mainLink.banner === 'down' ? 'offline' : 'history'} size={16} />
        <span class="label">{mainLink.banner === 'down' ? t('No link to %1', mainName) : t('%1 is still syncing', mainName)}</span>
      </button>
    {:else if status && !status.upToDate}
      <Icon name="history" size={16} />
      <span class="label"><SafeText text={status.label} /></span>
      <progress max="1" value={status.progress} aria-label="Verification progress"></progress>
      <span class="behind num">{status.behind}</span>
    {:else if status}
      <Icon name="check" size={16} />
      <span class="label ok">Up to date</span>
    {:else}
      <span class="label">Starting…</span>
    {/if}
  </div>

  {#if node.recent.length}
    <ol class="chain" aria-label="Latest blocks">
      {#each node.recent as block, i (block.hash)}
        <li
          class="link"
          class:newest={i === node.recent.length - 1}
          title={`Block ${block.height}\n${shortHash(block.hash, 10)}\n${formatTime(block.time)}`}
          use:css={{ opacity: 0.35 + (0.65 * (i + 1)) / node.recent.length }}
        >
          {#if i === node.recent.length - 1}<span class="num">{formatCount(block.height)}</span>{/if}
        </li>
      {/each}
    </ol>
  {/if}

  <div class="spacer"></div>

  {#if node.warnings.length}
    <span class="badge warning" title={node.warnings.join('\n')}><Icon name="alert" size={14} /> Warning</span>
  {/if}
  {#if proxy}
    <span class="proxy" id="proxy-status" title="Proxy is enabled: {proxy}" aria-label="Proxy is enabled: {proxy}"><Icon name="shield" size={15} /></span>
  {/if}

  <div class="net">
    <button
      type="button"
      class="btn ghost small"
      aria-haspopup="menu"
      aria-expanded={netMenu}
      title={node.networkActive ? `${node.connections} active connection${node.connections === 1 ? '' : 's'} to the ${CHAIN.name} network. Click for more actions.` : 'Network activity disabled. Click for more actions.'}
      onclick={() => (netMenu = !netMenu)}
    >
      <Icon name={node.networkActive ? 'peers' : 'offline'} size={16} />
      <span class="num">{node.networkActive ? node.connections : 'Off'}</span>
    </button>
    {#if netMenu}
      <div class="popup" role="menu">
        <button type="button" role="menuitem" class="item" onclick={() => { netMenu = false; go('node', 'peers'); }}>Show peers</button>
        <button type="button" role="menuitem" class="item" onclick={toggleNetwork}>{node.networkActive ? 'Disable network activity' : 'Enable network activity'}</button>
      </div>
    {/if}
  </div>

  <label class="unit">
    <span class="sr-only">Unit to show amounts in</span>
    <select value={prefs.unit} title="Unit to show amounts in" onchange={(e) => savePrefs({ unit: (e.currentTarget as HTMLSelectElement).value as Unit })}>
      {#each UNITS as u (u.id)}
        <option value={u.id}>{u.short}</option>
      {/each}
    </select>
  </label>

  {#if wallet}
    {#if !wallet.privateKeysDisabled}
      <span class="hd" class:off={!wallet.hd} title={wallet.hd ? 'HD key generation is enabled' : 'HD key generation is disabled'} aria-label={wallet.hd ? 'HD key generation is enabled' : 'HD key generation is disabled'}>HD</span>
    {/if}
    {#if wallet.encrypted}
      <span class="lock" title={wallet.locked ? 'Wallet is encrypted and currently locked' : 'Wallet is encrypted and currently unlocked'}>
        <Icon name={wallet.locked ? 'lock' : 'unlock'} size={16} label={wallet.locked ? 'Wallet locked' : 'Wallet unlocked'} />
      </span>
    {/if}
    {#if wallet.privateKeysDisabled}
      <span class="badge" title="This wallet has no private keys: it watches only">Watch-only</span>
    {/if}
  {/if}
</footer>

<style>
  .statusbar {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.2rem 0.9rem;
    min-height: 2.4rem;
    padding: 0.15rem 0.6rem 0.15rem 0.9rem;
    border-top: 1px solid var(--line);
    background: color-mix(in srgb, var(--bg) 75%, transparent);
    color: var(--text-muted);
    font-size: 0.86rem;
  }
  .proxy {
    display: inline-flex;
    align-items: center;
    color: var(--accent-fg);
  }
  .sync {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    min-width: 0;
  }
  .label {
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .label.ok {
    color: var(--positive);
  }
  progress {
    width: 9rem;
    height: 0.45rem;
    appearance: none;
    border: 0;
    border-radius: 999px;
    background: var(--raised);
    overflow: hidden;
  }
  progress::-webkit-progress-bar {
    background: var(--raised);
  }
  progress::-webkit-progress-value {
    background: var(--accent);
  }
  .behind {
    white-space: nowrap;
  }
  /* The chain: the latest blocks as links, the newest with its height. */
  .chain {
    display: flex;
    align-items: center;
    margin: 0;
    padding: 0;
    list-style: none;
  }
  .link {
    position: relative;
    width: 0.8rem;
    height: 0.8rem;
    border: 2px solid var(--accent);
    border-radius: 3px;
    margin-left: 0.55rem;
  }
  .link::before {
    content: '';
    position: absolute;
    left: calc(-0.55rem - 2px);
    top: 50%;
    width: 0.55rem;
    height: 2px;
    background: var(--chain-link);
  }
  .link:first-child {
    margin-left: 0;
  }
  .link:first-child::before {
    display: none;
  }
  .link.newest {
    width: auto;
    height: 1.35rem;
    padding: 0 0.45rem;
    display: flex;
    align-items: center;
    background: var(--accent-soft);
    color: var(--text);
    font-weight: 600;
    animation: arrive 900ms ease-out;
  }
  @keyframes arrive {
    from {
      transform: translateX(-0.5rem) scale(0.9);
      box-shadow: 0 0 0 0.35rem var(--accent-soft);
    }
  }
  .spacer {
    flex: 1;
  }
  .net {
    position: relative;
  }
  .popup {
    position: absolute;
    right: 0;
    bottom: calc(100% + 0.3rem);
    z-index: 30;
    min-width: 14rem;
    padding: 0.3rem;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-m);
    background: var(--surface);
    box-shadow: var(--shadow);
  }
  .item {
    display: block;
    width: 100%;
    padding: 0.4rem 0.6rem;
    border: 0;
    border-radius: var(--radius-s);
    background: none;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .item:hover,
  .item:focus-visible {
    background: var(--accent-soft);
  }
  .unit select {
    min-height: 1.8rem;
    padding: 0 1.7rem 0 0.55rem;
    border-color: var(--line);
    background-color: var(--surface-2);
    background-position: right 0.45rem center;
    font-size: 0.86rem;
  }
  .hd {
    padding: 0 0.3rem;
    border: 1px solid currentColor;
    border-radius: 4px;
    font-size: 0.72rem;
    font-weight: 700;
    color: var(--accent-fg);
  }
  .hd.off {
    color: var(--text-faint);
    text-decoration: line-through;
  }
  .lock {
    display: inline-flex;
    color: var(--text-muted);
  }
  /* A narrow window: everything stays, on two lines if it must (the height of the chain, how far behind). */
  @media (max-width: 760px) {
    progress {
      width: 5rem;
    }
    .statusbar {
      gap: 0.2rem 0.6rem;
    }
  }
  .mainlink {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    padding: 0;
    border: 0;
    background: none;
    font: inherit;
    cursor: pointer;
  }
  .mainlink.down {
    color: var(--negative);
  }
  .mainlink.syncing {
    color: var(--warning);
  }
</style>
