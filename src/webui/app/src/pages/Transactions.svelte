<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  import { simpleMode } from '../lib/uimode.svelte';
  import { thirdPartyLinks } from '../lib/tools';
  // The history of the wallet (TransactionView, TransactionTableModel, TransactionFilterProxy):
  // filters, a long sortable table, details, fee bumps, abandoning, copying, the CSV export.
  import Amount from '../components/Amount.svelte';
  import ContextMenu, { type ContextItem } from '../components/ContextMenu.svelte';
  import DataTable from '../components/DataTable.svelte';
  import EditAddressDialog, { type EditMode } from '../components/EditAddressDialog.svelte';
  import Icon from '../components/Icon.svelte';
  import TxDetailsDialog from '../components/TxDetailsDialog.svelte';
  import TxStatusIcon from '../components/TxStatusIcon.svelte';
  import { errorText, inform } from '../lib/dialogs.svelte';
  import { FILTERS, saveTextAs } from '../lib/files';
  import { formatTime } from '../lib/format';
  import { go, nav } from '../lib/nav.svelte';
  import { host } from '../lib/rpc';
  import { prefs, savePrefs, toast, wallets } from '../lib/state.svelte';
  import type { Column } from '../lib/table';
  import { abandon, bumpFee } from '../lib/txactions';
  import { formatAmount, parseAmount } from '../lib/units';
  import {
    addressText,
    amountText,
    DATE_FILTERS,
    dateRange,
    filterRecords,
    plainText,
    sortNewestFirst,
    statusText,
    transactionsCsv,
    TYPE_FILTERS,
    typeText,
    walletApi,
    type DateFilter,
    type TxRecord,
    type TypeFilter,
  } from '../lib/wallet';
  import { current, refreshAddresses, refreshRecords } from '../lib/walletdata.svelte';
  import { node } from '../lib/state.svelte';
  import { detail, showDetail } from '../lib/detail.svelte';
  import { untrack } from 'svelte';

  const wallet = $derived(wallets.current);

  let dateFilter: DateFilter = $state('all');
  let rangeFrom = $state('');
  let rangeTo = $state('');
  let typeFilter: TypeFilter = $state('all');
  let search = $state('');
  let minText = $state('');
  let selected: string | null = $state(null);
  let sortKey: string | null = $state('date');
  let sortDir: 'asc' | 'desc' = $state('desc');
  let details: Pick<TxRecord, 'txid' | 'idx' | 'address'> | null = $state(null);
  let edit: { mode: EditMode; address: string; label: string } | null = $state(null);
  let menu = $state({ open: false, x: 0, y: 0 });
  let actions = $state({ txid: '', canAbandon: false, canBump: false, hex: '' });

  // Reload on new transactions, new blocks, a new label.
  $effect(() => {
    void wallets.txVersion;
    void node.tip?.hash;
    void wallets.addressVersion;
    if (wallet !== null) refreshRecords();
  });
  $effect(() => {
    void wallets.addressVersion;
    if (wallet !== null) refreshAddresses();
  });

  const minAmount = $derived.by(() => {
    if (!minText.trim()) return 0n;
    const p = parseAmount(minText, prefs.unit);
    return p.ok ? p.sats : 0n;
  });
  const minInvalid = $derived(minText.trim() !== '' && !parseAmount(minText, prefs.unit).ok);
  const range = $derived(dateRange(dateFilter, new Date(), rangeFrom ? new Date(`${rangeFrom}T00:00`) : undefined, rangeTo ? new Date(`${rangeTo}T00:00`) : undefined));
  // The search waits for a pause in the typing when the history is long (TransactionView waits too).
  let searchApplied = $state('');
  $effect(() => {
    const text = search;
    if (current.records.length < 2000) {
      searchApplied = text;
      return;
    }
    const t = setTimeout(() => (searchApplied = text), 250);
    return () => clearTimeout(t);
  });
  const shown = $derived(filterRecords(current.records, { ...range, type: typeFilter, search: searchApplied, minAmount, showInactive: true }));
  const chosen = $derived(shown.find((r) => r.key === selected) ?? current.records.find((r) => r.key === selected) ?? null);

  // Totals of what the filters show (an addition to chains-qt).
  const totals = $derived.by(() => {
    let received = 0n;
    let sent = 0n;
    for (const r of shown) {
      if (r.amount > 0n) received += r.amount;
      else sent += r.amount;
    }
    return { received, sent, count: shown.length };
  });

  // After a send, or from the overview: that transaction selected.
  $effect(() => {
    const txid = nav.highlightTx;
    if (!txid || !current.recordsLoaded) return;
    const r = current.records.find((x) => x.txid === txid);
    if (r) {
      selected = r.key;
      nav.highlightTx = null;
    }
  });

  // The workspace layout shows the selected transaction in its detail panel.
  function inPanel(r: Pick<TxRecord, 'txid' | 'idx' | 'address'>) {
    if (wallet !== null) showDetail({ kind: 'tx', wallet, txid: r.txid, idx: r.idx, address: r.address });
  }
  const chosenKey = $derived(chosen?.key ?? null);
  $effect(() => {
    if (chosenKey !== null && detail.follow) untrack(() => chosen && inPanel(chosen));
  });

  // A narrow window: the type goes in front of the label, and the columns are tighter, so that the
  // amount stays in view.
  let pageWidth = $state(1000);
  let pageEl: HTMLDivElement | undefined = $state();
  $effect(() => {
    if (!pageEl || typeof ResizeObserver === 'undefined') return;
    const o = new ResizeObserver(() => pageEl && (pageWidth = pageEl.clientWidth));
    o.observe(pageEl);
    pageWidth = pageEl.clientWidth;
    return () => o.disconnect();
  });
  const narrow = $derived(pageWidth < 760);
  const simple = $derived(simpleMode());
  const columns: Column<TxRecord>[] = $derived(
    narrow
      ? [
          { key: 'status', label: '', value: (r) => r.sortKey, width: '2.2rem', align: 'center', noFilter: true },
          { key: 'date', label: 'Date', value: (r) => r.time, width: '8.6rem' },
          { key: 'label', label: 'Label', value: (r) => addressText(r, true), width: 'minmax(0, 1fr)' },
          { key: 'amount', label: `Amount (${prefs.unit})`, value: (r) => r.amount, width: '9.5rem', align: 'end' },
        ]
      : [
          // Simple mode: the status in plain words beside its ring (waiting for the next block, confirmed, final).
          simple
            ? { key: 'status', label: 'Status', value: (r) => r.sortKey, width: '15.5rem', noFilter: true }
            : { key: 'status', label: '', value: (r) => r.sortKey, width: '2.6rem', align: 'center', noFilter: true },
          { key: 'date', label: 'Date', value: (r) => r.time, width: '11.5rem' },
          { key: 'type', label: 'Type', value: (r) => typeText(r.type, r.role), width: '10rem' },
          { key: 'label', label: 'Label', value: (r) => addressText(r, true), width: 'minmax(14rem, 1fr)' },
          { key: 'amount', label: `Amount (${prefs.unit})`, value: (r) => r.amount, width: '12rem', align: 'end' },
        ],
  );

  async function copy(text: string, what: string) {
    try {
      await host.copy(text);
      toast('success', t('Copied'), what);
    } catch (e) {
      toast('error', t('Could not copy'), errorText(e));
    }
  }

  async function openMenu(r: TxRecord, e: MouseEvent) {
    selected = r.key;
    menu = { open: false, x: e.clientX, y: e.clientY };
    actions = { txid: r.txid, canAbandon: false, canBump: false, hex: '' };
    if (wallet !== null) {
      try {
        actions = { txid: r.txid, ...(await walletApi(wallet).txActions(r.txid)) };
      } catch {
        // Gone meanwhile.
      }
    }
    menu = { open: true, x: e.clientX, y: e.clientY };
  }

  function editLabel(r: TxRecord) {
    if (!r.address) return;
    // In the address book: edit it; otherwise add it as a sending address (TransactionView::editLabel).
    const entry = current.addresses.find((a) => a.address === r.address);
    if (entry) edit = { mode: entry.purpose === 'receive' ? 'edit-receive' : 'edit-send', address: entry.address, label: entry.label };
    else edit = { mode: 'new-send', address: r.address, label: '' };
  }

  const menuItems: ContextItem[] = $derived(
    chosen
      ? [
          { label: 'Copy address', action: () => copy(chosen.address, 'Address'), disabled: !chosen.address },
          { label: 'Copy label', action: () => copy(chosen.label, 'Label'), disabled: !chosen.label },
          { label: 'Copy amount', action: () => copy(amountText(chosen, prefs.unit, false, false), 'Amount') },
          { label: 'Copy transaction ID', action: () => copy(chosen.txid, 'Transaction ID') },
          { label: 'Copy raw transaction', action: () => copy(actions.hex, 'Raw transaction'), disabled: !actions.hex },
          { label: 'Copy full transaction details', action: () => copy(plainText(chosen, prefs.unit), 'Transaction details') },
          { label: 'Show transaction details', action: () => (details = chosen), separator: true },
          { label: 'Increase transaction fee', action: () => wallet !== null && bump(chosen.txid), disabled: !actions.canBump, separator: true },
          { label: 'Abandon transaction', action: () => wallet !== null && abandon(wallet, chosen.txid), disabled: !actions.canAbandon, danger: true },
          { label: 'Edit address label', action: () => editLabel(chosen), disabled: !chosen.address, separator: true },
          { label: 'Show in block explorer', action: () => go('explorer', `search:${chosen.txid}`), separator: true },
          ...thirdPartyLinks(prefs.thirdPartyTxUrls, chosen.txid).map((l) => ({ label: l.label, action: () => host.open('url', l.url).catch(() => undefined) })),
        ]
      : [],
  );

  async function bump(txid: string) {
    if (wallet === null) return;
    const newTxid = await bumpFee(wallet, txid);
    if (newTxid) nav.highlightTx = newTxid;
  }

  async function exportCsv() {
    // The rows as shown: filtered and sorted.
    const sorted = sortNewestFirst(shown);
    try {
      const path = await saveTextAs({ title: 'Export Transaction History', name: 'transactions.csv', filters: [FILTERS.csv] }, transactionsCsv(sorted, prefs.unit));
      if (path) toast('success', t('Exporting Successful'), t('The transaction history was successfully saved to %1.', path));
    } catch (e) {
      await inform(t('Exporting Failed'), t('There was an error trying to save the transaction history. %1', errorText(e)), 'error');
    }
  }

  function rowClass(r: TxRecord): string {
    if (r.status === 'abandoned') return 'danger';
    if (!r.countsForBalance && r.status !== 'immature') return 'unconfirmed';
    return '';
  }
