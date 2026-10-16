<script lang="ts">
  // The details of a transaction of the wallet (TransactionDesc::toHTML): in the dialog of the
  // transactions page, or in the detail panel of the workspace layout.
  import { t } from '../lib/i18n.svelte';
  import SafeText from './SafeText.svelte';
  import Amount from './Amount.svelte';
  import CopyButton from './CopyButton.svelte';
  import { formatTime } from '../lib/format';
  import { walletApi, type DetailRow, type TxDetails, type TxRecord } from '../lib/wallet';
  import { COINBASE_ROLES } from '../chain/roles';

  let { wallet, record }: { wallet: string; record: Pick<TxRecord, 'txid' | 'idx' | 'address'> } = $props();

  let details: TxDetails | null = $state(null);
  let error = $state('');

  $effect(() => {
    const r = record;
    details = null;
    error = '';
    walletApi(wallet)
      .transaction(r.txid, r.idx, r.address)
      .then((d) => (details = d))
      .catch((e) => (error = e instanceof Error ? e.message : String(e)));
  });
</script>

{#snippet value(row: DetailRow)}
  {#if row.address !== undefined}
    {#if row.label}<SafeText text={row.label} />{/if}
    <span class="mono break">{row.address}</span>
    {#if row.own}<span class="badge accent">own address</span>{/if}
  {:else if row.sats !== undefined}
    <Amount sats={row.sats} plus={row.plus} colored={row.k === 'Net amount'} />
    {#if row.note}<span class="muted">(<SafeText text={row.note} />)</span>{/if}
  {:else if row.time !== undefined}
    {row.time ? formatTime(row.time) : ''}
  {:else if row.k === 'Status'}
    <span class="break"><SafeText text={row.text ?? ''} /></span>
  {:else if row.k === 'Source' && row.role && COINBASE_ROLES[row.role]}
    <!-- What the output pays, in the chain's words (chain/roles.ts). -->
    <span class="break">{t(COINBASE_ROLES[row.role].source)}</span>
  {:else}
    <SafeText class="break" text={row.text} />
  {/if}
{/snippet}

{#if error}
  <p class="error">{error}</p>
{:else if !details}
  <p class="muted">Loading…</p>
{:else}
  <div class="details" data-txid={details.txid}>
    <dl class="facts">
      {#each details.rows as row, i (i)}
        <dt>{t(row.k)}</dt>
        <dd>{@render value(row)}</dd>
      {/each}
    </dl>
    {#each details.notes as note, i (i)}
      <h3>{t(note.k)}</h3>
      <p class="pre break" translate="no">{note.text}</p>
    {/each}
    {#each details.messages as m, i (i)}
      <h3>Message</h3>
      <p class="pre break" translate="no">{m}</p>
    {/each}
    <dl class="facts ids">
      {#each details.ids as row, i (i)}
        <dt>{t(row.k)}</dt>
        <dd>
          <SafeText class={row.k === 'Transaction ID' ? 'mono break' : 'break'} text={row.text} />
          {#if row.k === 'Transaction ID'}<CopyButton text={row.text ?? ''} label="Copy transaction ID" />{/if}
        </dd>
      {/each}
    </dl>
    {#if details.coinbaseNote}<p class="muted note"><SafeText text={details.coinbaseNote} /></p>{/if}
    {#if details.debug}
      <details class="debug">
        <summary>Debug information</summary>
        <dl class="facts">
          {#each details.debug.amounts as row, i (i)}
            <dt>{t(row.k)}</dt>
            <dd>{@render value(row)}</dd>
          {/each}
        </dl>
        <h3>Transaction</h3>
        <pre class="mono">{details.debug.transaction}</pre>
        <h3>Inputs</h3>
        <ul>
          {#each details.debug.inputs as input, i (i)}
            <li>
              {#if input.label}<SafeText text={input.label} />{/if} <span class="mono">{input.address ?? ''}</span> Amount=<Amount sats={input.sats} /> IsMine={input.mine ? 'true' : 'false'}
            </li>
          {/each}
        </ul>
      </details>
    {/if}
  </div>
{/if}

<style>
  .details {
    display: flex;
    flex-direction: column;
    gap: 0.8rem;
  }
  dd {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
  }
  .ids {
    padding-top: 0.7rem;
    border-top: 1px solid var(--line);
  }
  h3 {
    margin-top: 0.3rem;
    font-size: 0.95rem;
  }
  .pre {
    white-space: pre-wrap;
  }
  .note {
    font-size: 0.9rem;
  }
  .debug pre {
    white-space: pre-wrap;
    font-size: 0.78rem;
    background: var(--surface-2);
    padding: 0.6rem;
    border-radius: var(--radius-s);
  }
  summary {
    cursor: pointer;
    font-weight: 600;
  }
</style>
