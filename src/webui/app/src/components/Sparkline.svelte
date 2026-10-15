<script lang="ts">
  // A small line of a few values over a gradient: the trend beside a row (the balance before a
  // transaction, the blocks that merged mined a sidechain). Decorative: what it shows is in the row.
  let {
    values,
    width = 92,
    height = 28,
    tone = 'auto',
  }: { values: number[]; width?: number; height?: number; tone?: 'auto' | 'up' | 'down' | 'flat' } = $props();

  const id = `spark-${Math.random().toString(36).slice(2)}`;
  const kind = $derived(tone !== 'auto' ? tone : values.length < 2 || values[values.length - 1] === values[0] ? 'flat' : values[values.length - 1] > values[0] ? 'up' : 'down');
  const path = $derived.by(() => {
    if (values.length < 2) return { line: '', area: '' };
    const min = Math.min(...values);
    const max = Math.max(...values);
    const span = max - min || 1;
    const pts = values.map((v, i) => [2 + (i / (values.length - 1)) * (width - 4), 3 + (1 - (v - min) / span) * (height - 6)]);
    const line = pts.map(([x, y], i) => `${i ? 'L' : 'M'}${x.toFixed(1)} ${y.toFixed(1)}`).join('');
    return { line, area: `${line}L${width - 2} ${height}L2 ${height}Z` };
  });
</script>

{#if path.line}
  <svg class="spark {kind}" {width} {height} viewBox="0 0 {width} {height}" aria-hidden="true">
    <defs>
      <linearGradient {id} x1="0" x2="0" y1="0" y2="1">
        <stop class="top" offset="0" />
        <stop class="bottom" offset="1" />
      </linearGradient>
    </defs>
    <path class="area" d={path.area} fill="url(#{id})" />
    <path class="line" d={path.line} />
  </svg>
{:else}
  <span class="spark-none" aria-hidden="true"></span>
{/if}

<style>
  .spark {
    display: block;
    flex: none;
    --c: var(--text-faint);
  }
  .up {
    --c: var(--positive);
  }
  .down {
    --c: var(--negative);
  }
  .top {
    stop-color: var(--c);
    stop-opacity: 0.3;
  }
  .bottom {
    stop-color: var(--c);
    stop-opacity: 0;
  }
  .line {
    fill: none;
    stroke: var(--c);
    stroke-width: 1.6;
    stroke-linejoin: round;
    stroke-linecap: round;
  }
  .spark-none {
    display: block;
    width: 92px;
    flex: none;
  }
</style>
