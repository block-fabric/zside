<script lang="ts">
  // The balance of the wallet over time, as a step line over a gradient (an addition to the overview
  // of chains-qt), the balance now a lit point at its end. One series: no legend; a crosshair with
  // the date and the balance under the pointer. With `axes`, the dates below and the amounts at the
  // right; with `from`, only the time since then (the timeframes of the dashboard).
  import Amount from './Amount.svelte';
  import { css } from '../lib/actions';
  import { formatTime } from '../lib/format';
  import { prefs } from '../lib/state.svelte';
  import { formatAmount, unitInfo } from '../lib/units';

  let {
    points,
    height = 150,
    axes = false,
    from = undefined,
  }: { points: { time: number; balance: bigint }[]; height?: number; axes?: boolean; from?: number } = $props();

  const gradient = `chart-fill-${Math.random().toString(36).slice(2)}`;

  let box: HTMLDivElement | undefined = $state();
  let width = $state(480);
  let hover: number | null = $state(null);

  $effect(() => {
    if (!box || typeof ResizeObserver === 'undefined') return;
    const o = new ResizeObserver(() => box && (width = box.clientWidth));
    o.observe(box);
    width = box.clientWidth || width;
    return () => o.disconnect();
  });

  const PAD = $derived({ l: 6, r: axes ? 64 : 10, t: 12, b: axes ? 24 : 20 });
  const now = Math.floor(Date.now() / 1000);
  /** The points since `from`: the balance at that time first. */
  const shown = $derived.by(() => {
    if (from === undefined || !points.length || points[0].time >= from) return points;
    let before = points[0];
    const after: typeof points = [];
    for (const p of points) {
      if (p.time < from) before = p;
      else after.push(p);
    }
    return [{ time: from, balance: before.balance }, ...after];
  });
  const data = $derived(shown.length ? [...shown, { time: Math.max(now, shown[shown.length - 1].time), balance: shown[shown.length - 1].balance }] : []);
  const t0 = $derived(data.length ? data[0].time : 0);
  const t1 = $derived(data.length ? Math.max(data[data.length - 1].time, t0 + 1) : 1);
  const maxB = $derived(data.reduce((m, p) => (p.balance > m ? p.balance : m), 1n));
  const minB = $derived(data.reduce((m, p) => (p.balance < m ? p.balance : m), 0n));
  /** The amounts at the right: the bottom, the middle, the top. */
  const yTicks = $derived(axes ? [minB, minB + (maxB - minB) / 2n, maxB] : []);
  /** The dates below: five, from the first to now. */
  const xTicks = $derived(axes && data.length ? [0, 0.25, 0.5, 0.75, 1].map((f) => Math.round(t0 + f * (t1 - t0))) : []);
  function shortAmount(v: bigint): string {
    const coins = Number(v) / 10 ** unitInfo(prefs.unit).decimals;
    if (coins >= 1e6) return `${(coins / 1e6).toFixed(1)}M`;
    if (coins >= 1e3) return `${(coins / 1e3).toFixed(1)}k`;
    return coins >= 10 ? coins.toFixed(0) : coins.toFixed(2);
  }
  function shortDate(t: number): string {
    const span = t1 - t0;
    const d = new Date(t * 1000);
    if (span < 2 * 86400) return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
    return d.toLocaleDateString('en', { month: 'short', day: 'numeric', ...(span > 300 * 86400 ? { year: '2-digit' } : {}) });
  }
  const x = (t: number) => PAD.l + ((t - t0) / (t1 - t0)) * (width - PAD.l - PAD.r);
  const y = (b: bigint) => PAD.t + (1 - Number(b - minB) / Number(maxB - minB || 1n)) * (height - PAD.t - PAD.b);

  const line = $derived.by(() => {
    if (!data.length) return '';
    let d = `M${x(data[0].time)} ${y(data[0].balance)}`;
    for (let i = 1; i < data.length; i++) d += `H${x(data[i].time)}V${y(data[i].balance)}`;
    return d;
  });
  const area = $derived(data.length ? `${line}V${y(minB)}H${x(data[0].time)}Z` : '');

  function onmove(e: PointerEvent) {
    if (!box || !data.length) return;
    const r = box.getBoundingClientRect();
    const t = t0 + ((e.clientX - r.left - PAD.l) / (width - PAD.l - PAD.r)) * (t1 - t0);
    let i = 0;
    while (i + 1 < data.length && data[i + 1].time <= t) i++;
    hover = i;
  }

  const hovered = $derived(hover !== null ? data[hover] : null);
  const label = $derived(data.length ? `Balance over time, from ${formatTime(t0)} to now: ${prefs.mask ? 'hidden' : formatAmount(data[data.length - 1].balance, prefs.unit, { withUnit: true })}` : 'No balance history');
