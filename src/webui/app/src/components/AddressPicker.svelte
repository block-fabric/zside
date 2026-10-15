<script lang="ts">
  // Choose an address of the address book (AddressBookPage in ForSelection mode).
  import DataTable from './DataTable.svelte';
  import Dialog from './Dialog.svelte';
  import type { Column } from '../lib/table';
  import { current, refreshAddresses } from '../lib/walletdata.svelte';
  import type { AddressEntry } from '../lib/wallet';
  import { isSingleKeyAddress } from '../lib/address';
  import { app } from '../lib/state.svelte';

  let {
    open = $bindable(false),
    purpose,
    onchoose,
    /** Only addresses messages can be signed with (one key: P2PKH, P2WPKH). */
    singleKey = false,
  }: { open?: boolean; purpose: 'send' | 'receive'; onchoose: (entry: AddressEntry) => void; singleKey?: boolean } = $props();

  let filter = $state('');
  let selected: string | null = $state(null);

  $effect(() => {
    if (open) refreshAddresses();
  });

  const rows = $derived(current.addresses.filter((a) => a.purpose === purpose && (!singleKey || isSingleKeyAddress(a.address, app.info?.network ?? 'main'))));
  const columns: Column<AddressEntry>[] = [
    { key: 'label', label: 'Label', value: (r) => r.label || '(no label)', width: 'minmax(10rem, 1fr)' },
    { key: 'address', label: 'Address', value: (r) => r.address, width: 'minmax(18rem, 2fr)', mono: true },
  ];

  function choose(entry?: AddressEntry) {
    const e = entry ?? rows.find((r) => r.address === selected);
    if (!e) return;
    open = false;
    onchoose(e);
  }
</script>

<Dialog bind:open title={purpose === 'send' ? 'Choose the address to send coins to' : 'Choose the address to receive coins with'} wide>
  <div class="picker">
    <input type="search" placeholder="Enter address or label to search" aria-label="Search the addresses" bind:value={filter} />
    <div class="holder">
      <DataTable {rows} {columns} key={(r) => r.address} {filter} bind:selected label="Addresses" empty="No addresses yet" onactivate={(r) => choose(r)} />
    </div>
  </div>
  {#snippet actions()}
    <button type="button" class="btn" onclick={() => (open = false)}>Cancel</button>
    <button type="button" class="btn primary" disabled={!selected} onclick={() => choose()}>Choose</button>
  {/snippet}
</Dialog>

<style>
  .picker {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
  }
  .holder {
    height: 18rem;
  }
</style>
