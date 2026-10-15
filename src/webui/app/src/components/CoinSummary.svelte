<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  // The estimate of a transaction made of the coins chosen (the coin control labels of chains-qt):
  // quantity, bytes, amount, fee, after fee, change; each can be copied.
  import Amount from './Amount.svelte';
  import { host } from '../lib/rpc';
  import { prefs, toast } from '../lib/state.svelte';
  import { formatAmount } from '../lib/units';
  import type { CoinSummary } from '../lib/wallet';

  let { summary }: { summary: CoinSummary | null } = $props();

  const vary = $derived(summary ? `Can vary +/- ${Number(summary.feeVary.toFixed(3))} satoshi(s) per input.` : '');
  const approx = $derived(summary !== null && summary.fee > 0n);

  async function copy(text: string) {
    await host.copy(text).catch(() => undefined);
    toast('success', t('Copied'), text);
  }
  const plain = (v: bigint) => formatAmount(v, prefs.unit, { separators: false });
</script>

<div class="summary" aria-label="Coins chosen">
  {#if summary}
    <button type="button" class="cell" title="Copy quantity" onclick={() => copy(String(summary.quantity))}><span>Quantity</span><strong class="num">{summary.quantity}</strong></button>
    <button type="button" class="cell" title="Copy bytes" onclick={() => copy(String(summary.bytes))}><span>Bytes</span><strong class="num">{summary.bytes > 0 ? '≈' : ''}{summary.bytes}</strong></button>
    <button type="button" class="cell" title="Copy amount" onclick={() => copy(plain(summary.amount))}><span>Amount</span><strong><Amount sats={summary.amount} /></strong></button>
    <button type="button" class="cell" title={`${vary} Copy fee`} onclick={() => copy(plain(summary.fee))}><span>Fee</span><strong>{approx ? '≈' : ''}<Amount sats={summary.fee} /></strong></button>
    <button type="button" class="cell" title={`${vary} Copy after fee`} onclick={() => copy(plain(summary.afterFee))}><span>After fee</span><strong>{approx ? '≈' : ''}<Amount sats={summary.afterFee} /></strong></button>
    <button type="button" class="cell" class:off={!summary.changeEnabled} title={`${vary} Copy change`} onclick={() => copy(plain(summary.change))}>
      <span>Change</span><strong>{approx && summary.change > 0n ? '≈' : ''}<Amount sats={summary.change} /></strong>
    </button>
    {#if summary.insufficient}<p class="insufficient" role="alert">(insufficient funds)</p>{/if}
  {:else}
    <p class="muted auto">Coins are chosen automatically.</p>
  {/if}
</div>

<style>
  .summary {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(10rem, 100%), 1fr));
    gap: 0.4rem;
  }
  .cell {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 0.1rem;
    padding: 0.4rem 0.6rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-s);
    background: var(--surface-2);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: copy;
  }
  .cell span {
    color: var(--text-muted);
    font-size: 0.8rem;
  }
  .cell {
    min-width: 0;
    overflow: hidden;
  }
  .cell strong {
    max-width: 100%;
    font-weight: 600;
    overflow-wrap: anywhere;
  }
  /* A long amount wraps inside its box rather than spilling out of it. */
  .cell strong :global(*) {
    white-space: normal;
  }
  .cell.off {
    opacity: 0.55;
  }
  .insufficient {
    grid-column: 1 / -1;
    margin: 0;
    color: var(--negative);
    font-weight: 600;
  }
  .auto {
    grid-column: 1 / -1;
    margin: 0;
  }
</style>
