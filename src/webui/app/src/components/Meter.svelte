<script lang="ts">
  // A bar that fills to a share (acks of a proposal, the score of a bundle), with a mark where it
  // passes when there is one. Drawn without inline styles (the content security policy).
  import { css } from '../lib/actions';

  let {
    value,
    max,
    label,
    tone = 'accent',
    text = '',
  }: { value: number; max: number; label: string; tone?: 'accent' | 'positive' | 'warning' | 'negative' | 'muted'; text?: string } = $props();

  const pct = $derived(max > 0 ? Math.max(0, Math.min(100, (value / max) * 100)) : 0);
</script>

<div class="meter" role="progressbar" aria-label={label} aria-valuemin="0" aria-valuemax={max} aria-valuenow={value} aria-valuetext={text || `${value} of ${max}`}>
  <div class="track"><span class="fill {tone}" use:css={{ width: `${pct}%` }}></span></div>
  {#if text}<span class="text num">{text}</span>{/if}
</div>

<style>
  .meter {
    display: flex;
    align-items: center;
    gap: 0.55rem;
    min-width: 7rem;
  }
  .track {
    position: relative;
    flex: 1;
    height: 0.5rem;
    border-radius: 999px;
    background: var(--raised);
    overflow: hidden;
  }
  .fill {
    position: absolute;
    inset: 0 auto 0 0;
    border-radius: 999px;
    background: var(--accent);
    transition: width 0.3s ease;
  }
  .fill.positive {
    background: var(--positive);
  }
  .fill.warning {
    background: var(--warning);
  }
  .fill.negative {
    background: var(--negative);
  }
  .fill.muted {
    background: var(--text-faint);
  }
  .text {
    font-size: 0.82rem;
    color: var(--text-muted);
    white-space: nowrap;
  }
</style>
