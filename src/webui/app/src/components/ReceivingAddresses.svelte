<script lang="ts">
  // The Addresses tab of the Receive page (user decision, 2026-10-15: no Addresses page, no sending
  // addresses): the receiving addresses of the wallet (AddressBookPage, receiving tab), with their
  // type, whether they were used, what they received and when last; search, copy, QR code, a new
  // label, and a new address with its label and type.
  import { t } from '../lib/i18n.svelte';
  import SafeText from './SafeText.svelte';
  import Amount from './Amount.svelte';
  import ContextMenu, { type ContextItem } from './ContextMenu.svelte';
  import DataTable from './DataTable.svelte';
  import EditAddressDialog, { type EditMode } from './EditAddressDialog.svelte';
  import Icon from './Icon.svelte';
  import RequestDialog from './RequestDialog.svelte';
  import { addressTypeOf } from '../lib/address';
  import { errorText, inform, withUnlock } from '../lib/dialogs.svelte';
  import { FILTERS, saveTextAs } from '../lib/files';
  import { formatTime } from '../lib/format';
  import { go } from '../lib/nav.svelte';
  import { host } from '../lib/rpc';
  import { app, prefs, toast, wallets } from '../lib/state.svelte';
  import type { Column } from '../lib/table';
  import { ADDRESS_TYPES, addressesCsv, addressTypeText, receivingAddresses, walletApi, type ReceivingAddress } from '../lib/wallet';
  import { current, refreshAddresses, refreshInfo, refreshRecords } from '../lib/walletdata.svelte';

  const wallet = $derived(wallets.current);
  const info = $derived(current.info);
  const network = $derived(app.info?.network ?? 'main');

  let filter = $state('');
  let selected: string | null = $state(null);
  let sortKey: string | null = $state('lastUsed');
  let sortDir: 'asc' | 'desc' = $state('desc');
  let edit: { mode: EditMode; address: string; label: string } | null = $state(null);
  let shown: { address: string; label: string; amount: bigint; message: string } | null = $state(null);
  let menu = $state({ open: false, x: 0, y: 0 });
  let newLabel = $state('');
  let newType = $state('');
  let busy = $state(false);

  const types = $derived(ADDRESS_TYPES.filter((ty) => !ty.taproot || info?.taproot));

  $effect(() => {
    void wallets.addressVersion;
    if (wallet !== null) refreshAddresses();
  });
  // What each address received comes from the history (loaded once, then kept up to date).
  $effect(() => {
    void wallets.txVersion;
    void wallet;
    refreshRecords();
  });
  $effect(() => {
    if (wallet !== null) refreshInfo();
  });
  $effect(() => {
    if (info && !newType) newType = info.defaultAddressType;
  });

  const rows = $derived(receivingAddresses(current.addresses, current.records, (a) => addressTypeOf(a, network)));
  const chosen = $derived(rows.find((r) => r.address === selected) ?? null);
  const columns: Column<ReceivingAddress>[] = $derived([
    { key: 'label', label: 'Label', value: (r) => r.label, width: 'minmax(8rem, 1fr)' },
    { key: 'address', label: 'Address', value: (r) => r.address, width: 'minmax(16rem, 2fr)', mono: true },
    { key: 'type', label: 'Type', value: (r) => addressTypeText(r.type), width: '10rem', words: true },
    { key: 'used', label: 'Status', value: (r) => (r.used ? t('Used') : t('Unused')), width: '6rem', words: true },
    { key: 'received', label: `Received (${prefs.unit})`, value: (r) => r.received, width: '9.5rem', align: 'end', noFilter: true },
    { key: 'lastUsed', label: 'Last used', value: (r) => r.lastUsed, width: '9rem', noFilter: true },
  ]);

  async function copy(text: string, what: string) {
    await host.copy(text).catch(() => undefined);
    toast('success', t('Copied'), what);
  }

  function editLabel(r: ReceivingAddress | null = chosen) {
    if (r) edit = { mode: 'edit-receive', address: r.address, label: r.label };
  }

  function showQr(r: ReceivingAddress | null = chosen) {
    if (r) shown = { address: r.address, label: r.label, amount: 0n, message: '' };
  }

  async function generate() {
    if (wallet === null || busy) return;
    const api = walletApi(wallet);
    busy = true;
    try {
      const type = newType || info?.defaultAddressType || 'bech32';
      const address = await withUnlock(wallet, () => api.newAddress(newLabel, type));
      if (address === null) {
        await inform(t('Receive'), t('Could not unlock wallet.'), 'error');
        return;
      }
      await refreshAddresses(true);
      selected = address;
      newLabel = '';
      toast('success', t('New address'), address);
    } catch (e) {
      await inform(t('Receive'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  async function exportCsv() {
    try {
      const path = await saveTextAs({ title: 'Export Address List', name: 'receiving-addresses.csv', filters: [FILTERS.csv] }, addressesCsv(rows.map((r) => ({ address: r.address, label: r.label, purpose: 'receive', mine: true }))));
      if (path) toast('success', t('Exported'), path);
    } catch (e) {
      await inform(t('Exporting Failed'), t('There was an error trying to save the address list. Please try again. %1', errorText(e)), 'error');
    }
  }

  const menuItems: ContextItem[] = $derived(
    chosen
      ? [
          { label: 'Copy Address', action: () => copy(chosen.address, 'Address') },
          { label: 'Copy Label', action: () => copy(chosen.label, 'Label'), disabled: !chosen.label },
          { label: 'Edit label', action: () => editLabel() },
          { label: 'Show QR code', action: () => showQr() },
          { label: 'Sign a message with this address', action: () => go('messages', `sign:${chosen.address}`), separator: true },
        ]
      : [],
  );
</script>

<div class="addresses">
  <form
    class="panel new-address"
    aria-label="Generate new address"
    onsubmit={(e) => {
      e.preventDefault();
      generate();
    }}
  >
    <div class="field grow">
      <label for="addr-new-label">Label</label>
      <input id="addr-new-label" type="text" bind:value={newLabel} autocomplete="off" placeholder="An optional label for the new address" />
    </div>
    <div class="field">
      <label for="addr-new-type">Address type</label>
      <select id="addr-new-type" bind:value={newType}>
        {#each types as ty (ty.id)}<option value={ty.id} title={t(ty.tip)}>{t(ty.label)}</option>{/each}
      </select>
    </div>
    <button type="submit" class="btn primary" disabled={busy || !info?.canGetAddresses} title={info && !info.canGetAddresses ? 'This wallet cannot give out new addresses' : undefined}>
      <Icon name="plus" size={16} /> Generate new address
    </button>
  </form>

  <div class="row toolbar">
    <input type="search" class="grow" placeholder="Enter address or label to search" aria-label="Search the addresses" bind:value={filter} />
    <button type="button" class="btn small" disabled={!chosen} onclick={() => chosen && copy(chosen.address, 'Address')} title="Copy the currently selected address to the system clipboard"><Icon name="copy" size={15} /> Copy</button>
    <button type="button" class="btn small" disabled={!chosen} onclick={() => showQr()} title="Show the QR code of the selected address"><Icon name="qr" size={15} /> QR code</button>
    <button type="button" class="btn small" disabled={!chosen} onclick={() => editLabel()}><Icon name="edit" size={15} /> Edit label</button>
    <button type="button" class="btn small" onclick={exportCsv} title="Export the data in the current tab to a file"><Icon name="download" size={15} /> Export</button>
  </div>

  <div class="holder">
    <DataTable
      {rows}
      {columns}
      key={(r) => r.address}
      {filter}
      bind:selected
      bind:sortKey
      bind:sortDir
      label="Receiving addresses"
      empty="No receiving addresses yet"
      onactivate={(r) => editLabel(r)}
      oncontext={(r, e) => (menu = { open: true, x: e.clientX, y: e.clientY })}
    >
      {#snippet cell(row, column)}
        {#if column.key === 'label'}{#if row.label}<SafeText text={row.label} />{:else}<span class="faint">(no label)</span>{/if}
        {:else if column.key === 'address'}<SafeText text={row.address} />
        {:else if column.key === 'type'}{addressTypeText(row.type)}
        {:else if column.key === 'used'}{#if row.used}<span class="status ok">Used</span>{:else}<span class="status">Unused</span>{/if}
        {:else if column.key === 'received'}{#if row.used}<Amount sats={row.received} showUnit={false} />{:else}<span class="faint">–</span>{/if}
        {:else if row.lastUsed}{formatTime(row.lastUsed)}{:else}<span class="faint">Never</span>{/if}
      {/snippet}
    </DataTable>
  </div>
</div>

{#if wallet !== null}
  <EditAddressDialog {wallet} bind:edit />
  <RequestDialog bind:request={shown} {wallet} externalSigner={info?.externalSigner ?? false} />
{/if}
<ContextMenu items={menuItems} x={menu.x} y={menu.y} bind:open={menu.open} label="Address" />

<style>
  .addresses {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
    flex: 1;
    min-height: 0;
  }
  .new-address {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-end;
    gap: 0.7rem;
    padding: 0.8rem 1rem;
  }
  .new-address .grow {
    min-width: 12rem;
  }
  .toolbar {
    flex-wrap: wrap;
  }
  .toolbar input {
    min-width: 14rem;
  }
  .holder {
    flex: 1;
    min-height: 16rem;
  }
</style>