</script>

<div class="chart" bind:this={box} onpointermove={onmove} onpointerleave={() => (hover = null)} role="img" aria-label={label}>
  {#if data.length < 2}
    <p class="muted empty">The chart starts with the first transaction.</p>
  {:else}
    {@const last = data[data.length - 1]}
    <svg {width} {height} viewBox="0 0 {width} {height}" aria-hidden="true">
      <defs>
        <linearGradient id={gradient} x1="0" x2="0" y1="0" y2="1">
          <stop class="stop-top" offset="0" />
          <stop class="stop-bottom" offset="1" />
        </linearGradient>
      </defs>
      {#each yTicks as v, i (i)}
        <line class="grid" x1={PAD.l} x2={width - PAD.r} y1={y(v)} y2={y(v)} />
        {#if !prefs.mask}<text class="tick" x={width - PAD.r + 10} y={y(v) + 4}>{shortAmount(v)}</text>{/if}
      {/each}
      {#if !axes}<line class="base" x1={PAD.l} x2={width - PAD.r} y1={y(minB)} y2={y(minB)} />{/if}
      <path class="area" d={area} fill="url(#{gradient})" />
      <path class="line" d={line} />
      {#if axes}
        {#each xTicks as t, i (i)}
          <text class="tick" class:end={i === xTicks.length - 1} class:mid={i > 0 && i < xTicks.length - 1} x={x(t)} y={height - 6}>{i === xTicks.length - 1 ? 'now' : shortDate(t)}</text>
        {/each}
      {:else}
        <text class="tick" x={PAD.l} y={height - 5}>{formatTime(t0).slice(0, 10)}</text>
        <text class="tick end" x={width - PAD.r} y={height - 5}>now</text>
      {/if}
      <circle class="halo" cx={x(last.time)} cy={y(last.balance)} r="9" />
      <circle class="now" cx={x(last.time)} cy={y(last.balance)} r="4.5" />
      {#if hovered}
        <line class="cross" x1={x(hovered.time)} x2={x(hovered.time)} y1={PAD.t} y2={height - PAD.b} />
        <circle class="dot" cx={x(hovered.time)} cy={y(hovered.balance)} r="4" />
      {/if}
    </svg>
    {#if hovered}
      <div class="tip" use:css={{ left: `${Math.min(Math.max(x(hovered.time), 70), width - 70)}px` }}>
        <span class="muted">{formatTime(hovered.time)}</span>
        <strong><Amount sats={hovered.balance} /></strong>
      </div>
    {/if}
  {/if}
</div>

<style>
  .chart {
    position: relative;
    width: 100%;
    min-height: 4rem;
  }
  svg {
    display: block;
  }
  .base {
    stroke: var(--line);
    stroke-width: 1;
  }
  .stop-top {
    stop-color: var(--chart-fill);
  }
  .stop-bottom {
    stop-color: var(--surface);
    stop-opacity: 0;
  }
  .grid {
    stroke: var(--line);
    stroke-width: 1;
    stroke-dasharray: 2 4;
  }
  .line {
    fill: none;
    stroke: var(--accent);
    stroke-width: 2.2;
    stroke-linejoin: round;
    stroke-linecap: round;
    filter: drop-shadow(0 0 5px color-mix(in srgb, var(--accent) 45%, transparent));
  }
  .halo {
    fill: var(--accent);
    opacity: 0.22;
  }
  .now {
    fill: var(--accent);
    stroke: var(--surface);
    stroke-width: 2;
    filter: drop-shadow(0 0 6px var(--accent));
  }
  .cross {
    stroke: var(--line-strong);
    stroke-width: 1;
    stroke-dasharray: 3 3;
  }
  .dot {
    fill: var(--accent);
    stroke: var(--surface);
    stroke-width: 2;
  }
  .tick {
    fill: var(--text-faint);
    font-size: 11px;
  }
  .tick.end {
    text-anchor: end;
  }
  .tick.mid {
    text-anchor: middle;
  }
  .tip {
    position: absolute;
    top: -0.4rem;
    transform: translate(-50%, -100%);
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 0.3rem 0.55rem;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-s);
    background: var(--surface);
    box-shadow: var(--shadow);
    font-size: 0.85rem;
    white-space: nowrap;
    pointer-events: none;
  }
  .empty {
    margin: 0;
    padding: 1rem 0;
  }
</style>
