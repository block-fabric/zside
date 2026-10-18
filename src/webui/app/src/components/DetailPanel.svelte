<script lang="ts">
  // The detail panel at the right of the page: the transaction, block, peer or sidechain selected.
  // On a narrow window it covers the page. Escape closes it.
  import SafeText from './SafeText.svelte';
  import Amount from './Amount.svelte';
  import CopyButton from './CopyButton.svelte';
  import Icon from './Icon.svelte';
  import TxDetailsView from './TxDetailsView.svelte';
  import { closeDetail, detail, showDetail, type DetailItem } from '../lib/detail.svelte';
  import { formatBytes, formatCount, formatTime, niceTimeOffset, shortHash } from '../lib/format';
  import { go, goTo } from '../lib/nav.svelte';
  import { rpc } from '../lib/rpc';
  import { loadSidechainDetail } from '../lib/chain';
  import { SIDECHAIN } from '../chain/config';
  import { coinsToSats } from '../lib/units';

  interface Block {
    hash: string;
    height: number;
    time: number;
    confirmations: number;
    nTx: number;
    size: number;
    weight: number;
    difficulty: number;
    previousblockhash?: string;
    nextblockhash?: string;
    tx: string[];
  }
  interface Peer {
    id: number;
    addr: string;
    network: string;
    inbound: boolean;
    connection_type: string;
    subver: string;
    version: number;
    startingheight: number;
    synced_blocks: number;
    synced_headers: number;
    pingtime?: number;
    minping?: number;
    bytessent: number;
    bytesrecv: number;
    conntime: number;
    lastsend: number;
    lastrecv: number;
  }
  interface Sidechain {
    slot: number;
    title: string;
    description: string;
    version: number;
    hashid1: string;
    hashid2: string;
    activationheight: number;
    escrow?: { txid: string; n: number; amount: number };
    pendingbundles: number;
  }
  interface Bundle {
    hash: string;
    height: number;
    score: number;
    blocksleft: number;
    payable: boolean;
    vote: string;
  }

  let block: Block | null = $state(null);
  let peer: Peer | null = $state(null);
  let sidechain: { info: Sidechain; bundles: Bundle[] } | null = $state(null);
  let error = $state('');
  let panel: HTMLElement | undefined = $state();

  const item = $derived(detail.item);
  const TITLES: Record<DetailItem['kind'], string> = { tx: 'Transaction', block: 'Block', peer: 'Peer', sidechain: 'Sidechain' };

  $effect(() => {
    const it = item;
    block = null;
    peer = null;
    sidechain = null;
    error = '';
    if (!it || it.kind === 'tx') return;
    const fail = (e: unknown) => (error = e instanceof Error ? e.message : String(e));
    if (it.kind === 'block') {
      rpc.raw<Block>('getblock', [it.hash, 1]).then((b) => (block = b)).catch(fail);
    } else if (it.kind === 'peer') {
      rpc
        .raw<Peer[]>('getpeerinfo')
        .then((list) => {
          peer = list.find((p) => p.id === it.id) ?? null;
          if (!peer) error = 'This peer is not connected any more.';
        })
        .catch(fail);
    } else if (SIDECHAIN) {
      // A sidechain has no sidechains of its own (nothing shows one here).
      error = 'This sidechain is not active.';
    } else {
      loadSidechainDetail<Sidechain, Bundle>(it.slot)
        .then(([list, bundles]) => {
          const info = list.find((s) => s.slot === it.slot);
          if (info) sidechain = { info, bundles };
          else error = 'This sidechain is not active.';
        })
        .catch(fail);
    }
  });

  // Focus moves into the panel when it opens from a click, so the keyboard follows.
  let lastKind = '';
  $effect(() => {
    const kind = item?.kind ?? '';
    if (kind && kind !== lastKind && panel && !panel.contains(document.activeElement) && kind !== 'tx') panel.focus();
    lastKind = kind;
  });

  // Escape closes the panel when the focus is in it.
  function onkeydown(e: KeyboardEvent) {
    if (e.key === 'Escape' && panel?.contains(document.activeElement)) {
      e.stopPropagation();
      closeDetail();
    }
  }

  function open(page: Parameters<typeof go>[0], tab = '') {
    closeDetail();
    go(page, tab);
  }

  /** Open the page of the item, with the item selected in it. */
  function openAt(page: Parameters<typeof go>[0], tab: string, key: string) {
    closeDetail();
    goTo(page, tab, key);
  }
