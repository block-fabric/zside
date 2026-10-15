<script lang="ts">
  // The block explorer (BlockExplorer of chains-qt): search by height, block hash or transaction id;
  // the latest blocks as a ribbon of boxes after the block being mined (components/BlockRibbon.svelte,
  // after the user's pool site), then the latest blocks and the transactions waiting in the mempool as
  // tables; a block with its transactions; a transaction with its inputs and outputs. The raw JSON of the
  // node is there too.
  import { t } from '../lib/i18n.svelte';
  import Amount from '../components/Amount.svelte';
  import BlockRibbon from '../components/BlockRibbon.svelte';
  import { coinbaseTag, type RibbonBlock } from '../lib/blocks';
  import { every } from '../lib/poll';
  import CopyButton from '../components/CopyButton.svelte';
  import Pager from '../components/Pager.svelte';
  import DataTable from '../components/DataTable.svelte';
  import Icon from '../components/Icon.svelte';
  import { errorText, inform } from '../lib/dialogs.svelte';
  import { formatBytes, formatCount, formatTime, shortHash } from '../lib/format';
  import { untrack } from 'svelte';
  import { css, fillHeight } from '../lib/actions';
  import { detail, showDetail } from '../lib/detail.svelte';
  import { nav } from '../lib/nav.svelte';
  import { host, rpc } from '../lib/rpc';
  import { node, prefs, savePrefs } from '../lib/state.svelte';
  import { filterAndSort, type Column } from '../lib/table';
  import { explorerQuery } from '../lib/tools';
  import { coinsToSats } from '../lib/units';
  import { SIDECHAIN } from '../chain/config';
  import { bmmCommitments } from '../lib/chain';
  import { side, startSide } from '../lib/sidestate.svelte';

  /** Most waiting transactions listed (MAX_MEMPOOL_ROWS). */
  const MAX_MEMPOOL_ROWS = 1000;

  interface BlockRow {
    height: number;
    time: number;
    nTx: number;
    /** Read with the block (getblock) for the rows shown: unknown before. */
    size?: number;
    weight?: number;
    difficulty: number;
    hash: string;
    /** The id of its coinbase (for the miner's tag). */
    coinbase?: string;
  }
  interface MempoolRow {
    txid: string;
    time: number;
    fee: number;
    vsize: number;
  }
  interface Block extends BlockRow {
    size: number;
    weight: number;
    tx: string[];
    previousblockhash?: string;
    nextblockhash?: string;
    merkleroot: string;
    confirmations: number;
    bits: string;
    nonce: number;
    version: number;
    mediantime: number;
    chainwork: string;
    [key: string]: unknown;
  }
  interface Tx {
    txid: string;
    hash: string;
    size: number;
    vsize: number;
    weight: number;
    locktime: number;
    version: number;
    vin: { txid?: string; vout?: number; coinbase?: string; sequence: number }[];
    vout: { value: number; n: number; scriptPubKey: { type: string; address?: string; asm: string; hex: string } }[];
    blockhash?: string;
    confirmations?: number;
    time?: number;
    [key: string]: unknown;
  }

  const TABS = [
    { id: 'blocks', label: 'Blocks' },
    { id: 'mempool', label: 'Waiting transactions' },
  ];
  let list = $state<'blocks' | 'mempool'>('blocks');
  let query = $state('');
  let blocks = $state<BlockRow[]>([]);
  let mempool = $state<MempoolRow[]>([]);
  let mempoolTotal = $state(0);
  let selectedBlock = $state<string | null>(null);
  let selectedTx = $state<string | null>(null);
  // The workspace layout shows the block selected in its detail panel.
  $effect(() => {
    const hash = selectedBlock;
    if (hash && detail.follow) untrack(() => showDetail({ kind: 'block', hash }));
  });
  let blockSort = $state<string | null>('height');
  let blockDir = $state<'asc' | 'desc'>('desc');
  // How many of the latest blocks are read, and how many a page shows (user decisions); sorted across all
  // of them, then paged.
  // 0: all of them.
  const BLOCK_POINTS = [25, 50, 100, 250, 500, 1000, 10000, 50000, 0];
  const BLOCK_PAGE_SIZES = [10, 25, 50, 100];
  /** The height of a row of the list (DataTable's), to size the list to its page. */
  const ROW = 34;
  const blockPoints = $derived(BLOCK_POINTS.includes(prefs.explorerCount) ? BLOCK_POINTS : [...BLOCK_POINTS.filter((n) => n > 0), prefs.explorerCount].sort((a, b) => a - b).concat(0));
  let blocksPage = $state(0);
  // Another order: from its first page.
  $effect(() => {
    void blockSort;
    void blockDir;
    blocksPage = 0;
  });
  let shown = $state<{ kind: 'block'; block: Block } | { kind: 'tx'; tx: Tx; blockHash: string | null } | null>(null);
  let raw = $state(false);
  let loading = $state(false);
  const cache = new Map<string, BlockRow>();

  async function blockRow(hash: string): Promise<BlockRow & { previousblockhash?: string }> {
    const known = cache.get(hash);
    if (known) return known;
    const b = await rpc.raw<Block>('getblock', [hash, 1]);
    const row = { height: b.height, time: b.time, nTx: b.nTx as number, size: b.size, weight: b.weight, difficulty: b.difficulty, hash, previousblockhash: b.previousblockhash, coinbase: b.tx?.[0] };
    cache.set(hash, row);
    return row;
  }

  // The list: from the block index in one call (rpc.explorerBlocks), up to every block of the chain; the
  // size and weight of a block (read with the block) for the rows a page shows and the boxes of the ribbon.
  let detailsVersion = $state(0);
  async function refreshBlocks() {
    try {
      const count = prefs.explorerCount;
      const tip = await rpc.call('getbestblockhash', []);
      // Nothing to do if the list is up to date.
      if (blocks.length && blocks[0].hash === tip && (count === 0 ? blocks.length === blocks[0].height + 1 : blocks.length === Math.min(count, blocks[0].height + 1))) return;
      const rows = await host.explorerBlocks(count);
      blocks = rows.map(([height, hash, time, nTx, difficulty]) => cache.get(hash) ?? { height, hash, time, nTx, difficulty });
    } catch {
      // Again at the next block.
    }
  }
  /** The size and weight of these blocks, read once each. */
  async function fillDetails(rows: readonly BlockRow[]) {
    const missing = rows.filter((b) => !cache.has(b.hash));
    if (!missing.length) return;
    for (const b of missing) await blockRow(b.hash).catch(() => undefined);
    detailsVersion++;
  }
  const withDetails = (b: BlockRow): BlockRow => cache.get(b.hash) ?? b;

  async function refreshMempool() {
    try {
      const all = await rpc.raw<Record<string, { time: number; fees: { base: number }; vsize: number }>>('getrawmempool', [true]);
      const ids = Object.keys(all);
      mempoolTotal = ids.length;
      mempool = ids.slice(0, MAX_MEMPOOL_ROWS).map((txid) => ({ txid, time: all[txid].time, fee: all[txid].fees.base, vsize: all[txid].vsize }));
    } catch {
      mempool = [];
    }
  }

  $effect(() => {
    void node.tip?.height;
    void prefs.explorerCount;
    refreshBlocks();
  });
  $effect(() => {
    void node.mempool.size;
    void node.tip?.height;
    refreshMempool();
  });

  // --- The ribbon: the blocks of the list ("Last N blocks") as boxes, newest first. Their details (reward,
  //     tag, BMM) are read for the first boxes, and for more as the strip scrolls towards them. ---
  const RIBBON_STEP = 12;
  const RIBBON_MAX = 1000;
  let detailed = $state(RIBBON_STEP);
  interface Extra {
    subsidy: number;
    fees: number;
    feeRate: number;
    tag: string;
  }
  const extras = new Map<string, Extra>();
  let extrasVersion = $state(0);
  let bmmAt = $state.raw<Record<number, number[]>>({});
  let fresh = $state<string | null>(null);
  let nowSecs = $state(Math.floor(Date.now() / 1000));
  $effect(() => every(15000, () => (nowSecs = Math.floor(Date.now() / 1000))));

  async function extraOf(row: BlockRow): Promise<void> {
    if (extras.has(row.hash)) return;
    const stats = await rpc.raw<{ subsidy: number; totalfee: number; avgfeerate: number }>('getblockstats', [row.hash, ['subsidy', 'totalfee', 'avgfeerate']]);
    let tag = '';
    if (row.coinbase) {
      const cb = await rpc.raw<{ vin: { coinbase?: string }[] }>('getrawtransaction', [row.coinbase, true, row.hash]).catch(() => null);
      tag = coinbaseTag(cb?.vin[0]?.coinbase ?? '');
    }
    extras.set(row.hash, { subsidy: stats.subsidy, fees: stats.totalfee, feeRate: stats.avgfeerate, tag });
  }

  async function refreshRibbon(rows: readonly BlockRow[]) {
    const latest = rows.slice(0, detailed);
    if (!latest.length) return;
    try {
      // A block whose stats cannot be read (pruned) is drawn without them.
      // Their blocks first (the coinbase, the weight).
      await fillDetails(latest);
      for (const row of latest) await extraOf(withDetails(row)).catch(() => undefined);
      extrasVersion++;
      // A sidechain has no sidechains: its boxes show the block of the mainchain that merged mined each
      // (mainOf, from the events the app follows).
      if (SIDECHAIN) return;
      // Blind merged mining: the sidechains each block committed to.
      const from = latest[latest.length - 1].height;
      const count = latest[0].height - from + 1;
      bmmAt = await bmmCommitments(from, count);
    } catch {
      // Again at the next block.
    }
  }

  /** The strip neared its end: the details of the next boxes. */
  function moreDetails() {
    const boxes = Math.min(blocks.length, RIBBON_MAX);
    if (detailed >= boxes) return;
    detailed = Math.min(boxes, detailed + RIBBON_STEP);
    void refreshRibbon(blocks);
  }

  // A sidechain: the block of the mainchain that committed to each block of this chain (lib/sidestate.svelte.ts
  // follows the events of the mainchain while the app runs).
  if (SIDECHAIN) startSide();
  const mainOf = $derived.by(() => {
    const at = new Map<string, { height: number; hash: string }>();
    if (SIDECHAIN) for (const e of side.main.events) if (e.bmm) at.set(e.bmm, { height: e.height, hash: e.hash });
    return at;
  });

  const ribbon = $derived.by((): RibbonBlock[] => {
    void extrasVersion;
    void detailsVersion;
    // The boxes: the first RIBBON_MAX blocks of the list (all 50 000 would be too many to draw).
    return blocks.slice(0, RIBBON_MAX).map(withDetails).map((b) => {
      const x = extras.get(b.hash);
      return {
        height: b.height,
        hash: b.hash,
        time: b.time,
        nTx: b.nTx,
        weight: b.weight ?? null,
        subsidy: x?.subsidy ?? null,
        fees: x?.fees ?? null,
        feeRate: x?.feeRate ?? null,
        tag: x ? x.tag : null,
        bmm: bmmAt[b.height] ?? [],
        ...(SIDECHAIN ? { main: mainOf.get(b.hash) ?? null } : {}),
      };
    });
  });

  // A new block while the page is shown: it slides in once.
  let topSeen: string | null = null;
  $effect(() => {
    const top = blocks[0]?.hash ?? null;
    if (!top) return;
    const first = topSeen === null;
    const changed = topSeen !== top;
    topSeen = top;
    untrack(() => refreshRibbon(blocks));
    if (first || !changed) return;
    fresh = top;
    const timer = setTimeout(() => (fresh = null), 1500);
    return () => clearTimeout(timer);
  });

  async function pickBlock(hash: string) {
    selectedBlock = hash;
    list = 'blocks';
    if (!detail.follow && (await showBlock(hash))) {
      document.getElementById('explorer-title')?.scrollIntoView({ block: 'nearest' });
    }
  }

  async function showBlock(hash: string): Promise<boolean> {
    try {
      const block = await rpc.raw<Block>('getblock', [hash, 1]);
      shown = { kind: 'block', block };
      selectedTx = null;
      return true;
    } catch {
      return false;
    }
  }

  async function showTx(txid: string, blockHash: string | null): Promise<boolean> {
    try {
      const tx = await rpc.raw<Tx>('getrawtransaction', blockHash ? [txid, true, blockHash] : [txid, true]);
      shown = { kind: 'tx', tx, blockHash };
      return true;
    } catch {
      return false;
    }
  }

  async function search(input = query) {
    const q = explorerQuery(input);
    loading = true;
    try {
      if (q.kind === 'height') {
        try {
          const hash = await rpc.call('getblockhash', [q.height]);
          await showBlock(hash);
          return;
        } catch (e) {
          await inform(t('Block Explorer'), errorText(e));
          return;
        }
      }
      if (q.kind === 'hash') {
        if (await showBlock(q.hash)) return;
        if (await showTx(q.hash, null)) return;
        await inform(
          t('Block Explorer'),
          t('No block has this hash, and no transaction with this id is waiting to be mined. Finding a mined transaction by its id alone requires the node to run with -txindex.'),
        );
        return;
      }
      await inform(t('Block Explorer'), t('Enter a block height, a block hash or a transaction id.'));
    } finally {
      loading = false;
    }
  }

  // A search given with the page ("search:<hash>", from the overview and the transactions).
  $effect(() => {
    const t = nav.tab;
    if (!t.startsWith('search:')) return;
    const q = t.slice('search:'.length);
    nav.tab = '';
    query = q;
    search(q);
  });

  const blockColumns: Column<BlockRow>[] = [
    { key: 'height', label: 'Height', value: (b) => b.height, width: '6rem', align: 'end' },
    { key: 'time', label: 'Time', value: (b) => b.time, width: '11.5rem' },
    { key: 'nTx', label: 'Transactions', value: (b) => b.nTx, width: '7.5rem', align: 'end' },
    { key: 'size', label: 'Size', value: (b) => b.size, width: '6rem', align: 'end' },
    { key: 'weight', label: 'Weight', value: (b) => b.weight, width: '6.5rem', align: 'end' },
    { key: 'difficulty', label: 'Difficulty', value: (b) => b.difficulty, width: '10rem', align: 'end' },
    { key: 'hash', label: 'Hash', value: (b) => b.hash, width: 'minmax(14rem, 1fr)', mono: true },
  ];
  const sortedBlocks = $derived.by(() => {
    void detailsVersion;
    return filterAndSort(blocks.map(withDetails), blockColumns, '', blockSort, blockDir);
  });
  const blocksShown = $derived(sortedBlocks.slice(blocksPage * prefs.explorerPageSize, (blocksPage + 1) * prefs.explorerPageSize));
  $effect(() => {
    const rows = blocksShown;
    untrack(() => fillDetails(rows));
  });
  const mempoolColumns: Column<MempoolRow>[] = [
    { key: 'time', label: 'Received', value: (m) => m.time, width: '11.5rem' },
    { key: 'fee', label: 'Fee', value: (m) => m.fee, width: '9rem', align: 'end' },
    { key: 'vsize', label: 'Virtual size', value: (m) => m.vsize, width: '7rem', align: 'end' },
    { key: 'txid', label: 'Transaction id', value: (m) => m.txid, width: 'minmax(14rem, 1fr)', mono: true },
  ];

  function rawJson(value: Record<string, unknown>, omit: string[]): string {
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(value)) if (!omit.includes(k)) out[k] = v;
    return JSON.stringify(out, null, 2);
  }
  const txValueOut = $derived(shown?.kind === 'tx' ? shown.tx.vout.reduce((sum, o) => sum + coinsToSats(o.value), 0n) : 0n);
