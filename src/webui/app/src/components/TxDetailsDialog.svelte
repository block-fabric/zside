<script lang="ts">
  // The details of a transaction of the wallet (TransactionDescDialog).
  import Dialog from './Dialog.svelte';
  import TxDetailsView from './TxDetailsView.svelte';
  import type { TxRecord } from '../lib/wallet';

  let { wallet, record = $bindable(null) }: { wallet: string; record?: Pick<TxRecord, 'txid' | 'idx' | 'address'> | null } = $props();

  let open = $state(false);

  $effect(() => {
    open = record !== null;
  });
</script>

<Dialog bind:open title="Transaction details" wide onclose={() => (record = null)}>
  {#if record}<TxDetailsView {wallet} {record} />{/if}
  {#snippet actions()}
    <button type="button" class="btn primary" onclick={() => (record = null)}>Close</button>
  {/snippet}
</Dialog>
