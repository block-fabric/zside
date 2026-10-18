<script lang="ts">
  // The block explorer (BlockExplorer of chains-qt): search by height, block hash or transaction id;
  // the latest blocks as a ribbon of boxes after the block being mined (components/BlockRibbon.svelte,
  // after the user's pool site), then the latest blocks and the transactions waiting in the mempool as
  // tables; a block with its transactions; a transaction with its inputs and outputs. The raw JSON of the
  // node is there too. Every hash, id and height links to its view (a block, a transaction, an address of this chain),
  // the addresses of the open wallet are marked "yours", and Back returns along the views opened. An address is shown
  // from what the open wallet knows of it (no record of addresses, no scan). A block of a sidechain shows the block of
  // the mainchain that merged mined it and what it did with the peg (deposits paid, withdrawals, refunds), the hashes
  // of the mainchain copied, never linked: they are no blocks of this chain.
  import { t } from '../lib/i18n.svelte';
  import Amount from '../components/Amount.svelte';
  import BlockRibbon from '../components/BlockRibbon.svelte';
  import AddressLink from '../components/AddressLink.svelte';
  import AddressView from '../components/AddressView.svelte';
  import { mainBlockTip, mainchainName, mainTxTip, matchDeposits, mayBeAddress, type BlockPeg, type ScriptInfo } from '../lib/explorer';
  import { depositsOf } from '../lib/side';
  import { coinbaseTag, TARGET_SPACING, type RibbonBlock } from '../lib/blocks';
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
  import { EXPLORER, type ChainBlock } from '../chain/explorer';

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
    /** What getblock said of it, without its transactions: for the columns of the chain (chain/explorer.ts). */
    chain?: ChainBlock;
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
    vin: { txid?: string; vout?: number; coinbase?: string; sequence: number; prevout?: { height: number; value: number; scriptPubKey: { type: string; address?: string } } }[];
    vout: { value: number; n: number; scriptPubKey: { type: string; address?: string; asm: string; hex: string } }[];
    blockhash?: string;
    confirmations?: number;
    time?: number;
    /** With the coins it spent (verbosity 2, block undo data read). */
    fee?: number;
    [key: string]: unknown;
  }
  /** What the explorer shows: Back returns along them. */
  type View = { kind: 'block'; hash: string } | { kind: 'tx'; txid: string; blockHash: string | null; height?: number } | { kind: 'address'; address: string };
  /** The most views Back goes back along. */
  const MAX_TRAIL = 50;

  const TABS = [
    { id: 'blocks', label: 'Blocks' },
    { id: 'mempool', label: 'Waiting transactions' },
  ];
  let list = $state<'blocks' | 'mempool'>('blocks');
  let query = $state('');
  // Replaced whole, never changed in place: not made deep reactive (50 000 rows would be 50 000 proxies; audit 7 #12).
  let blocks = $state.raw<BlockRow[]>([]);
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
  let shown = $state<{ kind: 'block'; block: Block } | { kind: 'tx'; tx: Tx; blockHash: string | null } | { kind: 'address'; address: string; info: ScriptInfo } | null>(null);
  let trail = $state.raw<View[]>([]);
  let raw = $state(false);
  let loading = $state(false);
  const cache = new Map<string, BlockRow>();

  async function blockRow(hash: string): Promise<BlockRow & { previousblockhash?: string }> {
    const known = cache.get(hash);
    if (known) return known;
    const b = await rpc.raw<Block>('getblock', [hash, 1]);
    const { tx: _tx, ...chain } = b;
    const row = { height: b.height, time: b.time, nTx: b.nTx as number, size: b.size, weight: b.weight, difficulty: b.difficulty, hash, previousblockhash: b.previousblockhash, coinbase: b.tx?.[0], chain };
    cache.set(hash, row);
    return row;
  }

  // The list: from the block index (host.explorerBlocks), in parts of at most EXPLORER_PART rows from a height down, up
  // to every block of the chain; on a new tip only the new rows are read (audit 7 #12: the whole chain read again under
  // the lock of the node at each block stalled it). One refresh at a time: one asked meanwhile runs after it. The size and
  // weight of a block (read with the block) for the rows a page shows and the boxes of the ribbon.
  let detailsVersion = $state(0);
  const EXPLORER_PART = 10000;
  type IndexRow = [number, string, number, number, number];
  const toRow = ([height, hash, time, nTx, difficulty]: IndexRow): BlockRow => cache.get(hash) ?? { height, hash, time, nTx, difficulty };
  /** Up to `count` rows from the height `from` down, in parts; fewer when the chain is shorter (or moved meanwhile). */
  async function readRows(from: number, count: number): Promise<BlockRow[]> {
    const out: BlockRow[] = [];
    while (out.length < count && from >= 0) {
      const part = await host.explorerBlocks(Math.min(EXPLORER_PART, count - out.length), from);
      // The rows start where asked, or the chain changed under the reading: what was read is kept, the rest next time.
      if (!part.length || part[0][0] !== from) break;
      out.push(...part.map(toRow));
      from = part[part.length - 1][0] - 1;
    }
    return out;
  }
  async function loadBlocks() {
    const count = prefs.explorerCount;
    const [tip] = await host.explorerBlocks(1);
    if (!tip) {
      blocks = [];
      return;
    }
    const [tipHeight, tipHash] = tip;
    const wanted = count === 0 ? tipHeight + 1 : Math.min(count, tipHeight + 1);
    // Nothing to do if the list is up to date.
    if (blocks.length === wanted && blocks[0]?.hash === tipHash) return;
    // The rows kept: those below the new ones, if the top one is still in the chain (no reorganization past it).
    let kept: BlockRow[] = [];
    const top = blocks[0];
    if (top && top.height <= tipHeight) {
      const [same] = await host.explorerBlocks(1, top.height);
      if (same && same[1] === top.hash) kept = blocks;
    }
    const fresh = await readRows(tipHeight, Math.min(wanted, kept.length ? tipHeight - kept[0].height : wanted));
    let next = [...fresh, ...kept].slice(0, wanted);
    // More of the older ones (a larger count chosen).
    if (next.length < wanted && next.length && next[next.length - 1].height > 0) next = next.concat(await readRows(next[next.length - 1].height - 1, wanted - next.length));
    blocks = next;
  }
  let loadingBlocks: Promise<void> | null = null;
  let loadAgain = false;
  async function refreshBlocks() {
    if (loadingBlocks) {
      loadAgain = true;
      return loadingBlocks;
    }
    loadingBlocks = (async () => {
      do {
        loadAgain = false;
        try {
          await loadBlocks();
        } catch {
          // Again at the next block.
        }
      } while (loadAgain);
    })();
    try {
      await loadingBlocks;
    } finally {
      loadingBlocks = null;
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
  // What the chain's columns read besides the blocks (chain/explorer.ts), with each block.
  $effect(() => {
    void node.tip?.height;
    EXPLORER.refresh?.().then(() => detailsVersion++);
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
    let tag = (row.chain && EXPLORER.tag?.(row.chain)) || '';
    if (!tag && row.coinbase) {
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
    if (!detail.follow) await open({ kind: 'block', hash });
  }

  // --- Views, and Back along them ---
  function viewShown(): View | null {
    if (shown?.kind === 'block') return { kind: 'block', hash: shown.block.hash };
    if (shown?.kind === 'tx') return { kind: 'tx', txid: shown.tx.txid, blockHash: shown.blockHash };
    if (shown?.kind === 'address') return { kind: 'address', address: shown.address };
    return null;
  }
  const sameView = (a: View, b: View) => (a.kind === 'block' && b.kind === 'block' ? a.hash === b.hash : a.kind === 'tx' && b.kind === 'tx' ? a.txid === b.txid : a.kind === 'address' && b.kind === 'address' && a.address === b.address);
  function display(v: View): Promise<boolean> {
    if (v.kind === 'block') return showBlock(v.hash);
    if (v.kind === 'tx') return showTx(v.txid, v.blockHash, v.height);
    return showAddress(v.address);
  }
  /** Open a view; the one shown before goes on the trail Back returns along. */
  async function open(v: View): Promise<boolean> {
    const from = viewShown();
    if (!(await display(v))) return false;
    if (from && !sameView(from, v)) trail = [...trail, from].slice(-MAX_TRAIL);
    document.getElementById('explorer-title')?.scrollIntoView?.({ block: 'nearest' });
    return true;
  }
  async function back() {
    const v = trail[trail.length - 1];
    if (!v) return;
    trail = trail.slice(0, -1);
    await display(v);
  }
  const openBlock = (hash: string) => open({ kind: 'block', hash });
  const openTx = (txid: string, blockHash: string | null, height?: number) => open({ kind: 'tx', txid, blockHash, height });
  const openAddress = (address: string) => open({ kind: 'address', address });
  async function openHeight(height: number) {
    try {
      await openBlock(await rpc.call('getblockhash', [height]));
    } catch (e) {
      await inform(t('Block Explorer'), errorText(e));
    }
  }
  /** A transaction of a wallet's address (its block's height known; -1 waiting). */
  const openTxAt = (txid: string, height: number) => openTx(txid, null, height >= 0 ? height : undefined);

  // What the block shown did with the peg (a sidechain: host explorer.peg); null until read, or if it cannot be.
  let peg = $state<BlockPeg | null>(null);
  async function showBlock(hash: string): Promise<boolean> {
    try {
      const block = await rpc.raw<Block>('getblock', [hash, 1]);
      shown = { kind: 'block', block };
      selectedTx = null;
      peg = null;
      if (SIDECHAIN) {
        host.explorerPeg(hash).then(
          (p) => {
            if (shown?.kind === 'block' && shown.block.hash === hash) peg = p;
          },
          () => undefined,
        );
      }
      return true;
    } catch {
      return false;
    }
  }

  // Which transactions spend the outputs of the one shown (gettxspendingprevout: from the mempool, or from
  // -txospenderindex if it is on); by output number.
  let spenders = $state<Record<number, { txid: string; blockHash: string | null }>>({});
  async function showTx(txid: string, blockHash: string | null, height?: number): Promise<boolean> {
    try {
      // A transaction of a known block is read with it (no -txindex needed): from a coin's height when its block is not named.
      if (!blockHash && height !== undefined) blockHash = await rpc.call('getblockhash', [height]).catch(() => null);
      const at = blockHash ? [txid, 2, blockHash] : [txid, 2];
      // Verbosity 2 adds the coins spent (block undo data); without it (pruned), the transaction alone.
      const tx = await rpc.raw<Tx>('getrawtransaction', at).catch(() => rpc.raw<Tx>('getrawtransaction', blockHash ? [txid, 1, blockHash] : [txid, 1]));
      shown = { kind: 'tx', tx, blockHash: blockHash ?? tx.blockhash ?? null };
      spenders = {};
      const outs = tx.vout.slice(0, 100).map((o) => ({ txid: tx.txid, vout: o.n }));
      rpc.raw<{ txid: string; vout: number; spendingtxid?: string; blockhash?: string }[]>('gettxspendingprevout', [outs]).then(
        (r) => {
          if (shown?.kind !== 'tx' || shown.tx.txid !== tx.txid) return;
          const found: Record<number, { txid: string; blockHash: string | null }> = {};
          for (const x of r) if (x.spendingtxid) found[x.vout] = { txid: x.spendingtxid, blockHash: x.blockhash ?? null };
          spenders = found;
        },
        () => undefined,
      );
      return true;
    } catch {
      return false;
    }
  }

  async function showAddress(address: string): Promise<boolean> {
    try {
      const info = await rpc.raw<ScriptInfo>('validateaddress', [address]);
      if (!info.isvalid) return false;
      shown = { kind: 'address', address: info.address ?? address, info };
      return true;
    } catch {
      return false;
    }
  }

  // The block of the mainchain that merged mined the block shown (a sidechain: from the events the app follows).
  const mainOfShown = $derived(SIDECHAIN && shown?.kind === 'block' ? (mainOf.get(shown.block.hash) ?? null) : null);
  // The deposits the coinbase of the block shown pays, by output: the transaction of the mainchain that made each.
  const depositOf = $derived.by(() => {
    if (!SIDECHAIN || !peg || shown?.kind !== 'block') return new Map<number, string>();
    const recorded = depositsOf(side.main.events, Number.MAX_SAFE_INTEGER).map((d) => ({ txid: d.txid, destination: d.destination, sats: d.sats }));
    return matchDeposits(peg.payouts, recorded.reverse());
  });

  async function search(input = query) {
    const q = explorerQuery(input);
    loading = true;
    try {
      if (q.kind === 'height') {
        try {
          const hash = await rpc.call('getblockhash', [q.height]);
          await openBlock(hash);
          return;
        } catch (e) {
          await inform(t('Block Explorer'), errorText(e));
          return;
        }
      }
      if (q.kind === 'hash') {
        if (await openBlock(q.hash)) return;
        if (await openTx(q.hash, null)) return;
        await inform(
          t('Block Explorer'),
          t('No block has this hash, and no transaction with this id is waiting to be mined. Finding a mined transaction by its id alone requires the node to run with -txindex.'),
        );
        return;
      }
      // An address of this chain (the node says), of any kind.
      if (mayBeAddress(input) && (await openAddress(input.trim()))) return;
      await inform(t('Block Explorer'), t('Enter a block height, a block hash, a transaction id or an address.'));
    } finally {
      loading = false;
    }
  }

  // A search given with the page ("search:<hash>", from the overview and the transactions), or a transaction of a
  // block ("tx:<txid>:<block hash>", the detail panel: read with its block, no -txindex needed).
  $effect(() => {
    const t = nav.tab;
    const of = /^tx:([0-9a-f]{64}):([0-9a-f]{64})$/.exec(t);
    if (of) {
      nav.tab = '';
      void openTx(of[1], of[2]);
      return;
    }
    if (!t.startsWith('search:')) return;
    const q = t.slice('search:'.length);
    nav.tab = '';
    query = q;
    search(q);
  });

  // The chain's own columns (chain/explorer.ts) come after the height; the kit's it has no use for are left out.
  const chainColumns: Column<BlockRow>[] = EXPLORER.columns.map((c) => ({ key: `chain:${c.key}`, label: c.label, width: c.width, align: c.align, mono: c.mono, words: !c.mono, value: (b) => (b.chain ? c.value(b.chain) : undefined) }));
  const kitColumns: Column<BlockRow>[] = [
    { key: 'height', label: 'Height', value: (b) => b.height, width: '6rem', align: 'end' },
    { key: 'time', label: 'Time', value: (b) => b.time, width: '11.5rem' },
    { key: 'nTx', label: 'Transactions', value: (b) => b.nTx, width: '7.5rem', align: 'end' },
    { key: 'size', label: 'Size', value: (b) => b.size, width: '6rem', align: 'end' },
    { key: 'weight', label: 'Weight', value: (b) => b.weight, width: '6.5rem', align: 'end' },
    { key: 'difficulty', label: 'Difficulty', value: (b) => b.difficulty, width: '10rem', align: 'end' },
    { key: 'hash', label: 'Hash', value: (b) => b.hash, width: 'minmax(14rem, 1fr)', mono: true },
  ];
  const blockColumns: Column<BlockRow>[] = [kitColumns[0], ...chainColumns, ...kitColumns.slice(1)].filter((c) => !EXPLORER.hide.includes(c.key));
  // The pace the chain aims at: the node's when the chain reads it (chain/explorer.ts), else chain.json's.
  const blockTarget = $derived.by(() => {
    void detailsVersion;
    return EXPLORER.target?.() ?? TARGET_SPACING;
  });
  const chainColumnOf = (key: string) => EXPLORER.columns.find((c) => `chain:${c.key}` === key);
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
      <input id="explorer-search" type="search" class="mono" placeholder="Block height, block hash, transaction id or address" spellcheck="false" autocomplete="off" bind:value={query} aria-label="Search" />
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

  <BlockRibbon blocks={ribbon} selected={selectedBlock} {fresh} now={nowSecs} onselect={pickBlock} onmore={moreDetails} {blockTarget} />

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
      <DataTable fit rows={blocksShown} columns={blockColumns} key={(b) => b.hash} bind:selected={selectedBlock} bind:sortKey={blockSort} bind:sortDir={blockDir} label="Blocks" empty="Loading…" onactivate={(b) => openBlock(b.hash)}>
        {#snippet cell(b, column)}
          {#if column.key.startsWith('chain:')}
            {@const c = chainColumnOf(column.key)}
            {@const v = column.value(b)}
            {#if v === undefined || v === null || !c}<span class="faint">{b.chain ? '' : '…'}</span>
            {:else if c.badge?.(b.chain!)}{@const badge = c.badge(b.chain!)!}<span class="badge {badge.tone}" title={badge.tip}>{v}</span>
            {:else}<span title={c.tip?.(b.chain!)}>{typeof v === 'number' ? formatCount(v) : v}</span>{/if}
          {:else if column.key === 'time'}{formatTime(b.time)}
          {:else if column.key === 'size'}{#if b.size === undefined}<span class="faint">…</span>{:else}{formatBytes(b.size)}{/if}
          {:else if column.key === 'weight' && b.weight === undefined}<span class="faint">…</span>
          {:else if column.key === 'difficulty'}{b.difficulty.toPrecision(8)}
          {:else if column.key === 'height'}<button type="button" class="mono linkish" onclick={() => openBlock(b.hash)}>{formatCount(b.height)}</button>
          {:else if column.key === 'nTx' || column.key === 'weight'}{formatCount(Number(column.value(b)))}
          {:else}<button type="button" class="mono linkish cell-link" onclick={() => openBlock(b.hash)}>{b.hash}</button>{/if}
        {/snippet}
      </DataTable>
    </div>
  {:else}
    <div class="table-box" use:fillHeight>
      <DataTable rows={mempool} columns={mempoolColumns} key={(m) => m.txid} bind:selected={selectedTx} label="Waiting transactions" empty="No transaction is waiting" onactivate={(m) => openTx(m.txid, null)}>
        {#snippet cell(m, column)}
          {#if column.key === 'time'}{formatTime(m.time)}
          {:else if column.key === 'fee'}<Amount sats={coinsToSats(m.fee)} masked={false} />
          {:else if column.key === 'vsize'}{formatCount(m.vsize)} vB
          {:else}<button type="button" class="mono linkish cell-link" onclick={() => openTx(m.txid, null)}>{m.txid}</button>{/if}
        {/snippet}
      </DataTable>
    </div>
  {/if}
  <p class="faint hint">Double-click a block or a transaction to see its details, or click a link.</p>

  {#snippet backButton()}
    {#if trail.length}<button type="button" class="btn small ghost" onclick={back} title={t('Back to what was shown before')}><Icon name="receive" size={14} /> Back</button>{/if}
  {/snippet}

  {#if shown?.kind === 'block'}
    {@const b = shown.block}
    <section class="panel detail" aria-label="Block {b.height}">
      <div class="panel-head">
        <h2 id="explorer-title">Block {formatCount(b.height)}</h2>
        <div class="row">
          {@render backButton()}
          {#if b.previousblockhash}<button type="button" class="btn small" onclick={() => openBlock(b.previousblockhash!)}><Icon name="receive" size={14} /> Previous</button>{/if}
          {#if b.nextblockhash}<button type="button" class="btn small" onclick={() => openBlock(b.nextblockhash!)}>Next <Icon name="send" size={14} /></button>{/if}
          <button type="button" class="btn small ghost" aria-pressed={raw} onclick={() => (raw = !raw)}>{raw ? 'Details' : 'Raw JSON'}</button>
        </div>
      </div>
      {#if raw}
        <pre class="raw mono panel-body">{rawJson(b, ['tx'])}</pre>
      {:else}
        <div class="panel-body block-grid">
          <dl class="facts">
            {#each EXPLORER.facts(b) as f (f.dt)}<dt>{f.dt}</dt><dd class:mono={f.mono} class="break">{f.dd}{#if f.link}{@const l = f.link} <button type="button" class="mono linkish" title={l.id} onclick={() => (l.kind === 'block' ? openBlock(l.id) : l.kind === 'tx' ? openTx(l.id, null) : openAddress(l.id))}>{shortHash(l.id, 10)}</button>{/if}</dd>{/each}
            <dt>Hash</dt><dd class="mono break">{b.hash} <CopyButton text={b.hash} label="Copy the block hash" /></dd>
            <dt>Time</dt><dd>{formatTime(b.time)} <span class="faint">(median {formatTime(b.mediantime)})</span></dd>
            <dt>Confirmations</dt><dd class="num">{formatCount(b.confirmations)}</dd>
            <dt>Transactions</dt><dd class="num">{formatCount(b.tx.length)}</dd>
            <dt>Size</dt><dd>{formatBytes(b.size)} · weight {formatCount(b.weight)}</dd>
            <dt>Difficulty</dt><dd class="num">{b.difficulty.toPrecision(10)} <span class="faint mono">bits {b.bits}</span></dd>
            {#if b.previousblockhash}<dt>Previous block</dt><dd class="mono break"><button type="button" class="mono linkish" onclick={() => openBlock(b.previousblockhash!)}><span class="faint">{formatCount(b.height - 1)} ·</span> {b.previousblockhash}</button></dd>{/if}
            {#if mainOfShown}<dt>Merged mined in</dt><dd class="mono break main-ref" title={mainBlockTip()}><span class="faint">{mainchainName()} {formatCount(mainOfShown.height)} ·</span> {mainOfShown.hash} <CopyButton text={mainOfShown.hash} label={t('Copy the hash of the block of %1', mainchainName())} /></dd>{/if}
            <dt>Merkle root</dt><dd class="mono break">{b.merkleroot}</dd>
            <dt>Version / nonce</dt><dd class="mono">{b.version} / {b.nonce}</dd>
          </dl>
          <div>
            <h3>Transactions of the block</h3>
            <ul class="txs" aria-label="Transactions of the block">
              {#each b.tx as txid, i (txid)}
                <li><button type="button" class="mono" onclick={() => openTx(txid, b.hash)}><span class="faint">{i}</span> {txid}</button></li>
              {/each}
            </ul>
          </div>
        </div>
      {/if}
    </section>
    {#if SIDECHAIN && peg && !raw && (peg.payouts.length || peg.withdrawals.length || peg.refunds.length || peg.bundle)}
      {@const pg = peg}
      <section class="panel detail peg" aria-label="The peg in this block">
        <div class="panel-head"><h3>The peg in this block</h3>{#if pg.more}<span class="faint small">{t('More than shown')}</span>{/if}</div>
        <div class="panel-body peg-grid">
          {#if pg.payouts.length}
            <div>
              <h4>{t('Paid by the coinbase')} ({formatCount(pg.payouts.length)})</h4>
              <ul class="plain" aria-label="Paid by the coinbase">
                {#each pg.payouts as [vout, sats, address] (vout)}
                  {@const from = depositOf.get(vout)}
                  <li>
                    <button type="button" class="mono linkish" title={t('The coinbase, output %1', vout)} onclick={() => openTx(pg.coinbase, b.hash)}>#{vout}</button>
                    <span class="io-what grow">
                      {#if address}<AddressLink {address} onopen={openAddress} />{:else}<span class="faint">{t('a script')}</span>{/if}
                      {#if from}<span class="badge accent" title={mainTxTip()}>{t('deposit')}</span> <span class="mono faint" title={mainTxTip()}>{shortHash(from, 8)}</span> <CopyButton text={from} label={t('Copy the transaction of %1', mainchainName())} />{:else}<span class="faint small">{t('deposit or refund')}</span>{/if}
                    </span>
                    <Amount sats={sats} masked={false} />
                  </li>
                {/each}
              </ul>
            </div>
          {/if}
          {#if pg.withdrawals.length || pg.refunds.length || pg.bundle}
            <div>
              {#if pg.bundle}<p class="small"><span class="muted">{t('Commits to the withdrawal bundle')}</span> <span class="mono" title={mainTxTip()}>{shortHash(pg.bundle, 10)}</span> <CopyButton text={pg.bundle} label="Copy the bundle" /></p>{/if}
              {#if pg.withdrawals.length}
                <h4>{t('Withdrawals')} ({formatCount(pg.withdrawals.length)})</h4>
                <ul class="plain" aria-label="Withdrawals">
                  {#each pg.withdrawals as [txid, vout, sats, fee, script, refund] (`${txid}:${vout}`)}
                    <li>
                      <span class="io-line grow">
                        <button type="button" class="mono linkish" title={txid} onclick={() => openTx(txid, b.hash)}>{shortHash(txid, 10)}:{vout}</button>
                        <span class="io-what"><span class="faint small">{t('to %1', mainchainName())}</span> <span class="mono small break" title={mainTxTip()}>{shortHash(script, 12)}</span> <CopyButton text={script} label={t('Copy the script on %1', mainchainName())} /></span>
                        <span class="io-what"><span class="faint small">{t('refund to')}</span> <AddressLink address={refund} short={10} onopen={openAddress} /></span>
                      </span>
                      <span class="end"><Amount sats={sats} masked={false} />{#if fee}<br /><span class="faint small">{t('fee')} <Amount sats={fee} masked={false} /></span>{/if}</span>
                    </li>
                  {/each}
                </ul>
              {/if}
              {#if pg.refunds.length}
                <h4>{t('Refunds asked')} ({formatCount(pg.refunds.length)})</h4>
                <ul class="plain" aria-label="Refunds asked">
                  {#each pg.refunds as [txid, wtxid, wvout] (txid + wtxid + wvout)}
                    <li><button type="button" class="mono linkish" title={txid} onclick={() => openTx(txid, b.hash)}>{shortHash(txid, 10)}</button> <span class="faint small">{t('takes back')}</span> <button type="button" class="mono linkish" title={wtxid} onclick={() => openTx(wtxid, null)}>{shortHash(wtxid, 8)}:{wvout}</button></li>
                  {/each}
                </ul>
              {/if}
            </div>
          {/if}
        </div>
      </section>
    {/if}
  {:else if shown?.kind === 'tx'}
    {@const tx = shown.tx}
    <section class="panel detail" aria-label="Transaction">
      <div class="panel-head">
        <h2 id="explorer-title" class="break">Transaction <span class="mono">{shortHash(tx.txid, 12)}</span></h2>
        <div class="row">
          {@render backButton()}
          {#if shown.blockHash}<button type="button" class="btn small" onclick={() => openBlock(shown!.kind === 'tx' && shown!.blockHash ? shown!.blockHash : '')}>Its block</button>{/if}
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
            {#if shown.blockHash}{@const bh = shown.blockHash}{@const at = blocks.find((r) => r.hash === bh)?.height ?? (typeof tx.blockheight === 'number' ? tx.blockheight : null)}<dt>Block</dt><dd class="mono break"><button type="button" class="mono linkish" onclick={() => openBlock(shown!.kind === 'tx' && shown!.blockHash ? shown!.blockHash : '')}>{#if at !== null}<span class="faint">{formatCount(at)} ·</span>{' '}{/if}{shown.blockHash}</button></dd>{/if}
            <dt>Size</dt><dd>{formatCount(tx.size)} bytes · {formatCount(tx.vsize)} vB · weight {formatCount(tx.weight)}</dd>
            <dt>Paid out</dt><dd><Amount sats={txValueOut} masked={false} />{#if tx.fee !== undefined} <span class="faint">· fee <Amount sats={coinsToSats(tx.fee)} masked={false} /></span>{/if}</dd>
          </dl>
          <div class="io">
            <div>
              <h3>Inputs ({tx.vin.length})</h3>
              <ul class="plain">
                {#each tx.vin as input, i (i)}
                  <li class="mono break">
                    {#if input.coinbase}<span class="badge accent">coinbase</span> {input.coinbase}
                    {:else}
                      <span class="io-line">
                        <button type="button" class="mono linkish" title={t('The transaction of the coin spent')} onclick={() => openTx(input.txid ?? '', null, input.prevout?.height)}>{input.txid}:{input.vout}</button>
                        {#if input.prevout}<span class="io-what">{#if input.prevout.scriptPubKey.address}<AddressLink address={input.prevout.scriptPubKey.address} onopen={openAddress} />{:else}<span class="faint">{input.prevout.scriptPubKey.type}</span>{/if} <Amount sats={coinsToSats(input.prevout.value)} masked={false} /></span>{/if}
                      </span>
                    {/if}
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
                    <span class="mono break">{#if out.scriptPubKey.address}<AddressLink address={out.scriptPubKey.address} onopen={openAddress} />{:else}{out.scriptPubKey.type}{/if}{#if spenders[out.n]}{@const by = spenders[out.n]}<br /><span class="faint spent">spent by</span> <button type="button" class="mono linkish" title={by.txid} onclick={() => openTx(by.txid, by.blockHash)}>{shortHash(by.txid, 8)}</button>{/if}</span>
                    <Amount sats={coinsToSats(out.value)} masked={false} />
                  </li>
                {/each}
              </ul>
            </div>
          </div>
        </div>
      {/if}
    </section>
  {:else if shown?.kind === 'address'}
    {@const a = shown}
    <AddressView address={a.address} info={a.info} ontx={(txid, height) => openTxAt(txid, height)} onblock={openHeight} actions={backButton} />
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
  .linkish:hover,
  .linkish:focus-visible {
    text-decoration: underline;
  }
  .cell-link {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    max-width: 100%;
  }
  .io-line {
    display: flex;
    flex-direction: column;
    gap: 0.1rem;
  }
  .io-what {
    display: inline-flex;
    flex-wrap: wrap;
    gap: 0.5rem;
    align-items: baseline;
  }
  .grow {
    flex: 1;
    min-width: 0;
  }
  .main-ref {
    cursor: help;
  }
  .peg-grid {
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
    gap: 1.5rem;
  }
  .peg h4 {
    margin: 0 0 0.3rem;
    font-size: 0.9rem;
  }
  .peg .end {
    text-align: right;
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
    .peg-grid,
    .io {
      grid-template-columns: minmax(0, 1fr);
    }
  }
</style>