</script>

<svelte:window {onkeydown} />

{#if item}
  <aside class="detail-panel" aria-label="{TITLES[item.kind]} details" tabindex="-1" bind:this={panel}>
    <header>
      <h2>{TITLES[item.kind]}</h2>
      <button type="button" class="btn ghost icon small" aria-label="Close the details" title="Close (Esc)" onclick={closeDetail}><Icon name="x" size={16} /></button>
    </header>
    <div class="body">
      {#if error}
        <p class="error">{error}</p>
      {:else if item.kind === 'tx'}
        <TxDetailsView wallet={item.wallet} record={item} />
      {:else if item.kind === 'block'}
        {#if !block}<p class="muted">Loading…</p>{:else}
          <p class="big num">{formatCount(block.height)}</p>
          <dl class="facts">
            <dt>Hash</dt><dd><span class="mono break">{block.hash}</span> <CopyButton text={block.hash} label="Copy the block hash" /></dd>
            <dt>Time</dt><dd>{formatTime(block.time)} ({niceTimeOffset(Math.max(0, Math.floor(Date.now() / 1000) - block.time))} ago)</dd>
            <dt>Confirmations</dt><dd class="num">{formatCount(block.confirmations)}</dd>
            <dt>Transactions</dt><dd class="num">{formatCount(block.nTx)}</dd>
            <dt>Size</dt><dd>{formatBytes(block.size)} · weight {formatCount(block.weight)}</dd>
            <dt>Difficulty</dt><dd class="num">{block.difficulty.toPrecision(6)}</dd>
          </dl>
          <div class="nav">
            <button type="button" class="btn small" disabled={!block.previousblockhash} onclick={() => block?.previousblockhash && showDetail({ kind: 'block', hash: block.previousblockhash })}>← Previous</button>
            <button type="button" class="btn small" disabled={!block.nextblockhash} onclick={() => block?.nextblockhash && showDetail({ kind: 'block', hash: block.nextblockhash })}>Next →</button>
          </div>
          <h3>Transactions</h3>
          <ul class="ids">
            {#each block.tx.slice(0, 25) as txid (txid)}<li class="mono"><button type="button" class="mono linkish" title={txid} onclick={() => block && open('explorer', `tx:${txid}:${block.hash}`)}>{shortHash(txid, 12)}</button></li>{/each}
          </ul>
          {#if block.tx.length > 25}<p class="muted">and {formatCount(block.tx.length - 25)} more</p>{/if}
          <button type="button" class="btn" onclick={() => block && open('explorer', `search:${block.hash}`)}>Open in the block explorer</button>
        {/if}
      {:else if item.kind === 'peer'}
        {#if !peer}<p class="muted">Loading…</p>{:else}
          <p class="big mono break">{peer.addr}</p>
          <dl class="facts">
            <dt>Direction</dt><dd>{peer.inbound ? 'Inbound' : 'Outbound'} · {peer.connection_type}</dd>
            <dt>Network</dt><dd>{peer.network}</dd>
            <dt>User agent</dt><dd class="mono"><SafeText text={peer.subver} /></dd>
            <dt>Version</dt><dd>{peer.version}</dd>
            <dt>Starting block</dt><dd class="num">{formatCount(peer.startingheight)}</dd>
            <dt>Synced headers / blocks</dt><dd class="num">{formatCount(peer.synced_headers)} / {formatCount(peer.synced_blocks)}</dd>
            <dt>Ping</dt><dd>{peer.pingtime !== undefined ? `${Math.round(peer.pingtime * 1000)} ms` : '–'}{peer.minping !== undefined ? ` (min ${Math.round(peer.minping * 1000)} ms)` : ''}</dd>
            <dt>Sent / received</dt><dd>{formatBytes(peer.bytessent)} / {formatBytes(peer.bytesrecv)}</dd>
            <dt>Connected since</dt><dd>{formatTime(peer.conntime)}</dd>
          </dl>
          <button type="button" class="btn" onclick={() => peer && openAt('node', 'peers', String(peer.id))}>Open the peers tab</button>
        {/if}
      {:else if item.kind === 'sidechain'}
        {#if !sidechain}<p class="muted">Loading…</p>{:else}
          {@const s = sidechain.info}
          <p class="big"><SafeText text={s.title} /> <span class="badge">slot {s.slot}</span></p>
          <p class="muted"><SafeText text={s.description} /></p>
          <dl class="facts">
            <dt>Escrow</dt><dd>{#if s.escrow}<Amount sats={coinsToSats(s.escrow.amount)} />{:else}empty{/if}</dd>
            <dt>Active since block</dt><dd class="num">{formatCount(s.activationheight)}</dd>
            <dt>Version</dt><dd>{s.version}</dd>
            <dt>Release hash</dt><dd class="mono break">{s.hashid1}</dd>
            <dt>Commit hash</dt><dd class="mono break">{s.hashid2}</dd>
          </dl>
          <h3>Pending withdrawal bundles</h3>
          {#if sidechain.bundles.length === 0}
            <p class="muted">None.</p>
          {:else}
            <ul class="bundles">
              {#each sidechain.bundles as b (b.hash)}
                <li>
                  <span class="mono" title={b.hash}>{shortHash(b.hash, 8)}</span>
                  <span class="num">score {formatCount(b.score)}</span>
                  <span class="num muted">{formatCount(b.blocksleft)} left</span>
                  {#if b.payable}<span class="badge accent">payable</span>{/if}
                  <span class="badge">{b.vote}</span>
                </li>
              {/each}
            </ul>
          {/if}
          <div class="nav">
            <button type="button" class="btn" onclick={() => openAt('sidechains', 'sidechains', String(s.slot))}>Deposit</button>
            <button type="button" class="btn" onclick={() => openAt('sidechains', 'withdrawals', String(s.slot))}>Vote on bundles</button>
          </div>
        {/if}
      {/if}
    </div>
  </aside>
{/if}

<style>
  .detail-panel {
    display: flex;
    flex-direction: column;
    width: 25rem;
    flex: none;
    min-height: 0;
    margin: 0.75rem 0.75rem 0.75rem 0;
    border: 1px solid var(--line);
    border-radius: var(--radius-xl);
    background: linear-gradient(180deg, var(--card-top), transparent 10rem), var(--surface);
    box-shadow: var(--shadow-card);
    overflow: hidden;
    outline: none;
  }
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
    padding: 0.75rem 0.9rem 0.5rem 1.1rem;
    border-bottom: 1px solid var(--line);
  }
  h2 {
    font-size: 1rem;
  }
  h3 {
    margin-top: 0.4rem;
    font-size: 0.95rem;
  }
  .body {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 0.8rem;
    padding: 0.9rem 1.1rem 1.2rem;
  }
  /* Facts stack in the narrow panel: the name above its value. */
  .body :global(.facts) {
    grid-template-columns: minmax(0, 1fr);
    gap: 0.1rem;
  }
  .body :global(.facts dt) {
    font-size: 0.82rem;
    margin-top: 0.45rem;
  }
  .big {
    margin: 0;
    font-size: 1.35rem;
    font-weight: 650;
  }
  .nav {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
  }
  .ids,
  .bundles {
    margin: 0;
    padding: 0;
    list-style: none;
    font-size: 0.88rem;
  }
  .ids li {
    padding: 0.15rem 0;
    color: var(--text-muted);
  }
  .bundles li {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
    padding: 0.35rem 0;
    border-bottom: 1px solid var(--line);
  }
  .error {
    color: var(--negative);
  }
  @media (max-width: 900px) {
    .detail-panel {
      position: fixed;
      z-index: 35;
      top: 0;
      right: 0;
      bottom: 0;
      width: min(25rem, 100vw);
      margin: 0;
      border-radius: 0;
      box-shadow: var(--shadow);
    }
  }
  .linkish {
    padding: 0;
    border: 0;
    background: none;
    color: var(--link);
    cursor: pointer;
  }
  .linkish:hover,
  .linkish:focus-visible {
    text-decoration: underline;
  }
</style>
