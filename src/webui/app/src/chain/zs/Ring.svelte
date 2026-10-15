<script lang="ts">
  // A ring that fills to a share, the share written inside (how much of the wallet is shielded).

  let { share, label, size = 84 }: { share: number; label: string; size?: number } = $props();

  const r = 34;
  const length = 2 * Math.PI * r;
  const clamped = $derived(Number.isFinite(share) ? Math.max(0, Math.min(1, share)) : 0);

  /** The filled length, through the CSS object model (no inline style: the content security policy). */
  function dash(node: SVGElement, offset: number) {
    const set = (o: number) => node.style.setProperty('stroke-dashoffset', String(o));
    set(offset);
    return { update: set };
  }
</script>

<div class="ring" role="img" aria-label={label}>
  <svg class="zs-ring" width={size} height={size} viewBox="0 0 84 84" aria-hidden="true">
    <circle class="track" cx="42" cy="42" {r} />
    <circle class="fill" cx="42" cy="42" {r} stroke-dasharray={length} use:dash={length * (1 - clamped)} transform="rotate(-90 42 42)" />
  </svg>
  <span class="pct num">{Math.round(clamped * 100)}%</span>
</div>

<style>
  .ring {
    position: relative;
    display: inline-grid;
    place-items: center;
    flex: none;
  }
  .ring svg {
    grid-area: 1 / 1;
  }
  .pct {
    grid-area: 1 / 1;
    font-weight: 700;
    font-size: 1.05rem;
  }
</style>
