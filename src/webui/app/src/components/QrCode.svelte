<script lang="ts">
  // A QR code, drawn as one SVG path (no image, nothing from elsewhere).
  import { encode } from 'uqr';

  let { text, size = 220, label = 'QR code' }: { text: string; size?: number; label?: string } = $props();

  const qr = $derived.by(() => {
    try {
      return encode(text, { ecc: 'M', border: 2 });
    } catch {
      return null;
    }
  });

  const path = $derived.by(() => {
    if (!qr) return '';
    let d = '';
    qr.data.forEach((row, y) => {
      row.forEach((on, x) => {
        if (on) d += `M${x} ${y}h1v1h-1z`;
      });
    });
    return d;
  });
</script>

{#if qr}
  <svg class="qr" width={size} height={size} viewBox="0 0 {qr.size} {qr.size}" role="img" aria-label={label} shape-rendering="crispEdges">
    <rect width={qr.size} height={qr.size} fill="#ffffff" />
    <path d={path} fill="#000000" />
  </svg>
{:else}
  <p class="error">The text is too long for a QR code.</p>
{/if}

<style>
  .qr {
    display: block;
    border-radius: var(--radius-s);
  }
</style>