</script>

<div class="page explorer">
  <header class="page-head"><h1>Block explorer</h1></header>
  <form class="search row" onsubmit={(e) => { e.preventDefault(); search(); }}>
    <div class="box grow">
      <Icon name="search" size={17} />
      <input id="explorer-search" type="search" class="mono" placeholder="Block height, block hash or transaction id" spellcheck="false" autocomplete="off" bind:value={query} aria-label="Search" />
    </div>
    <button type="submit" class="btn primary" disabled={loading}>Search</button>
    <label class="row count">
      <span class="muted">Last</span>
      <select value={prefs.explorerCount} onchange={(e) => { savePrefs({ explorerCount: Number((e.currentTarget as HTMLSelectElement).value) }); blocksPage = 0; }} aria-label="Blocks listed">
        {#each blockPoints as n (n)}<option value={n}>{n === 0 ? t('All') : formatCount(n)}</option>{/each}
      </select>
      <span class="muted">blocks</span>
    </label>
    <button type="button" class="btn" onclick={() => { cache.clear(); blocks = []; refreshBlocks(); refreshMempool(); }}>Refresh</button>
  </form>

  <BlockRibbon blocks={ribbon} selected={selectedBlock} {fresh} now={nowSecs} onselect={pickBlock} onmore={moreDetails} />

  <div class="list-bar">
  <div class="tabs" role="tablist" aria-label="Lists" id="explorer-lists">
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={list === tb.id} onclick={() => (list = tb.id as 'blocks' | 'mempool')}>{tb.id === 'mempool' ? `${t(tb.label)} (${mempoolTotal})` : t(tb.label)}</button>
    {/each}
  </div>
    {#if list === 'blocks'}
      <div class="block-pages"><Pager total={sortedBlocks.length} bind:page={blocksPage} size={prefs.explorerPageSize} sizes={BLOCK_PAGE_SIZES} label="blocks" onsize={(n) => savePrefs({ explorerPageSize: n })} /></div>
    {/if}
  </div>
  {#if list === 'blocks'}
    <!-- As tall as the rows of the page (10 to 100, user decision): the page scrolls, not the list. -->
    <div class="table-box fit" use:css={{ height: `${(Math.max(1, blocksShown.length) + 1) * ROW + 2}px` }}>
      <DataTable fit rows={blocksShown} columns={blockColumns} key={(b) => b.hash} bind:selected={selectedBlock} bind:sortKey={blockSort} bind:sortDir={blockDir} label="Blocks" empty="Loading…" onactivate={(b) => showBlock(b.hash)}>
        {#snippet cell(b, column)}
          {#if column.key === 'time'}{formatTime(b.time)}
          {:else if column.key === 'size'}{#if b.size === undefined}<span class="faint">…</span>{:else}{formatBytes(b.size)}{/if}
          {:else if column.key === 'weight' && b.weight === undefined}<span class="faint">…</span>
          {:else if column.key === 'difficulty'}{b.difficulty.toPrecision(8)}
          {:else if column.key === 'height' || column.key === 'nTx' || column.key === 'weight'}{formatCount(Number(column.value(b)))}
          {:else}{b.hash}{/if}
        {/snippet}
      </DataTable>
    </div>
  {:else}
    <div class="table-box" use:fillHeight>
      <DataTable rows={mempool} columns={mempoolColumns} key={(m) => m.txid} bind:selected={selectedTx} label="Waiting transactions" empty="No transaction is waiting" onactivate={(m) => showTx(m.txid, null)}>
        {#snippet cell(m, column)}
          {#if column.key === 'time'}{formatTime(m.time)}
          {:else if column.key === 'fee'}<Amount sats={coinsToSats(m.fee)} masked={false} />
          {:else if column.key === 'vsize'}{formatCount(m.vsize)} vB
          {:else}{m.txid}{/if}
        {/snippet}
      </DataTable>
    </div>
  {/if}
  <p class="faint hint">Double-click a block or a transaction to see its details.</p>

  {#if shown?.kind === 'block'}
    {@const b = shown.block}
    <section class="panel detail" aria-label="Block {b.height}">
      <div class="panel-head">
        <h2 id="explorer-title">Block {formatCount(b.height)}</h2>
        <div class="row">
          {#if b.previousblockhash}<button type="button" class="btn small" onclick={() => showBlock(b.previousblockhash!)}><Icon name="receive" size={14} /> Previous</button>{/if}
          {#if b.nextblockhash}<button type="button" class="btn small" onclick={() => showBlock(b.nextblockhash!)}>Next <Icon name="send" size={14} /></button>{/if}
          <button type="button" class="btn small ghost" aria-pressed={raw} onclick={() => (raw = !raw)}>{raw ? 'Details' : 'Raw JSON'}</button>
        </div>
      </div>
      {#if raw}
        <pre class="raw mono panel-body">{rawJson(b, ['tx'])}</pre>
      {:else}
        <div class="panel-body block-grid">
          <dl class="facts">
            <dt>Hash</dt><dd class="mono break">{b.hash} <CopyButton text={b.hash} label="Copy the block hash" /></dd>
            <dt>Time</dt><dd>{formatTime(b.time)} <span class="faint">(median {formatTime(b.mediantime)})</span></dd>
            <dt>Confirmations</dt><dd class="num">{formatCount(b.confirmations)}</dd>
            <dt>Transactions</dt><dd class="num">{formatCount(b.tx.length)}</dd>
            <dt>Size</dt><dd>{formatBytes(b.size)} · weight {formatCount(b.weight)}</dd>
            <dt>Difficulty</dt><dd class="num">{b.difficulty.toPrecision(10)} <span class="faint mono">bits {b.bits}</span></dd>
            <dt>Merkle root</dt><dd class="mono break">{b.merkleroot}</dd>
            <dt>Version / nonce</dt><dd class="mono">{b.version} / {b.nonce}</dd>
          </dl>
          <div>
            <h3>Transactions of the block</h3>
            <ul class="txs" aria-label="Transactions of the block">
              {#each b.tx as txid, i (txid)}
                <li><button type="button" class="mono" onclick={() => showTx(txid, b.hash)}><span class="faint">{i}</span> {txid}</button></li>
              {/each}
            </ul>
          </div>
        </div>
      {/if}
    </section>
  {:else if shown?.kind === 'tx'}
    {@const tx = shown.tx}
    <section class="panel detail" aria-label="Transaction">
      <div class="panel-head">
        <h2 id="explorer-title" class="break">Transaction <span class="mono">{shortHash(tx.txid, 12)}</span></h2>
        <div class="row">
          {#if shown.blockHash}<button type="button" class="btn small" onclick={() => showBlock(shown!.kind === 'tx' && shown!.blockHash ? shown!.blockHash : '')}>Its block</button>{/if}
          <button type="button" class="btn small ghost" aria-pressed={raw} onclick={() => (raw = !raw)}>{raw ? 'Details' : 'Raw JSON'}</button>
        </div>
      </div>
      {#if raw}
        <pre class="raw mono panel-body">{rawJson(tx, ['hex'])}</pre>
      {:else}
        <div class="panel-body">
          <dl class="facts">
            <dt>Transaction id</dt><dd class="mono break">{tx.txid} <CopyButton text={tx.txid} label="Copy the transaction id" /></dd>
            <dt>Status</dt><dd>{tx.confirmations ? `${formatCount(tx.confirmations)} confirmations` : 'Waiting to be mined'}{tx.time ? `, ${formatTime(tx.time)}` : ''}</dd>
            <dt>Size</dt><dd>{formatCount(tx.size)} bytes · {formatCount(tx.vsize)} vB · weight {formatCount(tx.weight)}</dd>
            <dt>Paid out</dt><dd><Amount sats={txValueOut} masked={false} /></dd>
          </dl>
          <div class="io">
            <div>
              <h3>Inputs ({tx.vin.length})</h3>
              <ul class="plain">
                {#each tx.vin as input, i (i)}
                  <li class="mono break">
                    {#if input.coinbase}<span class="badge accent">coinbase</span> {input.coinbase}{:else}<button type="button" class="mono linkish" onclick={() => { query = input.txid ?? ''; search(input.txid); }}>{input.txid}:{input.vout}</button>{/if}
                  </li>
                {/each}
              </ul>
            </div>
            <div>
              <h3>Outputs ({tx.vout.length})</h3>
              <ul class="plain">
                {#each tx.vout as out (out.n)}
                  <li>
                    <span class="faint">{out.n}</span>
                    <span class="mono break">{out.scriptPubKey.address ?? out.scriptPubKey.type}</span>
                    <Amount sats={coinsToSats(out.value)} masked={false} />
                  </li>
                {/each}
              </ul>
            </div>
          </div>
        </div>
      {/if}
    </section>
  {/if}
</div>

<style>
  .search {
    margin-top: 0.6rem;
    flex-wrap: wrap;
  }
  .box {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0 0.6rem;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-s);
    background: var(--surface);
    color: var(--text-muted);
  }
  .box input {
    border: 0;
    padding-left: 0;
    background: transparent;
  }
  .box input:focus-visible {
    outline: none;
  }
  .box:has(input:focus-visible) {
    outline: 2px solid var(--focus);
    outline-offset: 2px;
  }
  .count select {
    width: auto;
  }
  .list-bar {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem 1rem;
  }
  .block-pages {
    margin-left: auto;
  }
  .block-pages :global(.pager) {
    padding: 0;
  }
  /* The list takes the height the window has left (fillHeight), 16rem at least. */
  .table-box {
    height: 21rem;
    min-height: 16rem;
  }
  .table-box.fit {
    min-height: 0;
  }
  .hint {
    margin: 0.4rem 0 1rem;
    font-size: 0.86rem;
  }
  .detail {
    margin-bottom: 1rem;
  }
  .block-grid {
    display: grid;
    grid-template-columns: minmax(0, 1.2fr) minmax(0, 1fr);
    gap: 1.5rem;
  }
  .txs {
    margin: 0.5rem 0 0;
    padding: 0;
    list-style: none;
    max-height: 18rem;
    overflow: auto;
    border: 1px solid var(--line);
    border-radius: var(--radius-s);
  }
  .txs button,
  .linkish {
    display: block;
    width: 100%;
    padding: 0.3rem 0.6rem;
    border: 0;
    background: none;
    color: var(--link);
    text-align: left;
    cursor: pointer;
    overflow-wrap: anywhere;
  }
  .linkish {
    padding: 0;
    display: inline;
    width: auto;
  }
  .txs button:hover {
    background: var(--raised);
  }
  .raw {
    margin: 0;
    max-height: 30rem;
    overflow: auto;
    white-space: pre;
  }
  .io {
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
    gap: 1.5rem;
    margin-top: 1rem;
  }
  .plain {
    margin: 0.5rem 0 0;
    padding: 0;
    list-style: none;
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
  }
  .plain li {
    display: flex;
    gap: 0.6rem;
    align-items: baseline;
    font-size: 0.9rem;
  }
  .plain li .break {
    flex: 1;
  }
  @media (max-width: 1000px) {
    .block-grid,
    .io {
      grid-template-columns: minmax(0, 1fr);
    }
  }
</style>
