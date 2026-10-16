<script lang="ts">
  // The home of the dashboard layout on a sidechain (Home.svelte is the mainchain's): a row of summary
  // cards (the balance, the network, the peg, merged mining), each opening its full page, with its
  // actions; the balance over time with its timeframes; the blocks of the mainchain that committed to
  // this chain; then tables of the recent transactions, the latest blocks and the peers. A row opens
  // the detail panel; Send and Receive open the side panel (components/QuickPanel.svelte).
  import { t } from '../../lib/i18n.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { untrack } from 'svelte';
  import Amount from '../../components/Amount.svelte';
  import BalanceChart from '../../components/BalanceChart.svelte';
  import Icon, { type IconName } from '../../components/Icon.svelte';
  import Sparkline from '../../components/Sparkline.svelte';
  import Card from './Card.svelte';
  import HomeTables from './HomeTables.svelte';
  import LocalCards from '../../chain/LocalCards.svelte';
  import { HOME_QUICK_ACTIONS } from '../../lib/layout';
  import { openPalette, pageDisabled } from '../../lib/commands.svelte';
  import { formatCount, niceTimeOffset } from '../../lib/format';
  import { go, type PageId } from '../../lib/nav.svelte';
  import { host, rpc } from '../../lib/rpc';
  import { app, node, noWalletText, prefs, toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import { syncStatus } from '../../lib/sync';
  import { every } from '../../lib/poll';
  import { coinsToSats, formatAmount, unitInfo } from '../../lib/units';
  import { openQuick } from '../../lib/quick.svelte';
  import { balanceHistory } from '../../lib/wallet';
  import { current, refreshRecords } from '../../lib/walletdata.svelte';
  import { CHAIN } from '../../chain/config';
  import { bmmBlocks, bmmShare, depositsOf } from '../../lib/side';
  import { refreshSide, side, startSide } from '../../lib/sidestate.svelte';
  import FirstRunTips from '../../components/FirstRunTips.svelte';
  import Term from '../../components/Term.svelte';
  import { pageShown, simpleMode } from '../../lib/uimode.svelte';

  interface Peer {
    id: number;
    addr: string;
    inbound: boolean;
    pingtime?: number;
    synced_blocks: number;
  }
  interface MiningInfo {
    blocks: number;
    difficulty: number;
    networkhashps: number;
    pooledtx: number;
  }
  interface BlockRow {
    height: number;
    hash: string;
    time: number;
    txs: number;
  }

  /** Blocks of the mainchain looked at for blind merged mining. */
  const bmmWindowSize = 24;
  const mainName = CHAIN.mainchain?.name ?? '';

  let now = $state(Math.floor(Date.now() / 1000));
  let peers: Peer[] = $state([]);
  let blocks: BlockRow[] = $state([]);
  let mining: MiningInfo | null = $state(null);

  // --- The peg and merged mining (lib/sidestate.svelte.ts) ---
  $effect(() => {
    startSide();
    void refreshSide(true);
  });
  const info = $derived(side.info);
  const connected = $derived(!!info?.connected);
  const pendingDeposits = $derived(depositsOf(side.main.events, info?.tipheight ?? -1).filter((d) => !d.credited));
  const voted = $derived(side.bundle?.hash ? side.main.bundles.find((b) => b.hash === side.bundle?.hash) : undefined);
  const minScore = $derived(side.main.drivechain?.withdrawalminscore ?? 0);
  const share = $derived(bmmShare(side.main.events, bmmWindowSize));
  const marks = $derived(bmmBlocks(side.main.events, bmmWindowSize).reverse());
  /** The blocks of this chain the last mainchain blocks committed to, so far in the window (the sparkline). */
  const series = $derived.by(() => {
    let n = 0;
    return [0, ...marks.map((m) => (n += m.bmm ? 1 : 0))];
  });

  $effect(() => every(5000, () => (now = Math.floor(Date.now() / 1000))));

  // --- The wallet ---
  const balances = $derived(wallets.current !== null ? wallets.balances[wallets.current] : undefined);
  const total = $derived(balances ? balances.trusted + balances.untrustedPending + balances.immature : null);
  $effect(() => {
    void wallets.txVersion;
    void node.tip?.hash;
    if (wallets.current !== null) refreshRecords();
  });
  $effect(() => {
    const name = wallets.current;
    if (name === null || wallets.balances[name]) return;
    rpc
      .call('getbalances', [], { wallet: name })
      .then((b) => {
        if (!wallets.balances[name]) {
          wallets.balances[name] = { trusted: coinsToSats(b.mine.trusted), untrustedPending: coinsToSats(b.mine.untrusted_pending), immature: coinsToSats(b.mine.immature) };
        }
      })
      .catch(() => undefined);
  });
  const recent = $derived([...current.records].sort((a, b) => b.time - a.time || b.sortKey.localeCompare(a.sortKey)).slice(0, 8));
  const history = $derived(balanceHistory(current.records));

  // --- The node ---
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

  $effect(() => {
    void node.connections;
    const load = () =>
      rpc
        .raw<Peer[]>('getpeerinfo')
        .then((list) => (peers = list))
        .catch(() => undefined);
    return every(15000, load, { now: true });
  });

  async function toggleNetwork() {
    try {
      await host.setNetworkActive(!node.networkActive);
    } catch (e) {
      toast('error', t('Could not change network activity'), e instanceof Error ? e.message : String(e));
    }
  }

  // --- The chain, the sidechains, mining: after each new block (once the blocks stop coming fast) ---
  async function loadChain(tipHash: string, signal: AbortSignal) {
    const rows: BlockRow[] = [];
    let hash: string | undefined = tipHash;
    for (let i = 0; i < 6 && hash; i++) {
      const h: { height: number; time: number; nTx: number; previousblockhash?: string } = await rpc.raw('getblockheader', [hash, true], { signal });
      rows.push({ height: h.height, hash, time: h.time, txs: h.nTx });
      hash = h.previousblockhash;
    }
    blocks = rows;
    now = Math.floor(Date.now() / 1000);
    mining = await rpc.raw<MiningInfo>('getmininginfo', [], { signal });
  }

  $effect(() => {
    const tip = node.tip?.hash;
    if (!tip) return;
    const controller = new AbortController();
    const timer = setTimeout(() => loadChain(tip, controller.signal).catch(() => undefined), untrack(() => blocks.length) ? 700 : 0);
    return () => {
      clearTimeout(timer);
      controller.abort();
    };
  });


  // Send and Receive, then what concerns the chain, then the tools (lib/layout.ts).
  // Simple mode leaves out the pages of Advanced (lib/uimode.svelte.ts), merged mining among them.
  const quickLinks = $derived(HOME_QUICK_ACTIONS.filter((q) => pageShown(q.page)));
  const simple = $derived(simpleMode());
  /** Simple mode: the blocks of the mainchain open on demand. */
  let bmmDetails = $state(false);

  // --- The balance over time, in a timeframe ---
  const FRAMES = [
    { id: '1D', label: '1D', name: 'the last day', secs: 86400 },
    { id: '7D', label: '7D', name: 'the last 7 days', secs: 7 * 86400 },
    { id: '1M', label: '1M', name: 'the last month', secs: 30 * 86400 },
    { id: '1Y', label: '1Y', name: 'the last year', secs: 365 * 86400 },
    { id: 'all', label: 'All', name: 'all time', secs: 0 },
  ] as const;
  let frame: (typeof FRAMES)[number]['id'] = $state('all');
  const frameInfo = $derived(FRAMES.find((f) => f.id === frame)!);
  const from = $derived(frameInfo.secs ? now - frameInfo.secs : undefined);
  const balanceAt = (t: number | undefined) => {
    if (t === undefined) return 0n;
    let b = 0n;
    for (const p of history) {
      if (p.time > t) break;
      b = p.balance;
    }
    return b;
  };
  const latest = $derived(history.length ? history[history.length - 1].balance : 0n);
  const change = $derived(latest - balanceAt(from));
  const changePct = $derived.by(() => {
    const before = balanceAt(from);
    if (before <= 0n) return null;
    return (Number(change) / Number(before)) * 100;
  });

  /** A long total takes a smaller figure in its card. */
  const longTotal = $derived(total !== null && formatAmount(total, prefs.unit).length > 12);
  // The characters of the balance with its unit: its size fits them in the card, the unit never cut (audit port-2 #14).
  const totalChars = $derived(total === null ? 1 : formatAmount(total, prefs.unit).length + 1 + unitInfo(prefs.unit).short.length);

</script>

<div class="page home">
  <header class="hero">
    <div>
      <h1>Home</h1>
      <p class="muted sub">
        {#if status}{#if status.upToDate}Up to date{:else}<SafeText text={status.label} />{/if}, block {formatCount(node.tip?.height ?? 0)}, {node.networkActive ? `${node.connections} peer${node.connections === 1 ? '' : 's'}` : 'network off'}{:else}Starting…{/if}
      </p>
    </div>
    <nav class="quick" aria-label="Quick actions">
      {#each quickLinks as q (q.label)}
        <button type="button" class="pill action" disabled={pageDisabled(q.page)} onclick={() => (q.panel ? openQuick(q.panel) : go(q.page, q.tab ?? ''))}><Icon name={q.icon} size={15} /> {t(q.label)}</button>
      {/each}
      <button type="button" class="pill action ghost" onclick={openPalette}><Icon name="search" size={15} /> All actions <kbd>Ctrl K</kbd></button>
    </nav>
  </header>

  <FirstRunTips />

  {#if node.tip?.ibd}
    <button type="button" class="out-of-sync" onclick={() => go('node', 'info')}>
      <Icon name="alert" size={16} /> Out of sync: balances and activity may be out of date until the node has caught up with the network.
    </button>
  {/if}

  {#if info && !connected}
    <button type="button" class="out-of-sync" onclick={() => go('bmm')}>
      <Icon name="alert" size={16} /> {mainName} is not reachable: this node cannot tell which new blocks are valid, nor mine. <SafeText text={info.error ?? ''} />
    </button>
  {/if}

  <div class="stats">
    <Card title="Balance" icon="wallet" open={() => go('overview')} openLabel="Open the overview" stat>
      {#if !walletsEnabled()}
        <p class="muted">{noWalletText()}</p>
      {:else if wallets.current === null}
        <p class="muted">No wallet is loaded. Create one, or open one of the wallets of this node.</p>
      {:else}
        <span class="value big-amount fit" class:long={longTotal} style:--chars={totalChars}><Amount sats={total} /></span>
        <dl class="pairs">
          <dt>Available</dt><dd><Amount sats={balances?.trusted} showUnit={false} /></dd>
          <dt>Pending</dt><dd><Amount sats={balances?.untrustedPending} showUnit={false} /></dd>
          <dt>Immature</dt><dd><Amount sats={balances?.immature} showUnit={false} /></dd>
        </dl>
      {/if}
      {#snippet actions()}
        {#if !walletsEnabled()}
          <!-- No wallets on this node: nothing to create. -->
        {:else if wallets.current === null}
          <button type="button" class="btn primary" onclick={() => go('wallets', 'create')}>Create or open a wallet</button>
        {:else}
          <button type="button" class="btn primary grow" disabled={pageDisabled('send')} onclick={() => openQuick('send')}><Icon name="send" size={16} /> Send</button>
          <button type="button" class="btn grow" disabled={pageDisabled('receive')} onclick={() => openQuick('receive')}><Icon name="receive" size={16} /> Receive</button>
        {/if}
      {/snippet}
    </Card>

    <Card title="Network" icon="peers" open={() => go('node', 'info')} openLabel="Open the node window" stat>
      <span class="value">
        {#if status}
          <span class="state" class:ok={status.upToDate}><Icon name={status.upToDate ? 'check' : 'history'} size={16} /> {#if status.upToDate}Up to date{:else}<SafeText text={status.label} />{/if}</span>
        {:else}<span class="muted">Starting…</span>{/if}
      </span>
      {#if status && !status.upToDate}<progress max="1" value={status.progress} aria-label="Verification progress"></progress>{/if}
      <dl class="pairs">
        <dt>Height</dt><dd class="num">{formatCount(node.tip?.height ?? 0)}</dd>
        <dt>Peers</dt><dd class="num">{node.networkActive ? node.connections : 'network off'}</dd>
        <dt>Sync</dt><dd class="num">{status ? (status.upToDate ? '100%' : `${(status.progress * 100).toFixed(1)}%`) : '–'}</dd>
        <dt>Mempool</dt><dd class="num">{mining ? `${formatCount(mining.pooledtx)} tx` : '–'}</dd>
      </dl>
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('node', 'peers')}><Icon name="peers" size={15} /> Peers</button>
        {#if !simple}<button type="button" class="btn ghost" onclick={() => go('node', 'console')}>Console</button>{/if}
        <button type="button" class="btn ghost icon" aria-label={node.networkActive ? 'Disable network' : 'Enable network'} title={node.networkActive ? 'Disable network activity' : 'Enable network activity'} onclick={toggleNetwork}><Icon name={node.networkActive ? 'offline' : 'refresh'} size={16} /></button>
      {/snippet}
    </Card>

    <Card title="Drivechains" icon="peg" open={() => go('peg')} openLabel="Open Drivechains" stat>
      <span class="value"><span class="state" class:ok={connected} class:bad={info !== null && !connected}><Icon name={connected ? 'check' : 'offline'} size={16} /> {#if info === null}Starting…{:else if connected}{mainName} connected{:else}Not reachable{/if}</span></span>
      {#if simple}
        <dl class="pairs">
          <dt>Coming in</dt><dd class="num">{pendingDeposits.length}</dd>
          <dt>Going back</dt><dd class="num">{side.withdrawals.length}</dd>
        </dl>
      {:else}
      <dl class="pairs">
        <dt>Deposits on their way</dt><dd class="num">{pendingDeposits.length}</dd>
        <dt>Withdrawals waiting</dt><dd class="num">{side.withdrawals.length}</dd>
        <dt>Bundle</dt><dd>{side.bundle?.status === 'pending' ? (voted && minScore ? `${voted.score} / ${minScore} votes` : 'being voted on') : side.bundle?.status === 'next' ? 'next block' : 'none'}</dd>
        <dt>Slot</dt><dd class="num">{info?.slot ?? '–'}</dd>
      </dl>
      {/if}
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('peg', 'deposit')}><Icon name="down" size={15} /> {simple ? 'Bring coins in' : 'Deposit'}</button>
        <button type="button" class="btn ghost" onclick={() => go('peg', 'withdraw')}>{simple ? 'Send back' : 'Withdraw'}</button>
      {/snippet}
    </Card>

    {#if !simple}
    <Card title="Merged mining" icon="merged" open={() => go('bmm')} openLabel="Open merged mining" stat>
      <span class="value"><span class="state" class:ok={!!side.bmm?.mining}><Icon name={side.bmm?.mining ? 'check' : 'power'} size={16} /> {side.bmm ? (side.bmm.mining ? (side.bmm.idle ? 'Waiting for fees' : 'Mining') : 'Off') : '–'}</span></span>
      <dl class="pairs">
        <dt>Blocks mined</dt><dd class="num">{side.bmm ? formatCount(side.bmm.blocks) : '–'}</dd>
        <dt>Requests</dt><dd class="num">{side.bmm ? formatCount(side.bmm.requests) : '–'}</dd>
        <dt title="Blocks of the last {share.of} of {mainName} that committed to this chain">Committed</dt><dd class="num">{share.of ? `${share.committed}/${share.of}` : '–'}</dd>
        <dt>Last block</dt><dd>{blocks[0] ? `${niceTimeOffset(Math.max(0, now - blocks[0].time))} ago` : '–'}</dd>
      </dl>
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('bmm')}><Icon name="merged" size={15} /> {side.bmm?.mining ? 'Mining settings' : 'Start mining'}</button>
      {/snippet}
    </Card>
    {/if}
  </div>

  <LocalCards />

  <div class="middle">
    <section class="panel chart-card" aria-labelledby="chart-title">
      <header class="chart-head">
        <div>
          <h2 id="chart-title">Wallet balance</h2>
          {#if wallets.current !== null}
            <div class="figure">
              <span class="now big-amount"><Amount sats={latest} /></span>
              {#if !prefs.mask && history.length}
                <span class="delta" class:up={change > 0n} class:down={change < 0n}>
                  <Icon name={change < 0n ? 'down' : 'up'} size={13} />
                  <Amount sats={change} plus showUnit={false} />{#if changePct !== null}&nbsp;({changePct >= 0 ? '+' : ''}{changePct.toFixed(2)}%){/if}
                  <span class="faint">{t(frameInfo.name)}</span>
                </span>
              {/if}
            </div>
          {/if}
        </div>
        <div class="frames" role="group" aria-label="Timeframe">
          {#each FRAMES as f (f.id)}
            <button type="button" aria-pressed={frame === f.id} title={t(f.name)} onclick={() => (frame = f.id)}>{t(f.label)}</button>
          {/each}
        </div>
      </header>
      <div class="chart-body">
        {#if wallets.current === null}
          <p class="muted">No wallet is loaded.</p>
        {:else if prefs.mask}
          <p class="muted">Hidden: privacy mode.</p>
        {:else if history.length < 2}
          <p class="muted">The chart starts with the first transaction.</p>
        {:else}
          <BalanceChart points={history} height={210} axes {from} />
        {/if}
      </div>
    </section>

    <Card title="Blocks of {mainName}" icon="merged" open={() => go('bmm')} openLabel="Open merged mining">
      {#if simple && !bmmDetails}
        <!-- Simple mode: one line, the blocks on demand. -->
        <p class="bmm-line" data-testid="bmm-line">
          {#if !connected}{mainName} is not reachable.{:else}This chain is <Term id="bmm">merge-mined</Term> by {mainName}: {share.committed} of its last {share.of || bmmWindowSize} blocks made one of ours.{/if}
        </p>
        <p class="muted small">The miners of {mainName} make the blocks of this chain too, so it needs no miners of its own.</p>
      {:else}
      <p class="muted small">The last {share.of || bmmWindowSize} blocks of {mainName}: those that committed to a block of this chain are lit.</p>
      {#if marks.length === 0}
        <p class="muted">{side.main.error ? side.main.error : connected ? 'No block on record yet.' : `${mainName} is not reachable.`}</p>
      {:else}
        <div class="side-blocks" role="img" aria-label="{share.committed} of the last {share.of} blocks of {mainName} committed to this chain">
          {#each marks as m (m.hash)}<span class:hit={!!m.bmm} class:miss={!m.bmm} title={m.bmm ? `Block ${m.height}: committed` : `Block ${m.height}: no commitment`}></span>{/each}
        </div>
        <div class="trend">
          <span class="num big-amount">{share.committed}<span class="unit-word"> / {share.of}</span></span>
          <Sparkline values={series} width={110} height={34} tone={share.committed ? 'up' : 'flat'} />
        </div>
      {/if}
      {/if}
      {#snippet actions()}
        {#if simple}
          <button type="button" class="btn" aria-expanded={bmmDetails} onclick={() => (bmmDetails = !bmmDetails)}>{bmmDetails ? 'Fewer details' : 'Details'}</button>
        {:else}
          <button type="button" class="btn" onclick={() => go('bmm')}>Merged mining</button>
          <button type="button" class="btn ghost" onclick={() => go('peg', 'bundle')}>Bundle</button>
        {/if}
      {/snippet}
    </Card>
  </div>

  <HomeTables {recent} {history} sidechains={[]} bundles={[]} bmm={{}} bmmSeries={{}} bmmWindow={bmmWindowSize} {blocks} {peers} {now} hide={['sidechains']} />
</div>

<style>
  .bmm-line {
    margin: 0.2rem 0 0.4rem;
    font-size: 1.05rem;
    font-weight: 600;
  }
  .home {
    display: flex;
    flex-direction: column;
    gap: 1.1rem;
    /* The cards follow the room the page has (a side panel takes some), not the window. */
    container-type: inline-size;
  }
  .hero {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-end;
    justify-content: space-between;
    gap: 0.8rem 1.5rem;
  }
  .sub {
    margin: 0.25rem 0 0;
  }
  .quick {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem;
  }
  .action {
    cursor: pointer;
  }
  .action:hover:not(:disabled) {
    background: var(--raised);
    border-color: var(--line-strong);
  }
  .action:disabled {
    opacity: 0.5;
    cursor: default;
  }
  .action :global(svg) {
    color: var(--accent-fg);
  }
  .action.ghost {
    border-color: transparent;
    background: none;
    color: var(--text-muted);
  }
  .action.ghost :global(svg) {
    color: inherit;
  }
  .action kbd {
    margin-left: 0.2rem;
  }
  .out-of-sync {
    display: flex;
    gap: 0.5rem;
    align-items: center;
    padding: 0.6rem 0.9rem;
    border: 1px solid var(--warning);
    border-radius: var(--radius-l);
    background: var(--warning-soft);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .stats {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(14.5rem, 1fr));
    gap: 1rem;
  }
  .value {
    display: block;
    margin: 0.1rem 0 0.55rem;
    font-size: 1.55rem;
    font-weight: 650;
    letter-spacing: -0.02em;
    line-height: 1.2;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  /* The balance: as large as the card lets its characters be (a digit is about 0.62em wide), never cut. */
  .value.fit {
    container-type: inline-size;
    text-overflow: clip;
  }
  .value.fit :global(.amount) {
    font-size: min(1em, calc(100cqi / (var(--chars, 12) * 0.62)));
  }
  .value.long {
    font-size: 1.22rem;
    margin-top: 0.3rem;
    margin-bottom: 0.75rem;
  }
  .unit-word {
    color: var(--text-muted);
    font-size: 0.95rem;
    font-weight: 500;
    letter-spacing: 0;
  }
  .state {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    font-size: 1.25rem;
  }
  .state.ok {
    color: var(--positive);
  }
  .pairs {
    display: grid;
    /* The labels keep their longest word; the values wrap when the card is narrow. */
    grid-template-columns: minmax(min-content, 1fr) minmax(0, auto);
    gap: 0.35rem 1rem;
    margin: 0;
    padding-top: 0.6rem;
    border-top: 1px solid var(--line);
  }
  .pairs dt {
    /* The whole label, on two lines when the card is narrow (audit port-1 #11). */
    color: var(--text-muted);
    font-size: 0.86rem;
    overflow-wrap: break-word;
  }
  .pairs dd {
    min-width: 0;
    overflow-wrap: anywhere;
    margin: 0;
    text-align: right;
    font-weight: 600;
    font-size: 0.92rem;
  }
  progress {
    width: 100%;
    height: 0.35rem;
    margin: 0 0 0.5rem;
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
  .middle {
    display: grid;
    grid-template-columns: minmax(0, 2.1fr) minmax(16rem, 1fr);
    gap: 1rem;
    align-items: stretch;
  }
  .chart-card {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }
  .chart-head {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-start;
    justify-content: space-between;
    gap: 0.75rem;
    padding: 1.05rem 1.2rem 0.3rem 1.3rem;
  }
  .chart-head h2 {
    font-size: 1rem;
    color: var(--text-muted);
    font-weight: 600;
  }
  .chart-head > div:first-child {
    min-width: 0;
  }
  .figure {
    display: flex;
    flex-direction: column;
    gap: 0.25rem;
    margin-top: 0.35rem;
  }
  .frames {
    flex: none;
  }
  .now {
    font-size: 2.2rem;
    font-weight: 650;
    letter-spacing: -0.03em;
    line-height: 1.1;
  }
  .delta {
    display: inline-flex;
    align-items: center;
    gap: 0.25rem;
    font-size: 0.9rem;
    font-weight: 600;
    color: var(--text-muted);
  }
  .delta.up {
    color: var(--positive);
  }
  .delta.down {
    color: var(--negative);
  }
  .delta .faint {
    margin-left: 0.3rem;
    font-weight: 500;
  }
  .frames {
    display: flex;
    gap: 0.15rem;
    padding: 0.2rem;
    border: 1px solid var(--line);
    border-radius: 999px;
    background: var(--surface-2);
  }
  .frames button {
    min-width: 2.4rem;
    padding: 0.25rem 0.6rem;
    border: 0;
    border-radius: 999px;
    background: none;
    color: var(--text-muted);
    font: inherit;
    font-size: 0.84rem;
    font-weight: 600;
    cursor: pointer;
  }
  .frames button:hover {
    color: var(--text);
  }
  .frames button[aria-pressed='true'] {
    background: var(--accent);
    color: var(--on-accent);
  }
  .chart-body {
    flex: 1;
    padding: 0.5rem 1rem 0.9rem 1.1rem;
    min-width: 0;
  }
  .state.bad {
    color: var(--negative);
  }
  .trend {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.8rem;
    margin-top: 0.8rem;
  }
  .trend .num {
    font-size: 1.5rem;
    font-weight: 650;
  }
  .small {
    font-size: 0.86rem;
  }
  @container (max-width: 760px) {
    .middle {
      grid-template-columns: minmax(0, 1fr);
    }
  }
  @container (max-width: 520px) {
    .stats {
      grid-template-columns: minmax(0, 1fr);
    }
    .quick .action {
      flex: 1 1 45%;
    }
  }
</style>
