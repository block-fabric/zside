<script lang="ts">
  // The network traffic tab (TrafficGraphWidget): rates in and out over a range of 5 minutes to a day,
  // the totals, and Clear.
  import { every } from '../lib/poll';
  import { t } from '../lib/i18n.svelte';
  import { formatBytes } from '../lib/format';
  import { prefs, savePrefs } from '../lib/state.svelte';
  import { clearGraph, MAX_RANGE_MINUTES, MIN_RANGE_MINUTES, rangeText, rates, traffic } from '../lib/traffic.svelte';

  const W = 1000;
  const H = 300;
  let now = $state(Date.now());
  $effect(() => {
    return every(1000, () => (now = Date.now()));
  });

  const points = $derived(rates(traffic.samples, prefs.trafficRange, now, traffic.clearedAt));
  const peak = $derived(Math.max(1, ...points.map((p) => Math.max(p.inRate, p.outRate))));
  // A scale with round steps (TrafficGraphWidget: the max rounded up).
  const scale = $derived.by(() => {
    const exp = Math.pow(10, Math.floor(Math.log10(peak)));
    for (const m of [1, 2, 5, 10]) if (m * exp >= peak) return m * exp;
    return 10 * exp;
  });
  const span = $derived(prefs.trafficRange * 60);

  function path(key: 'inRate' | 'outRate', area: boolean): string {
    if (points.length === 0) return '';
    const xy = points.map((p) => `${(W - (p.age / span) * W).toFixed(1)},${(H - (p[key] / scale) * H).toFixed(1)}`);
    const line = `M${xy.join(' L')}`;
    if (!area) return line;
    const first = xy[0].split(',')[0];
    const last = xy[xy.length - 1].split(',')[0];
    return `${line} L${last},${H} L${first},${H} Z`;
  }

  function rateText(kbs: number): string {
    return kbs >= 1000 ? t('%1 MB/s', (kbs / 1000).toFixed(1)) : t('%1 kB/s', kbs.toFixed(kbs < 10 ? 2 : 1));
  }
  const latest = $derived(points.length ? points[points.length - 1] : null);
</script>

<section class="panel traffic">
  <div class="graph">
    <svg viewBox="0 0 {W} {H}" preserveAspectRatio="none" role="img" aria-label="Network traffic, received and sent, over the last {rangeText(prefs.trafficRange)}">
      {#each [0.25, 0.5, 0.75] as f (f)}<line class="grid" x1="0" x2={W} y1={H * f} y2={H * f} />{/each}
      <path class="area in" d={path('inRate', true)} />
      <path class="area out" d={path('outRate', true)} />
      <path class="line in" d={path('inRate', false)} />
      <path class="line out" d={path('outRate', false)} />
    </svg>
    <span class="scale num">{rateText(scale)}</span>
    {#if points.length === 0}<span class="empty muted">Collecting samples…</span>{/if}
  </div>
  <div class="controls">
    <label class="range">
      <span class="muted">Range</span>
      <input type="range" min={MIN_RANGE_MINUTES} max={MAX_RANGE_MINUTES} step="5" value={prefs.trafficRange} oninput={(e) => savePrefs({ trafficRange: Number((e.currentTarget as HTMLInputElement).value) })} aria-label="Range of the graph" />
      <span class="num">{rangeText(prefs.trafficRange)}</span>
    </label>
    <button type="button" class="btn small" onclick={() => clearGraph()}>Clear</button>
  </div>
  <dl class="totals">
    <div><dt><span class="dot in"></span>Received</dt><dd class="num">{formatBytes(traffic.recv)}</dd><dd class="rate num">{latest ? rateText(latest.inRate) : '–'}</dd></div>
    <div><dt><span class="dot out"></span>Sent</dt><dd class="num">{formatBytes(traffic.sent)}</dd><dd class="rate num">{latest ? rateText(latest.outRate) : '–'}</dd></div>
  </dl>
</section>

<style>
  .traffic {
    padding: 1rem 1.1rem;
    display: grid;
    gap: 0.9rem;
  }
  .graph {
    position: relative;
    height: 18rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-s);
    background: var(--bg);
    overflow: hidden;
  }
  svg {
    width: 100%;
    height: 100%;
    display: block;
  }
  .grid {
    stroke: var(--line);
    stroke-dasharray: 4 6;
    vector-effect: non-scaling-stroke;
  }
  .line {
    fill: none;
    stroke-width: 2;
    vector-effect: non-scaling-stroke;
  }
  .line.in {
    stroke: var(--positive);
  }
  .line.out {
    stroke: var(--accent);
  }
  .area.in {
    fill: color-mix(in srgb, var(--positive) 18%, transparent);
  }
  .area.out {
    fill: color-mix(in srgb, var(--accent) 14%, transparent);
  }
  .scale {
    position: absolute;
    top: 0.4rem;
    left: 0.6rem;
    font-size: 0.8rem;
    color: var(--text-muted);
  }
  .empty {
    position: absolute;
    inset: 0;
    display: grid;
    place-items: center;
  }
  .controls {
    display: flex;
    align-items: center;
    gap: 1rem;
    flex-wrap: wrap;
  }
  .range {
    display: flex;
    align-items: center;
    gap: 0.6rem;
  }
  .range input {
    width: 18rem;
    accent-color: var(--accent);
  }
  .totals {
    display: flex;
    gap: 2.5rem;
    margin: 0;
  }
  .totals div {
    display: grid;
    grid-template-columns: auto auto;
    gap: 0.1rem 0.8rem;
  }
  .totals dt {
    grid-column: 1 / -1;
    display: flex;
    align-items: center;
    gap: 0.4rem;
    color: var(--text-muted);
  }
  .totals dd {
    margin: 0;
    font-size: 1.15rem;
    font-weight: 600;
  }
  .totals .rate {
    font-size: 0.9rem;
    color: var(--text-muted);
    align-self: end;
  }
  .dot {
    width: 0.7rem;
    height: 0.7rem;
    border-radius: 50%;
  }
  .dot.in {
    background: var(--positive);
  }
  .dot.out {
    background: var(--accent);
  }
</style>
