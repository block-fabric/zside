<script lang="ts">
  // The network hash estimator of the Mine page (user request): the network's estimated hash rate over a
  // window of blocks (getnetworkhashps), the difficulty, a chart of the estimate over the last day; and,
  // for a hash rate the user gives (or this node's, while it mines), the share of the network, the time
  // to a block, and the blocks and coins of a day on average. Asked at a new block only; the estimates of
  // the blocks below the tip are kept (lib/hashrate.ts).
  import Amount from './Amount.svelte';
  import { errorText } from '../lib/dialogs.svelte';
  import { formatCount, niceTimeOffset } from '../lib/format';
  import { HASH_UNITS, blocksPerDay, chartHeights, estimatesAt, expectedSeconds, formatHashRate, hashParts, hashUnitOf, hashWindows, networkShare, parseHashRate, type HashUnit } from '../lib/hashrate';
  import { t } from '../lib/i18n.svelte';
  import { rpc } from '../lib/rpc';
  import { app, node } from '../lib/state.svelte';

  let { ownRate = null }: { ownRate?: number | null } = $props();

  interface MiningInfo {
    blocks: number;
    difficulty: number;
    next?: { height: number; difficulty: number };
  }

  const windows = $derived(hashWindows(app.info?.blockSpacing ?? 60));
  let windowId = $state('2h');
  const win = $derived(windows.find((w) => w.id === windowId) ?? windows[1]);
  // The chart: a day of blocks, a point every 20 blocks.
  const day = $derived(windows[3].blocks);
  const STEP = 20;

  let network = $state<number | null>(null);
  let difficulty = $state<number | null>(null);
  /** The reward of the tip's block, in satoshis (getblockstats). */
  let subsidy = $state<number | null>(null);
  let series = $state<{ heights: number[]; values: number[] }>({ heights: [], values: [] });
  let error = $state('');

  // A new block, or another window: asked again (never on a timer). The tip as a string: the status
  // of the node comes often, and only a new block counts.
  const tipKey = $derived(node.tip ? `${node.tip.height}:${node.tip.hash}` : '');
  $effect(() => {
    const tip = tipKey ? Number(tipKey.split(':')[0]) : -1;
    const blocks = win.blocks;
    const span = day;
    if (tip < 0) return;
    const controller = new AbortController();
    const { signal } = controller;
    (async () => {
      try {
        const info = await rpc.raw<MiningInfo>('getmininginfo', [], { signal });
        difficulty = info.next?.difficulty ?? info.difficulty;
        network = await rpc.raw<number>('getnetworkhashps', [blocks, -1], { signal });
        subsidy = tip > 0 ? await rpc.raw<{ subsidy: number }>('getblockstats', [tip, ['subsidy']], { signal }).then((s) => s.subsidy, () => null) : null;
        const heights = chartHeights(tip, span, STEP);
        const values = await estimatesAt(heights, blocks, tip, (n, h) => rpc.raw<number>('getnetworkhashps', [n, h], { signal }), signal);
        series = { heights, values };
        error = '';
      } catch (e) {
        if (!signal.aborted) error = errorText(e);
      }
    })();
    return () => controller.abort();
  });

  const big = $derived(network === null ? null : hashParts(network));

  // The rate of the user: typed, or this node's while it mines (until the user types).
  let text = $state('');
  let unit = $state<HashUnit>('TH/s');
  let touched = $state(false);
  $effect(() => {
    if (touched) return;
    if (ownRate && ownRate > 0) {
      const i = hashUnitOf(ownRate);
      unit = HASH_UNITS[i];
      text = String(Number((ownRate / 1000 ** i).toFixed(2)));
    } else text = '';
  });
  const parsed = $derived(text.trim() ? parseHashRate(text, unit) : null);
  function typed() {
    touched = true;
  }
  // A unit typed with the number ("100 TH/s") goes to the unit picker.
  function settle() {
    const p = parseHashRate(text, unit);
    if (p && (p.unit !== unit || /[a-df-z]/i.test(text))) {
      unit = p.unit;
      text = String(Number((p.rate / 1000 ** HASH_UNITS.indexOf(p.unit)).toPrecision(12)));
    }
  }

  const mine = $derived(parsed && parsed.rate > 0 ? parsed.rate : null);
  const share = $derived(mine !== null && network !== null ? networkShare(mine, network) : null);
  const secs = $derived(mine !== null && difficulty !== null ? expectedSeconds(difficulty, mine) : null);
  const perDay = $derived(mine !== null && difficulty !== null ? blocksPerDay(difficulty, mine) : null);
  const coinsPerDay = $derived(perDay !== null && subsidy !== null ? Math.round(perDay * subsidy) : null);

  function percent(x: number): string {
    if (x === 0) return '0%';
    if (x >= 0.9999995) return '100%';
    const p = x * 100;
    return `${p >= 1 ? p.toFixed(2) : p.toPrecision(3)}%`;
  }
  function count(x: number): string {
    return x >= 100 ? formatCount(Math.round(x)) : x >= 1 ? x.toFixed(2) : x.toPrecision(3);
  }

  // The chart: the estimate at each point, in a box of 600 × 120.
  const W = 600;
  const H = 120;
  const chart = $derived.by(() => {
    const v = series.values;
    if (v.length < 2) return null;
    const max = Math.max(...v);
    const min = Math.min(...v);
    const span = max - min || max || 1;
    const lo = Math.max(0, min - span * 0.1);
    const hi = max + span * 0.1;
    const pts = v.map((y, i) => [(i / (v.length - 1)) * W, H - 4 - ((y - lo) / (hi - lo || 1)) * (H - 8)]);
    const line = pts.map(([x, y], i) => `${i ? 'L' : 'M'}${x.toFixed(1)} ${y.toFixed(1)}`).join('');
    return { line, area: `${line}L${W} ${H}L0 ${H}Z`, max, min, from: series.heights[0], to: series.heights[series.heights.length - 1] };
  });
