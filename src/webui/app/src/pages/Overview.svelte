<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  import { untrack } from 'svelte';
  // Overview: the balances of the wallet, and the chain as it grows (overviewpage.cpp, chainactivity.cpp).
  import Amount from '../components/Amount.svelte';
  import Icon from '../components/Icon.svelte';
  import { formatCount, formatTime, niceTimeOffset, shortHash } from '../lib/format';
  import { go } from '../lib/nav.svelte';
  import { rpc } from '../lib/rpc';
  import { node, prefs, savePrefs, wallets, walletsEnabled } from '../lib/state.svelte';
  import { coinsToSats } from '../lib/units';
  import BalanceChart from '../components/BalanceChart.svelte';
  import TxStatusIcon from '../components/TxStatusIcon.svelte';
  import { nav } from '../lib/nav.svelte';
  import { addressText, typeText, walletApi, type TxRecord } from '../lib/wallet';

  /** The number of transactions shown (NUM_ITEMS of the overview of chains-qt). */
  const NUM_ITEMS = 5;

  // The newest transactions and the balance chart, asked of the host: the overview never loads the
  // whole history (a large wallet would stall it, and every block would send it again).
  let recent: TxRecord[] = $state.raw([]);
  let recentOf: string | null = $state(null);
  let history: { time: number; balance: bigint }[] = $state.raw([]);
  let historyOf: string | null = null;
  $effect(() => {
    void wallets.txVersion;
    void node.tip?.hash;
    const name = wallets.current;
    if (name === null) {
      recent = [];
      return;
    }
    // At once the first time; then at most a few times a second while blocks come fast (a sync).
    const t = setTimeout(() => {
      walletApi(name)
        .latest(NUM_ITEMS)
        .then((r) => {
          if (wallets.current === name) {
            recent = r;
            recentOf = name;
          }
        })
        .catch(() => undefined);
    }, untrack(() => recentOf) === name ? 150 : 0);
    return () => clearTimeout(t);
  });
  $effect(() => {
    void wallets.txVersion;
    const name = wallets.current;
    if (name === null) {
      history = [];
      return;
    }
    const t = setTimeout(
      () => {
        walletApi(name)
          .balanceHistory()
          .then((h) => {
            if (wallets.current === name) {
              history = h;
              historyOf = name;
            }
          })
          .catch(() => undefined);
      },
      historyOf === name ? 300 : 0,
    );
    return () => clearTimeout(t);
  });

  function openTx(txid: string) {
    nav.highlightTx = txid;
    go('transactions');
  }

  const balances = $derived(wallets.current !== null ? wallets.balances[wallets.current] : undefined);
  const total = $derived(balances ? balances.trusted + balances.untrustedPending + balances.immature : null);

  // The balances of a wallet chosen before any balance event came.
  $effect(() => {
    const name = wallets.current;
    if (name === null || wallets.balances[name]) return;
    rpc
      .call('getbalances', [], { wallet: name })
      .then((b) => {
        if (!wallets.balances[name]) {
          wallets.balances[name] = {
            trusted: coinsToSats(b.mine.trusted),
            untrustedPending: coinsToSats(b.mine.untrusted_pending),
            immature: coinsToSats(b.mine.immature),
          };
        }
      })
      .catch(() => undefined);
  });

  interface BlockRow {
    height: number;
    hash: string;
    time: number;
    txs: number;
  }
  let blocks: BlockRow[] = $state([]);
  let now = $state(Math.floor(Date.now() / 1000));

  $effect(() => {
    const tip = node.tip;
    if (!tip) return;
    const controller = new AbortController();
    (async () => {
      const rows: BlockRow[] = [];
      let hash: string | undefined = tip.hash;
      for (let i = 0; i < 8 && hash; i++) {
        const header: { height: number; time: number; nTx: number; previousblockhash?: string } = await rpc.raw('getblockheader', [hash, true], { signal: controller.signal });
        rows.push({ height: header.height, hash, time: header.time, txs: header.nTx });
        hash = header.previousblockhash;
      }
      blocks = rows;
      now = Math.floor(Date.now() / 1000);
    })().catch(() => undefined);
    return () => controller.abort();
  });

  // The latest transactions waiting in the mempool (ChainActivity): what is known of one is kept until
  // it leaves, and only new ones are looked up.
  interface MempoolRow {
    txid: string;
    time: number;
    fee: number;
    vsize: number;
  }
  const MAX_LOOKUPS = 100;
  let mempoolRows = $state<MempoolRow[]>([]);
  const known = new Map<string, MempoolRow>();
  $effect(() => {
    void node.mempool.size;
    void node.tip?.height;
    const controller = new AbortController();
    (async () => {
      const ids = await rpc.raw<string[]>('getrawmempool', [], { signal: controller.signal });
      const present = new Set(ids);
      for (const id of [...known.keys()]) if (!present.has(id)) known.delete(id);
      const unknown = ids.filter((id) => !known.has(id));
      if (unknown.length > MAX_LOOKUPS) {
        const all = await rpc.raw<Record<string, { time: number; fees: { base: number }; vsize: number }>>('getrawmempool', [true], { signal: controller.signal });
        known.clear();
        for (const [txid, e] of Object.entries(all)) known.set(txid, { txid, time: e.time, fee: e.fees.base, vsize: e.vsize });
      } else {
        for (const txid of unknown) {
          try {
            const e = await rpc.raw<{ time: number; fees: { base: number }; vsize: number }>('getmempoolentry', [txid], { signal: controller.signal });
            known.set(txid, { txid, time: e.time, fee: e.fees.base, vsize: e.vsize });
          } catch {
            // Gone from the mempool meanwhile.
          }
        }
      }
      mempoolRows = [...known.values()].sort((a, b) => b.time - a.time).slice(0, 8);
    })().catch(() => undefined);
    return () => controller.abort();
  });
