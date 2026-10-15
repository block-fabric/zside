<script lang="ts">
  import { every } from '../lib/poll';
  import { t } from '../lib/i18n.svelte';
  // Solo mining with the processor of this computer (MiningDialog of chains-qt): an address for the
  // rewards (new from the wallet), threads, start and stop, how the miner does, and the network.
  import HashEstimator from '../components/HashEstimator.svelte';
  import Icon from '../components/Icon.svelte';
  import { formatHashRate } from '../lib/hashrate';
  import { nav } from '../lib/nav.svelte';
  import { tick } from 'svelte';
  import { errorText, inform } from '../lib/dialogs.svelte';
  import { formatCount, niceTimeOffset } from '../lib/format';
  import { rpc } from '../lib/rpc';
  import { prefs, savePrefs, wallets, walletsEnabled } from '../lib/state.svelte';

  interface Generate {
    generate: boolean;
    threads: number;
    hashespersec: number;
    hashes: number;
    blocksfound: number;
    blocksrejected: number;
    cores: number;
  }
  interface MiningInfo {
    blocks: number;
    difficulty: number;
    networkhashps: number;
    pooledtx: number;
    chain: string;
    next: { height: number; difficulty: number };
  }

  let address = $state(prefs.miningAddress);
  let threads = $state(prefs.miningThreads);
  let gen = $state<Generate | null>(null);
  let info = $state<MiningInfo | null>(null);
  let error = $state('');
  let busy = $state(false);

  async function refresh() {
    try {
      [gen, info] = await Promise.all([rpc.raw<Generate>('getgenerate'), rpc.raw<MiningInfo>('getmininginfo')]);
      error = '';
      if (gen.generate) threads = gen.threads;
    } catch (e) {
      error = errorText(e);
    }
  }
  // Every 2 seconds (MiningDialog REFRESH_INTERVAL_MS).
  $effect(() => {
    return every(2000, refresh, { now: true });
  });

  const running = $derived(!!gen?.generate);
  const maxThreads = $derived(Math.max(1, gen?.cores ?? 1));

  async function newAddress(): Promise<boolean> {
    const wallet = wallets.current;
    if (wallet === null) {
      await inform(t('Solo Mine'), walletsEnabled() ? t('Open or create a wallet first, or enter an address.') : t('Enter an address: this node runs without wallets (-disablewallet).'));
      return false;
    }
    try {
      address = await rpc.call('getnewaddress', ['mining'], { wallet });
      return true;
    } catch (e) {
      await inform(t('Solo Mine'), errorText(e), 'warning');
      return false;
    }
  }

  async function start() {
    if (busy) return;
    if (!address.trim() && !(await newAddress())) return;
    busy = true;
    try {
      await rpc.raw('setgenerate', [true, address.trim(), threads]);
      savePrefs({ miningAddress: address.trim(), miningThreads: threads });
    } catch (e) {
      await inform(t('Solo Mine'), errorText(e), 'warning');
    } finally {
      busy = false;
      refresh();
    }
  }

  async function stop() {
    busy = true;
    try {
      await rpc.raw('setgenerate', [false]);
    } catch (e) {
      await inform(t('Solo Mine'), errorText(e), 'warning');
    } finally {
      busy = false;
      refresh();
    }
  }

  /** Hash rate with a unit (NodeRpc::HashRate). */
  const hashRate = (h: number) => formatHashRate(h, 1);

  // Opened on the estimator (the Network line of the Home's Mining card): shown at once.
  $effect(() => {
    if (nav.tab !== 'estimator') return;
    tick().then(() => document.getElementById('hash-estimator')?.scrollIntoView({ block: 'start' }));
  });

  // A block takes difficulty * 2^32 hashes on average.
  const expected = $derived.by(() => {
    if (!running || !gen || !info || gen.hashespersec <= 0) return '–';
    const secs = (info.next.difficulty * 4294967296) / gen.hashespersec;
    return secs < 1 ? 'less than a second' : niceTimeOffset(Math.min(secs, 1e15));
  });
</script>

