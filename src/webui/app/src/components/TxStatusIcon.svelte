<script lang="ts">
  // The status of a transaction as a ring that fills with the confirmations (the transaction_N icons of chains-qt).
  import { t } from '../lib/i18n.svelte';
  import { confirmationProgress, statusText, type TxRecord } from '../lib/wallet';

  let { record, size = 18 }: { record: Pick<TxRecord, 'status' | 'depth' | 'maturesIn'>; size?: number } = $props();

  const progress = $derived(confirmationProgress(record));
  const R = 7;
  const C = 2 * Math.PI * R;
</script>

<svg class="status {record.status}" width={size} height={size} viewBox="0 0 18 18" role="img" aria-label={statusText(record)}>
  <title>{statusText(record)}</title>
  {#if record.status === 'conflicted' || record.status === 'notaccepted'}
    <circle cx="9" cy="9" r={R} class="track" />
    <path d="M6 6l6 6M12 6l-6 6" class="mark" />
  {:else if record.status === 'abandoned' || record.status === 'dropped'}
    <circle cx="9" cy="9" r={R} class="track" />
    <path d="M4.5 13.5l9-9" class="mark" />
  {:else if record.status === 'confirmed'}
    <circle cx="9" cy="9" r={R} class="done" />
    <path d="M5.5 9.3l2.3 2.3 4.7-4.9" class="tick" />
  {:else}
    <circle cx="9" cy="9" r={R} class="track" />
    <circle cx="9" cy="9" r={R} class="fill" stroke-dasharray="{C * progress} {C}" transform="rotate(-90 9 9)" />
    {#if record.status === 'unconfirmed'}<path d="M9 5.5V9l2 1.5" class="clock" />{/if}
  {/if}
</svg>

<style>
  svg {
    flex: none;
    display: block;
    fill: none;
    stroke-width: 2;
    stroke-linecap: round;
    stroke-linejoin: round;
  }
  .track {
    stroke: var(--line-strong);
  }
  .fill {
    stroke: var(--accent);
  }
  .immature .fill {
    stroke: var(--warning);
  }
  .done {
    stroke: var(--positive);
  }
  .tick {
    stroke: var(--positive);
  }
  .mark {
    stroke: var(--negative);
  }
  .clock {
    stroke: var(--text-muted);
    stroke-width: 1.5;
  }
</style>