</script>

<div class="page overview">
  <header class="page-head">
    <h1>Overview</h1>
    <button type="button" class="btn ghost small" aria-pressed={prefs.mask} title="Mask values (Ctrl+Shift+M)" onclick={() => savePrefs({ mask: !prefs.mask })}>
      <Icon name={prefs.mask ? 'eyeoff' : 'eye'} size={16} />
      {prefs.mask ? 'Values masked' : 'Mask values'}
    </button>
  </header>

  {#if node.tip?.ibd}
    <button type="button" class="out-of-sync" title="The displayed information may be out of date. Your wallet automatically synchronizes with the network after a connection is established, but this process has not completed yet." onclick={() => (nav.dialog = 'sync')}>
      <Icon name="alert" size={16} /> Out of sync: the balances and transactions shown may be out of date until the node has caught up with the network.
    </button>
  {/if}

  <section class="balances panel" aria-labelledby="balances-title">
    <div class="panel-head">
      <h2 id="balances-title">Balances</h2>
      {#if wallets.current !== null}<span class="muted">{wallets.current === '' ? 'default wallet' : wallets.current}</span>{/if}
    </div>
    {#if !walletsEnabled()}
      <div class="panel-body empty">
        <p>This node runs without wallets (the option -disablewallet is set).</p>
      </div>
    {:else if wallets.current === null}
      <div class="panel-body empty">
        <p>No wallet is loaded. Create one, or open one of the wallets of this node.</p>
        <button type="button" class="btn primary" onclick={() => go('wallets', 'create')}>Create or open a wallet</button>
      </div>
    {:else}
      <div class="panel-body figures">
        <div class="total">
          <span class="caption">Total</span>
          <span class="big"><Amount sats={total} /></span>
        </div>
        <dl>
          <div>
            <dt title="Your current spendable balance">Available</dt>
            <dd><Amount sats={balances?.trusted} /></dd>
          </div>
          <div>
            <dt title="Total of transactions that have yet to be confirmed, and do not yet count toward the spendable balance">Pending</dt>
            <dd><Amount sats={balances?.untrustedPending} /></dd>
          </div>
          <div>
            <dt title="Mined balance that has not yet matured">Immature</dt>
            <dd><Amount sats={balances?.immature} /></dd>
          </div>
        </dl>
        {#if !prefs.mask}<BalanceChart points={history} />{/if}
      </div>
    {/if}
  </section>

  {#if wallets.current !== null}
    <section class="recent panel" aria-labelledby="recent-title">
      <div class="panel-head">
        <h2 id="recent-title">Recent transactions</h2>
        <button type="button" class="btn ghost small" disabled={prefs.mask} onclick={() => go('transactions')}>All transactions</button>
      </div>
      <div class="panel-body">
        {#if recent.length === 0}
          <p class="muted">{recentOf === wallets.current ? 'No transactions yet.' : 'Loading…'}</p>
        {:else}
          <ul class="txs-list">
            {#each recent as r (r.key)}
              <li>
                <button type="button" class="tx" disabled={prefs.mask} title={prefs.mask ? 'Hidden: privacy mode' : 'Show in the transactions'} onclick={() => openTx(r.txid)}>
                  <TxStatusIcon record={r} />
                  <span class="what">
                    <span class="addr" class:muted={!r.label} translate="no">{addressText(r)}</span>
                    <span class="faint small">{typeText(r.type, r.role) || 'Other'} · {formatTime(r.time)}</span>
                  </span>
                  <span class:pending={!r.countsForBalance} title={r.countsForBalance ? undefined : 'Not counted in the balance yet'}><Amount sats={r.amount} colored plus /></span>
                </button>
              </li>
            {/each}
          </ul>
        {/if}
      </div>
    </section>
  {/if}

  <section class="chain panel" aria-labelledby="chain-title">
    <div class="panel-head">
      <h2 id="chain-title">Latest blocks</h2>
      <button type="button" class="btn ghost small" onclick={() => go('explorer')}>Block explorer</button>
    </div>
    <div class="panel-body">
      {#if blocks.length === 0}
        <p class="muted">Waiting for the node…</p>
      {:else}
        <ol class="blocks">
          {#each blocks as b (b.hash)}
            <li>
              <button type="button" class="block-row" title="Show block {b.height} in the block explorer" onclick={() => go('explorer', `search:${b.hash}`)}>
                <span class="height num">{formatCount(b.height)}</span>
                <span class="mono hash" title={b.hash}>{shortHash(b.hash, 6)}</span>
                <span class="num txs">{b.txs} tx{b.txs === 1 ? '' : 's'}</span>
                <time class="muted" datetime={new Date(b.time * 1000).toISOString()} title={formatTime(b.time)}>{niceTimeOffset(now - b.time)} ago</time>
              </button>
            </li>
          {/each}
        </ol>
      {/if}
    </div>
  </section>

  <section class="chain panel" aria-labelledby="mempool-title">
    <div class="panel-head">
      <h2 id="mempool-title">Latest transactions</h2>
      <span class="muted">{formatCount(node.mempool.size)} waiting</span>
    </div>
    <div class="panel-body">
      {#if mempoolRows.length === 0}
        <p class="muted">No transaction is waiting to be mined.</p>
      {:else}
        <ol class="blocks">
          {#each mempoolRows as t (t.txid)}
            <li>
              <button type="button" class="block-row" title="Show the transaction in the block explorer" onclick={() => go('explorer', `search:${t.txid}`)}>
                <span class="mono hash" title={t.txid}>{shortHash(t.txid, 6)}</span>
                <span class="num txs">{formatCount(t.vsize)} vB</span>
                <span class="num txs">fee <Amount sats={coinsToSats(t.fee)} masked={false} /></span>
                <time class="muted" datetime={new Date(t.time * 1000).toISOString()} title={formatTime(t.time)}>{niceTimeOffset(now - t.time)} ago</time>
              </button>
            </li>
          {/each}
        </ol>
      {/if}
    </div>
  </section>
</div>

<style>
  .overview {
    display: grid;
    grid-template-columns: minmax(0, 1.2fr) minmax(0, 1fr);
    gap: 1rem;
    align-content: start;
    align-items: start;
  }
  .page-head,
  .out-of-sync {
    grid-column: 1 / -1;
  }
  .out-of-sync {
    display: flex;
    gap: 0.5rem;
    align-items: center;
    padding: 0.55rem 0.8rem;
    border: 1px solid var(--warning);
    border-radius: var(--radius-m);
    background: var(--warning-soft);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .txs-list {
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
    margin: 0;
    padding: 0;
    list-style: none;
  }
  /* Each transaction a rounded row-card. */
  .tx {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr) auto;
    gap: 0.7rem;
    align-items: center;
    width: 100%;
    padding: 0.55rem 0.8rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-l);
    background: var(--surface-2);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .tx:hover:not(:disabled) {
    background: var(--raised);
  }
  .tx:disabled {
    cursor: default;
  }
  .what {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }
  .addr {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .small {
    font-size: 0.82rem;
  }
  /* Not counted in the balance yet: in italics (a faded amount was too faint to read). */
  .pending {
    font-style: italic;
  }
  .figures {
    display: flex;
    flex-direction: column;
    gap: 1.1rem;
  }
  .caption {
    display: block;
    color: var(--text-muted);
    font-size: 0.9rem;
  }
  .big {
    font-size: 2.3rem;
    font-weight: 650;
    letter-spacing: -0.02em;
    line-height: 1.15;
  }
  dl {
    display: grid;
    grid-template-columns: repeat(3, minmax(0, 1fr));
    gap: 0.8rem;
    margin: 0;
    padding-top: 1rem;
    border-top: 1px solid var(--line);
  }
  dt {
    color: var(--text-muted);
    font-size: 0.88rem;
  }
  dd {
    margin: 0.15rem 0 0;
    font-weight: 600;
  }
  .empty p {
    color: var(--text-muted);
  }
  .blocks {
    margin: 0;
    padding: 0;
    list-style: none;
  }
  .blocks li {
    border-bottom: 1px solid var(--line);
  }
  .block-row {
    display: grid;
    grid-template-columns: 5.5rem minmax(0, 1fr) 4.5rem 8.5rem;
    gap: 0.6rem;
    align-items: center;
    width: 100%;
    padding: 0.45rem 0.2rem;
    border: 0;
    background: none;
    color: inherit;
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .block-row:hover {
    background: var(--raised);
  }
  [aria-labelledby='mempool-title'] .block-row {
    grid-template-columns: minmax(0, 1fr) 5rem 9.5rem 7.5rem;
  }
  .blocks li:last-child {
    border-bottom: 0;
  }
  .height {
    font-weight: 650;
  }
  .hash {
    white-space: nowrap;
    color: var(--text-muted);
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .txs {
    text-align: right;
  }
  time {
    text-align: right;
    font-size: 0.88rem;
  }
  @media (max-width: 980px) {
    .overview {
      grid-template-columns: minmax(0, 1fr);
    }
    dl {
      grid-template-columns: minmax(0, 1fr);
    }
  }
</style>
