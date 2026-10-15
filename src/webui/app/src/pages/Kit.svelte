<script lang="ts">
  // The shared components, together: for the tests of the app and for working on them.
  import SafeText from '../components/SafeText.svelte';
  import AddressInput from '../components/AddressInput.svelte';
  import Amount from '../components/Amount.svelte';
  import AmountInput from '../components/AmountInput.svelte';
  import CopyButton from '../components/CopyButton.svelte';
  import DataTable from '../components/DataTable.svelte';
  import Dialog from '../components/Dialog.svelte';
  import QrCode from '../components/QrCode.svelte';
  import { formatTime, shortHash } from '../lib/format';
  import { toast } from '../lib/state.svelte';
  import { t } from '../lib/i18n.svelte';
  import type { Column } from '../lib/table';
  import { formatPaymentUri } from '../lib/uri';

  interface Row {
    id: string;
    n: number;
    time: number;
    label: string;
    amount: bigint;
  }

  const COUNT = 100_000;
  const words = ['rent', 'coffee', 'mining payout', 'deposit to Thunder', 'withdrawal', 'friend', 'exchange', 'salary'];
  // Made up rows, the same every time: a long history to scroll.
  const rows: Row[] = Array.from({ length: COUNT }, (_, n) => {
    const x = (n * 2654435761) % 4294967296;
    return {
      id: String(n),
      n,
      time: 1760000000 - n * 600,
      label: `${words[x % words.length]} ${n}`,
      amount: BigInt((x % 2000000000) - 600000000),
    };
  });
  const columns: Column<Row>[] = [
    { key: 'n', label: '#', value: (r) => r.n, width: '6rem', align: 'end' },
    { key: 'time', label: 'Date', value: (r) => r.time, width: '12rem' },
    { key: 'label', label: 'Label', value: (r) => r.label, width: 'minmax(12rem, 1fr)' },
    { key: 'amount', label: 'Amount', value: (r) => r.amount, width: '12rem', align: 'end' },
  ];

  let filter = $state('');
  let selected: string | null = $state(null);
  let amount: bigint | null = $state(null);
  let address = $state('');
  let addressValid = $state(false);
  let dialogOpen = $state(false);
  const uri = $derived(formatPaymentUri({ address: address || 'rchn1qexample', amount: amount ?? undefined, label: 'Kit' }));
</script>

<div class="page kit">
  <header class="page-head"><h1>Interface kit</h1></header>

  <div class="top">
    <section class="panel">
      <div class="panel-head"><h2>Inputs</h2></div>
      <div class="panel-body stack">
        <AmountInput id="kit-amount" bind:value={amount} />
        <p>Value: <Amount sats={amount} /> <span class="faint">({amount === null ? 'none' : `${amount} sat`})</span></p>
        <AddressInput id="kit-address" bind:value={address} bind:valid={addressValid} />
        <div class="row">
          <button type="button" class="btn" onclick={() => (dialogOpen = true)}>Open a dialog</button>
          <button type="button" class="btn" onclick={() => toast('success', t('Copied'), t('A notice, as after copying.'))}>Show a notice</button>
          <CopyButton text={uri} label="Copy the URI" iconOnly={false} small={false} />
        </div>
      </div>
    </section>
    <section class="panel qr">
      <div class="panel-head"><h2>Payment request</h2></div>
      <div class="panel-body">
        <QrCode text={uri} size={180} label="QR code of the payment request" />
        <p class="mono break uri">{uri}</p>
      </div>
    </section>
  </div>

  <section class="table-box">
    <div class="row head">
      <h2 class="grow">{COUNT.toLocaleString('en')} rows</h2>
      <input type="search" placeholder="Filter" aria-label="Filter the rows" bind:value={filter} />
    </div>
    <div class="table-holder">
      <DataTable {rows} {columns} key={(r) => r.id} {filter} bind:selected label="Generated rows" onactivate={(r) => toast('info', t('Row %1', r.n), r.label)}>
        {#snippet cell(row, column)}
          {#if column.key === 'amount'}<Amount sats={row.amount} colored plus />
          {:else if column.key === 'time'}{formatTime(row.time)}
          {:else if column.key === 'label'}<SafeText text={row.label} /> <span class="faint mono">{shortHash(`${row.id}a1b2c3d4e5f60718293a4b5c6d7e8f90`, 4)}</span>
          {:else}<SafeText text={String(column.value(row) ?? '')} />{/if}
        {/snippet}
      </DataTable>
    </div>
  </section>

  <Dialog bind:open={dialogOpen} title="A dialog">
    <p>Dialogs keep the focus inside them; Escape closes them.</p>
    {#snippet actions()}
      <button type="button" class="btn" onclick={() => (dialogOpen = false)}>Cancel</button>
      <button type="button" class="btn primary" onclick={() => (dialogOpen = false)}>Done</button>
    {/snippet}
  </Dialog>
</div>

<style>
  .kit {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    height: 100%;
  }
  .top {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 1rem;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.8rem;
  }
  .uri {
    max-width: 16rem;
    font-size: 0.8rem;
    color: var(--text-muted);
  }
  .table-box {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    flex: 1;
    min-height: 18rem;
  }
  .head input {
    width: 16rem;
  }
  .table-holder {
    flex: 1;
    min-height: 0;
  }
  @media (max-width: 900px) {
    .top {
      grid-template-columns: minmax(0, 1fr);
    }
  }
</style>