<div class="page mine">
  <header class="page-head"><h1>Solo Mine</h1></header>
  <p class="muted">
    Mine blocks with the processor of this computer. Rewards are paid to the address below, and can be spent once they have matured. Mining with a processor only finds blocks while the difficulty of the network is
    low.
  </p>

  <div class="layout">
    <section class="panel">
      <div class="panel-head"><h2>Miner</h2><span class="state" class:on={running}><span class="dot"></span>{running ? `Mining, threads: ${gen?.threads}` : 'Not mining'}</span></div>
      <form class="panel-body form" onsubmit={(e) => { e.preventDefault(); start(); }}>
        <div class="field">
          <label for="mining-address">Pay to</label>
          <div class="row">
            <input id="mining-address" type="text" class="mono grow" spellcheck="false" autocomplete="off" placeholder="Address that receives the block rewards" bind:value={address} disabled={running} />
            <button type="button" class="btn" title="Take a new address from the open wallet" disabled={running} onclick={newAddress}>New address</button>
          </div>
        </div>
        <div class="field">
          <label for="mining-threads">Threads</label>
          <div class="row">
            {#key maxThreads}<input id="mining-threads" type="range" min="1" max={maxThreads} step="1" bind:value={threads} disabled={running} />{/key}
            <span class="num threads">{threads} of {maxThreads}</span>
          </div>
        </div>
        <div class="row">
          <button type="submit" class="btn primary" disabled={running || busy}><Icon name="mine" size={16} /> Start mining</button>
          <button type="button" class="btn" disabled={!running || busy} onclick={stop}>Stop mining</button>
        </div>
        {#if error}<p class="error" role="alert">{error}</p>{/if}
      </form>
    </section>

    <section class="panel">
      <div class="panel-head"><h2>This miner</h2></div>
      <div class="panel-body tiles">
        <div class="tile"><span class="muted">Hash rate</span><strong class="num" id="mining-hashrate">{running && gen ? hashRate(gen.hashespersec) : '–'}</strong></div>
        <div class="tile"><span class="muted">Expected time per block</span><strong>{expected}</strong></div>
        <div class="tile"><span class="muted">Blocks found</span><strong class="num" id="mining-found">{gen ? gen.blocksfound : '–'}</strong>{#if gen && gen.blocksrejected}<span class="bad">and {gen.blocksrejected} not accepted</span>{/if}</div>
        <div class="tile"><span class="muted">Hashes</span><strong class="num">{gen ? formatCount(gen.hashes) : '–'}</strong></div>
      </div>
    </section>

    <section class="panel">
      <div class="panel-head"><h2>Network</h2></div>
      <dl class="facts panel-body">
        <dt>Chain</dt><dd>{info?.chain ?? '–'}</dd>
        <dt>Block height</dt><dd class="num" id="mining-height">{info ? formatCount(info.blocks) : '–'}</dd>
        <dt>Difficulty of the next block</dt><dd class="num">{info ? info.next.difficulty.toPrecision(10) : '–'}</dd>
        <dt>Network hash rate</dt><dd class="num">{info ? hashRate(info.networkhashps) : '–'}</dd>
        <dt>Transactions waiting</dt><dd class="num">{info ? formatCount(info.pooledtx) : '–'}</dd>
      </dl>
    </section>

    <HashEstimator ownRate={running && gen ? gen.hashespersec : null} />
  </div>
</div>

<style>
  .layout {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(24rem, 100%), 1fr));
    gap: 1rem;
    margin-top: 1rem;
  }
  .form {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  input[type='range'] {
    flex: 1;
    accent-color: var(--accent);
  }
  .threads {
    min-width: 5rem;
    text-align: right;
  }
  .state {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    color: var(--text-muted);
    font-weight: 600;
  }
  .state .dot {
    width: 0.6rem;
    height: 0.6rem;
    border-radius: 50%;
    background: var(--text-faint);
  }
  .state.on {
    color: var(--positive);
  }
  .state.on .dot {
    background: var(--positive);
    animation: pulse 1.4s ease-in-out infinite;
  }
  @keyframes pulse {
    50% {
      opacity: 0.3;
    }
  }
  .tiles {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 1rem;
  }
  .tile {
    display: flex;
    flex-direction: column;
    gap: 0.2rem;
  }
  .tile strong {
    font-size: 1.3rem;
  }
  .bad,
  .error {
    color: var(--negative);
  }
</style>