</script>

<div class="page transactions" bind:this={pageEl}>
  <header class="page-head">
    <h1>Transactions</h1>
    <div class="row">
      <span class="muted count">{totals.count.toLocaleString('en')} of {current.records.length.toLocaleString('en')}</span>
      <button type="button" class="btn small" onclick={exportCsv} disabled={prefs.mask} title="Export the data in the current tab to a file"><Icon name="download" size={16} /> Export</button>
    </div>
  </header>

  {#if prefs.mask}
    <div class="panel masked">
      <Icon name="eyeoff" size={28} />
      <div>
        <p><strong>The history is hidden while values are masked.</strong></p>
        <p class="muted">As in chains-qt, the transactions tab is unavailable in privacy mode.</p>
        <button type="button" class="btn" onclick={() => savePrefs({ mask: false })}>Show values</button>
      </div>
    </div>
  {:else}
    <div class="filters" role="search">
      <select aria-label="Date" bind:value={dateFilter}>
        {#each DATE_FILTERS as f (f.id)}<option value={f.id}>{t(f.label)}</option>{/each}
      </select>
      {#if dateFilter === 'range'}
        <label class="range">From <input type="date" bind:value={rangeFrom} /></label>
        <label class="range">to <input type="date" bind:value={rangeTo} /></label>
      {/if}
      <select aria-label="Type" bind:value={typeFilter}>
        {#each TYPE_FILTERS as f (f.id)}<option value={f.id}>{t(f.label)}</option>{/each}
      </select>
      <input type="search" class="grow search" placeholder="Enter address, transaction id, or label to search" aria-label="Search" bind:value={search} />
      <input type="text" class="min num" placeholder="Min amount" aria-label="Minimum amount" inputmode="decimal" bind:value={minText} aria-invalid={minInvalid ? 'true' : undefined} />
    </div>

    <div class="totals">
      <span>Received <Amount sats={totals.received} colored /></span>
      <span>Sent <Amount sats={totals.sent} colored /></span>
      <span>Net <Amount sats={totals.received + totals.sent} colored plus /></span>
    </div>

    <div class="holder">
      {#if current.recordsError}
        <p class="error">{current.recordsError}</p>
      {:else}
        <DataTable
          rows={shown}
          {columns}
          key={(r) => r.key}
          bind:selected
          bind:sortKey
          bind:sortDir
          label="Transactions"
          fit={narrow}
          empty={current.recordsLoaded ? 'No transactions yet' : 'Loading…'}
          onactivate={(r) => (detail.follow ? inPanel(r) : (details = r))}
          oncontext={(r, e) => openMenu(r, e)}
        >
          {#snippet cell(row, column)}
            <span class="cellwrap {rowClass(row)}" title={column.key === 'label' ? `${statusText(row)}\n${typeText(row.type, row.role)} ${addressText(row, true)}` : undefined}>
              {#if column.key === 'status'}<TxStatusIcon record={row} />{#if simple && !narrow}<span class="status-word">{statusText(row)}</span>{/if}
              {:else if column.key === 'date'}{narrow ? formatTime(row.time, true) : formatTime(row.time)}
              {:else if column.key === 'type'}<span class="type {row.type}"><Icon name={row.type === 'mined' ? 'mine' : row.amount < 0n ? 'send' : 'receive'} size={14} /> {typeText(row.type, row.role) || 'Other'}</span>
              {:else if column.key === 'label'}{#if narrow}<span class="type {row.type}" title={typeText(row.type, row.role) || 'Other'}><Icon name={row.type === 'mined' ? 'mine' : row.amount < 0n ? 'send' : 'receive'} size={14} /></span> {/if}<span class:bare={!row.label} translate="no">{addressText(row)}</span>
              {:else if column.key === 'amount'}
                {#if prefs.mask}<Amount sats={row.amount} showUnit={false} />
                {:else}<span class="amt" class:neg={row.amount < 0n} translate="no">{amountText(row, prefs.unit)}</span>{/if}
              {/if}
            </span>
          {/snippet}
        </DataTable>
      {/if}
    </div>
    <p class="hint">Double-click a transaction for its details, right-click for more.</p>
  {/if}
</div>

{#if wallet !== null}
  <TxDetailsDialog {wallet} bind:record={details} />
  <EditAddressDialog {wallet} bind:edit />
{/if}
<ContextMenu items={menuItems} x={menu.x} y={menu.y} bind:open={menu.open} label="Transaction" />

<style>
  .status-word {
    margin-left: 0.45rem;
    color: var(--text-muted);
    font-size: 0.9rem;
  }
  .transactions {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
    height: 100%;
  }
  .filters {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
    align-items: center;
  }
  .filters select {
    width: auto;
  }
  .search {
    min-width: 16rem;
  }
  .min {
    width: 9rem;
    text-align: right;
  }
  .range {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
    color: var(--text-muted);
  }
  .range input {
    width: auto;
  }
  .totals {
    display: flex;
    flex-wrap: wrap;
    gap: 1.4rem;
    color: var(--text-muted);
    font-size: 0.9rem;
  }
  .holder {
    flex: 1;
    min-height: 16rem;
  }
  .cellwrap {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .cellwrap.unconfirmed {
    color: var(--text-muted);
  }
  .cellwrap.danger {
    color: var(--negative);
  }
  .bare {
    color: var(--text-muted);
  }
  .amt.neg {
    color: var(--negative);
  }
  .unconfirmed .amt.neg {
    color: inherit;
  }
  .type {
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
  }
  .type.mined {
    color: var(--warning);
  }
  .count {
    font-size: 0.9rem;
  }
  .hint {
    margin: 0;
    color: var(--text-faint);
    font-size: 0.85rem;
  }
  .masked {
    display: flex;
    gap: 1rem;
    align-items: flex-start;
    padding: 1.4rem;
    color: var(--accent-fg);
  }
  .masked div {
    color: var(--text);
  }
</style>
