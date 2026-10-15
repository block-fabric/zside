<script lang="ts">
  // The node window (rpcconsole.cpp): information, console, network traffic, peers.
  import { t } from '../lib/i18n.svelte';
  import SafeText from '../components/SafeText.svelte';
  import ConsoleView from '../components/ConsoleView.svelte';
  import PeersView from '../components/PeersView.svelte';
  import TrafficGraph from '../components/TrafficGraph.svelte';
  import { errorText, inform } from '../lib/dialogs.svelte';
  import { formatBytes, formatCount, formatTime } from '../lib/format';
  import { nav } from '../lib/nav.svelte';
  import { host, rpc, type BlockchainInfo, type NetworkInfo } from '../lib/rpc';
  import { app, node, wallets } from '../lib/state.svelte';
  import { walletDisplayName } from '../lib/wallet';

  const TABS = [
    { id: 'info', label: 'Information', key: 'Ctrl+I' },
    { id: 'console', label: 'Console', key: 'Ctrl+T' },
    { id: 'traffic', label: 'Network traffic', key: 'Ctrl+N' },
    { id: 'peers', label: 'Peers', key: 'Ctrl+P' },
  ];
  const tab = $derived(TABS.some((t) => t.id === nav.tab) ? nav.tab : 'info');

  let chain: BlockchainInfo | null = $state(null);
  let net: NetworkInfo | null = $state(null);
  let startup: number | null = $state(null);
  let paths: { datadir: string; blocksdir: string; debuglog: string; config: string } | null = $state(null);

  $effect(() => {
    void node.tip?.height;
    void node.connections;
    if (tab !== 'info') return;
    Promise.all([rpc.call('getblockchaininfo', []), rpc.call('getnetworkinfo', []), rpc.call('uptime', [])])
      .then(([c, n, up]) => {
        chain = c;
        net = n;
        startup = Math.floor(Date.now() / 1000) - up;
      })
      .catch(() => undefined);
  });
  $effect(() => {
    host.paths().then((p) => (paths = p)).catch(() => undefined);
  });

  async function openLog() {
    try {
      await host.open('debuglog');
    } catch (e) {
      await inform(t('Debug log file'), errorText(e), 'warning');
    }
  }

  const warnings = $derived(node.warnings.filter(Boolean));
</script>

<div class="page">
  <header class="page-head"><h1>Node window</h1></header>
  <div class="tabs" role="tablist" aria-label="Node window">
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} title={tb.key} onclick={() => (nav.tab = tb.id)}>{t(tb.label)}</button>
    {/each}
  </div>
  {#if warnings.length}
    <p class="alert" role="alert">{warnings.join(' ')}</p>
  {/if}

  {#if tab === 'info'}
    <div class="grid">
      <section class="panel">
        <div class="panel-head"><h2>General</h2></div>
        <dl class="facts panel-body">
          <dt>Client version</dt><dd>{app.info?.version}</dd>
          <dt>User agent</dt><dd class="mono"><SafeText text={net?.subversion ?? '–'} /></dd>
          <dt>Datadir</dt><dd class="mono">{paths?.datadir ?? app.info?.datadir}</dd>
          <dt>Blocksdir</dt><dd class="mono">{paths?.blocksdir ?? '–'}</dd>
          <dt>Startup time</dt><dd>{startup ? formatTime(startup) : '–'}</dd>
          <dt>Wallet</dt><dd>{#if wallets.current === null}None{:else}<SafeText text={walletDisplayName(wallets.current)} />{/if}</dd>
        </dl>
      </section>
      <section class="panel">
        <div class="panel-head"><h2>Network</h2></div>
        <dl class="facts panel-body">
          <dt>Name</dt><dd>{chain?.chain ?? app.info?.network}</dd>
          <dt>Number of connections</dt>
          <dd>{net ? `${net.connections} (In: ${net.connections_in} / Out: ${net.connections_out})` : node.connections}</dd>
          <dt>Network activity</dt><dd>{node.networkActive ? 'Enabled' : 'Disabled'}</dd>
          <dt>Local addresses</dt><dd class="mono">{net?.localaddresses.length ? net.localaddresses.map((a) => `${a.address}:${a.port}`).join(', ') : 'None'}</dd>
        </dl>
      </section>
      <section class="panel">
        <div class="panel-head"><h2>Block chain</h2></div>
        <dl class="facts panel-body">
          <dt>Current block height</dt><dd class="num">{formatCount(chain?.blocks ?? node.tip?.height ?? 0)}</dd>
          <dt>Last block time</dt><dd>{formatTime(chain?.time ?? node.tip?.time ?? 0)}</dd>
          <dt>Headers</dt><dd class="num">{formatCount(chain?.headers ?? 0)}</dd>
          <dt>Size on disk</dt><dd>{chain ? formatBytes(chain.size_on_disk) : '–'}{chain?.pruned ? ' (pruned)' : ''}</dd>
        </dl>
      </section>
      <section class="panel">
        <div class="panel-head"><h2>Memory pool</h2></div>
        <dl class="facts panel-body">
          <dt>Current number of transactions</dt><dd class="num">{formatCount(node.mempool.size)}</dd>
          <dt>Memory usage</dt><dd>{formatBytes(node.mempool.usage)}{node.mempool.max ? ` (${formatBytes(node.mempool.max)} max)` : ''}</dd>
        </dl>
      </section>
      <section class="panel">
        <div class="panel-head"><h2>Debug log file</h2></div>
        <div class="panel-body">
          <p class="muted">Open the debug log file of the current data directory. This can take a few seconds for large log files.</p>
          <p class="mono break small">{paths?.debuglog ?? ''}</p>
          <button type="button" class="btn" onclick={openLog}>Open</button>
        </div>
      </section>
    </div>
  {:else if tab === 'console'}
    <ConsoleView />
  {:else if tab === 'traffic'}
    <TrafficGraph />
  {:else}
    <PeersView />
  {/if}
</div>

<style>
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(24rem, 100%), 1fr));
    gap: 1rem;
  }
  .alert {
    max-width: none;
    padding: 0.6rem 0.9rem;
    border-radius: var(--radius-s);
    background: var(--warning-soft);
    color: var(--warning);
    font-weight: 550;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