</script>

<section class="panel estimator" id="hash-estimator" aria-labelledby="hash-estimator-title">
  <div class="panel-head">
    <h2 id="hash-estimator-title">Network hash estimator</h2>
    <label class="window">
      <span class="muted">Over the</span>
      <select id="estimator-window" aria-label="Window of the estimate" bind:value={windowId}>
        {#each windows as w (w.id)}<option value={w.id}>{t(w.label)} ({w.blocks})</option>{/each}
      </select>
    </label>
  </div>
  <div class="panel-body">
    <div class="top">
      <div class="figure">
        <span class="muted">Estimated network hash rate</span>
        <strong class="big num" id="estimator-network">
          {#if big}{big.whole}<span class="dec">{big.decimals}</span><span class="unit">&nbsp;{big.unit}</span>{:else}–{/if}
        </strong>
        <span class="muted small">Difficulty <span class="num" id="estimator-difficulty">{difficulty === null ? '–' : difficulty.toPrecision(6)}</span></span>
      </div>
      {#if chart}
        <figure class="chart" aria-label="The estimate over blocks {chart.from} to {chart.to}: from {formatHashRate(chart.min)} to {formatHashRate(chart.max)}">
          <svg viewBox="0 0 {W} {H}" preserveAspectRatio="none" aria-hidden="true">
            <defs>
              <linearGradient id="estimator-fill" x1="0" x2="0" y1="0" y2="1">
                <stop class="top-stop" offset="0" />
                <stop class="bottom-stop" offset="1" />
              </linearGradient>
            </defs>
            <path class="area" d={chart.area} fill="url(#estimator-fill)" />
            <path class="line" d={chart.line} vector-effect="non-scaling-stroke" />
          </svg>
          <figcaption class="muted small"><span>Block {chart.from}</span><span>{formatHashRate(chart.max)} at most</span><span>Block {chart.to}</span></figcaption>
        </figure>
      {/if}
    </div>

    <div class="yours">
      <label for="estimator-rate">Your hash rate</label>
      <div class="row">
        <input id="estimator-rate" type="text" inputmode="decimal" class="num grow" spellcheck="false" autocomplete="off" placeholder="e.g. 100 TH/s" bind:value={text} oninput={typed} onchange={settle} />
        <select id="estimator-unit" aria-label="Unit of your hash rate" bind:value={unit} onchange={typed}>
          {#each HASH_UNITS as u (u)}<option value={u}>{u}</option>{/each}
        </select>
      </div>
      {#if text.trim() && !parsed}<p class="error" role="alert">Enter a hash rate, such as 100 TH/s.</p>{/if}
      <dl class="facts" id="estimator-results">
        <dt>Share of the network</dt><dd class="num" id="estimator-share">{share === null ? '–' : percent(share)}</dd>
        <dt>Expected time to find a block</dt><dd id="estimator-time">{secs === null ? '–' : !Number.isFinite(secs) ? t('never') : secs < 1 ? t('less than a second') : niceTimeOffset(Math.min(secs, 1e15))}</dd>
        <dt>Expected blocks per day</dt><dd class="num" id="estimator-blocks">{perDay === null ? '–' : count(perDay)}</dd>
        {#if subsidy !== null}<dt>Expected coins per day</dt><dd id="estimator-coins">{#if coinsPerDay === null}–{:else}<Amount sats={coinsPerDay} />{/if}</dd>{/if}
      </dl>
      <p class="muted small">Averages: finding a block is luck, and the network's rate is an estimate from the times of its recent blocks.</p>
    </div>
    {#if error}<p class="error" role="alert">{error}</p>{/if}
  </div>
</section>

<style>
  .estimator {
    grid-column: 1 / -1;
    scroll-margin-top: 1rem;
  }
  .window {
    display: inline-flex;
    align-items: center;
    gap: 0.5rem;
    white-space: nowrap;
  }
  .window select {
    width: auto;
  }
  .panel-body {
    display: flex;
    flex-direction: column;
    gap: 1.25rem;
  }
  .top {
    display: grid;
    grid-template-columns: minmax(14rem, auto) 1fr;
    gap: 1.5rem;
    align-items: center;
  }
  @media (max-width: 720px) {
    .top {
      grid-template-columns: 1fr;
    }
  }
  .figure {
    display: flex;
    flex-direction: column;
    gap: 0.25rem;
  }
  .big {
    font-size: 2.2rem;
    font-weight: 700;
    line-height: 1.1;
    white-space: nowrap;
  }
  .dec {
    color: var(--text-muted);
    font-weight: 450;
  }
  .unit {
    color: var(--text-muted);
    font-size: 0.55em;
    font-weight: 600;
  }
  .small {
    font-size: 0.85rem;
  }
  .chart {
    margin: 0;
    min-width: 0;
  }
  .chart svg {
    display: block;
    width: 100%;
    height: 7rem;
  }
  .top-stop {
    stop-color: var(--accent);
    stop-opacity: 0.28;
  }
  .bottom-stop {
    stop-color: var(--accent);
    stop-opacity: 0;
  }
  .line {
    fill: none;
    stroke: var(--accent);
    stroke-width: 2;
    stroke-linejoin: round;
  }
  figcaption {
    display: flex;
    justify-content: space-between;
    gap: 1rem;
    margin-top: 0.25rem;
  }
  .yours {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    max-width: 40rem;
  }
  .yours label {
    font-weight: 600;
  }
  .row {
    display: flex;
    flex-wrap: nowrap;
    gap: 0.5rem;
  }
  .grow {
    flex: 1;
    min-width: 0;
  }
  .row select {
    flex: none;
    width: 7.5rem;
  }
  .error {
    color: var(--negative);
  }
</style>
