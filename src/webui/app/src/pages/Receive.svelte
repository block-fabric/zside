<script lang="ts">
  // Receive (ReceiveCoinsDialog): a request with a new address, a label, an amount and a message,
  // and the requests made before (RecentRequestsTableModel), kept in the wallet. Its Addresses tab: the
  // receiving addresses of the wallet (there is no Addresses page: user decision, 2026-10-15).
  import { t } from '../lib/i18n.svelte';
  import { CHAIN } from '../chain/config';
  import SafeText from '../components/SafeText.svelte';
  import Amount from '../components/Amount.svelte';
  import AmountInput from '../components/AmountInput.svelte';
  import ContextMenu, { type ContextItem } from '../components/ContextMenu.svelte';
  import DataTable from '../components/DataTable.svelte';
  import Icon from '../components/Icon.svelte';
  import ReceivingAddresses from '../components/ReceivingAddresses.svelte';
  import RequestDialog from '../components/RequestDialog.svelte';
  import { nav } from '../lib/nav.svelte';
  import { errorText, inform, withUnlock } from '../lib/dialogs.svelte';
  import { formatTime } from '../lib/format';
  import { host } from '../lib/rpc';
  import { prefs, toast, wallets } from '../lib/state.svelte';
  import type { Column } from '../lib/table';
  import { formatAmount } from '../lib/units';
  import { formatPaymentUri } from '../lib/uri';
  import { ADDRESS_TYPES, walletApi, type ReceiveRequest } from '../lib/wallet';
  import { current, refreshInfo } from '../lib/walletdata.svelte';

  const wallet = $derived(wallets.current);
  const api = $derived(wallet === null ? null : walletApi(wallet));
  const info = $derived(current.info);

  let label = $state('');
  let amount: bigint | null = $state(null);
  let message = $state('');
  let addressType = $state('');
  let busy = $state(false);
  let formKey = $state(0);
  let requests: ReceiveRequest[] = $state([]);
  let selected: string | null = $state(null);
  let shown: { address: string; label: string; amount: bigint; message: string } | null = $state(null);
  let filter = $state('');
  let menu = $state({ open: false, x: 0, y: 0 });
  let sortKey: string | null = $state('date');
  let sortDir: 'asc' | 'desc' = $state('desc');

  const TABS = [
    { id: '', label: 'Requests' },
    { id: 'addresses', label: 'Addresses' },
  ];
  const tab = $derived(nav.tab === 'addresses' ? 'addresses' : '');

  const types = $derived(ADDRESS_TYPES.filter((t) => !t.taproot || info?.taproot));
  const typeTip = $derived(types.find((ty) => ty.id === addressType)?.tip ?? '');

  $effect(() => {
    if (wallet !== null) refreshInfo();
  });
  $effect(() => {
    if (info && !addressType) addressType = info.defaultAddressType;
  });

  async function loadRequests() {
    if (!api) return;
    try {
      requests = await api.requests();
    } catch {
      requests = [];
    }
  }
  $effect(() => {
    void wallet;
    loadRequests();
  });

  function clear() {
    label = '';
    amount = null;
    message = '';
    formKey++;
  }

  async function create() {
    if (!api || wallet === null || busy) return;
    busy = true;
    try {
      const type = addressType || info?.defaultAddressType || 'bech32';
      const address = await withUnlock(wallet, () => api.newAddress(label, type));
      if (address === null) {
        await inform(t('Receive'), t('Could not unlock wallet.'), 'error');
        return;
      }
      const request = { address, label, amount: amount ?? 0n, message };
      shown = request;
      // Kept for later (RecentRequestsTableModel::addNewRequest).
      await api.addRequest(request);
      await loadRequests();
      clear();
    } catch (e) {
      await inform(t('Receive'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  const keyOf = (r: ReceiveRequest) => `${r.address}#${r.id}`;
  const chosen = $derived(requests.find((r) => keyOf(r) === selected) ?? null);

  const columns: Column<ReceiveRequest>[] = $derived([
    { key: 'date', label: 'Date', value: (r) => r.time, width: '10rem' },
    { key: 'label', label: 'Label', value: (r) => r.label || '(no label)', width: 'minmax(9rem, 1fr)' },
    { key: 'message', label: 'Message', value: (r) => r.message || '(no message)', width: 'minmax(9rem, 1fr)' },
    { key: 'amount', label: `Requested (${prefs.unit})`, value: (r) => r.amount, width: '11rem', align: 'end' },
  ]);

  async function remove() {
    if (!api || !chosen) return;
    try {
      await api.removeRequests([{ address: chosen.address, id: chosen.id }]);
      selected = null;
      await loadRequests();
    } catch (e) {
      toast('error', t('Remove'), errorText(e));
    }
  }

  async function copy(text: string, what: string) {
    await host.copy(text).catch(() => undefined);
    toast('success', t('Copied'), what);
  }

  const menuItems: ContextItem[] = $derived(
    chosen
      ? [
          { label: 'Copy URI', action: () => copy(formatPaymentUri({ address: chosen.address, amount: chosen.amount || undefined, label: chosen.label || undefined, message: chosen.message || undefined }), 'URI') },
          { label: 'Copy address', action: () => copy(chosen.address, 'Address') },
          { label: 'Copy label', action: () => copy(chosen.label, 'Label'), disabled: !chosen.label },
          { label: 'Copy message', action: () => copy(chosen.message, 'Message'), disabled: !chosen.message },
          { label: 'Copy amount', action: () => copy(formatAmount(chosen.amount, prefs.unit, { separators: false }), 'Amount'), disabled: chosen.amount === 0n },
        ]
      : [],
  );
</script>

<div class="page receive">
  <header class="page-head"><h1>Receive</h1></header>
  <div class="tabs" role="tablist" aria-label="Receive">
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>{t(tb.label)}</button>
    {/each}
  </div>

  {#if tab === 'addresses'}
    <ReceivingAddresses />
  {:else}
  <div class="layout">
    <section class="panel form" aria-labelledby="request-title">
      <div class="panel-head"><h2 id="request-title">New payment request</h2></div>
      {#key formKey}
        <form
          class="panel-body stack"
          onsubmit={(e) => {
            e.preventDefault();
            create();
          }}
        >
          <div class="field">
            <label for="req-label">Label</label>
            <input id="req-label" type="text" bind:value={label} autocomplete="off" title="An optional label to associate with the new receiving address." />
          </div>
          <AmountInput id="req-amount" label="Amount" bind:value={amount} placeholder="An optional amount to request" />
          <div class="field">
            <label for="req-message">Message</label>
            <input
              id="req-message"
              type="text"
              bind:value={message}
              autocomplete="off"
              title="An optional message to attach to the payment request, which will be displayed when the request is opened. Note: The message will not be sent with the payment over the {CHAIN.name} network."
            />
          </div>
          <div class="field">
            <label for="req-type">Address type</label>
            <select id="req-type" bind:value={addressType}>
              {#each types as tb (tb.id)}<option value={tb.id} title={t(tb.tip)}>{t(tb.label)}</option>{/each}
            </select>
            <span class="hint">{t(typeTip)}</span>
          </div>
          <div class="row">
            <button type="submit" class="btn primary" disabled={busy || !info?.canGetAddresses} title={info && !info.canGetAddresses ? 'This wallet cannot give out new addresses' : undefined}>
              <Icon name="receive" size={17} /> Create new receiving address
            </button>
            <button type="button" class="btn" onclick={clear}>Clear</button>
          </div>
          <p class="hint">Use this form to request payments. All fields are optional.</p>
        </form>
      {/key}
    </section>

    <section class="history" aria-labelledby="history-title">
      <div class="row head">
        <h2 id="history-title" class="grow">Requested payments history</h2>
        <input type="search" placeholder="Search" aria-label="Search the requests" bind:value={filter} />
        <button type="button" class="btn small" disabled={!chosen} onclick={() => chosen && (shown = chosen)}><Icon name="eye" size={15} /> Show</button>
        <button type="button" class="btn small" disabled={!chosen} onclick={remove}><Icon name="trash" size={15} /> Remove</button>
      </div>
      <div class="holder">
        <DataTable
          rows={requests}
          {columns}
          key={keyOf}
          {filter}
          bind:selected
          bind:sortKey
          bind:sortDir
          label="Requested payments"
          empty="No requests yet"
          onactivate={(r) => (shown = r)}
          oncontext={(r, e) => (menu = { open: true, x: e.clientX, y: e.clientY })}
        >
          {#snippet cell(row, column)}
            {#if column.key === 'date'}{formatTime(row.time)}
            {:else if column.key === 'amount'}{#if row.amount > 0n}<Amount sats={row.amount} showUnit={false} />{:else}<span class="faint">(no amount requested)</span>{/if}
            {:else if column.key === 'label'}{#if row.label}<SafeText text={row.label} />{:else}<span class="faint">(no label)</span>{/if}
            {:else if row.message}<SafeText text={row.message} />{:else}<span class="faint">(no message)</span>{/if}
          {/snippet}
        </DataTable>
      </div>
    </section>
  </div>
  {/if}
</div>

{#if wallet !== null}
  <RequestDialog bind:request={shown} {wallet} externalSigner={info?.externalSigner ?? false} />
{/if}
<ContextMenu items={menuItems} x={menu.x} y={menu.y} bind:open={menu.open} label="Request" />

<style>
  .receive {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    height: 100%;
  }
  .layout {
    display: grid;
    grid-template-columns: minmax(18rem, 26rem) minmax(0, 1fr);
    gap: 1rem;
    flex: 1;
    min-height: 0;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.85rem;
  }
  .hint {
    color: var(--text-muted);
    font-size: 0.86rem;
    margin: 0;
  }
  .history {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    min-height: 20rem;
  }
  .head {
    flex-wrap: wrap;
  }
  .head input {
    width: 12rem;
  }
  .holder {
    flex: 1;
    min-height: 14rem;
  }
  @media (max-width: 980px) {
    .layout {
      grid-template-columns: minmax(0, 1fr);
    }
  }
</style>
