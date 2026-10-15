<script lang="ts">
  // "Choose coins" on the Send page (user decision, 2026-10-15: coin control in the page, not behind a
  // setting nor in a dialog): the coins of the wallet (CoinControlDialog's list) to tick, with their
  // amount, address, label, confirmations, age and lock; sorted, filtered, all or none chosen; what
  // the coins chosen hold against what is sent, and the estimate of the host (CoinSummary). Only the
  // coins chosen are spent (the host's coin control: no other inputs once some are selected). The
  // list is virtual (DataTable): thousands of coins scroll as fast as ten. With purpose 'prove' (the
  // Proof of funds): the coins to prove, locked ones too; nothing about sending.
  import { t } from '../lib/i18n.svelte';
  import SafeText from './SafeText.svelte';
  import Amount from './Amount.svelte';
  import CoinSummary from './CoinSummary.svelte';
  import ContextMenu, { type ContextItem } from './ContextMenu.svelte';
  import DataTable from './DataTable.svelte';
  import Icon from './Icon.svelte';
  import { shortDuration } from '../lib/format';
  import { host } from '../lib/rpc';
  import { node, prefs, toast, wallets } from '../lib/state.svelte';
  import { filterAndSort, type Column } from '../lib/table';
  import { formatAmount } from '../lib/units';
  import { walletApi, type Coin, type CoinSummary as Summary, type FeeChoice, type Outpoint } from '../lib/wallet';

  let {
    wallet,
    selected = $bindable([]),
    amounts = [],
    subtractFee = false,
    fee = { confTarget: 6 },
    purpose = 'send',
  }: { wallet: string; selected?: Outpoint[]; amounts?: bigint[]; subtractFee?: boolean; fee?: FeeChoice; purpose?: 'send' | 'prove' } = $props();
  const proving = $derived(purpose === 'prove');

  type Row = Coin & { key: string };
  type Show = 'all' | 'free' | 'unconfirmed' | 'locked';

  const keyOf = (o: Outpoint) => `${o.txid}:${o.vout}`;
  const api = $derived(walletApi(wallet));

  let coins: Row[] = $state.raw([]);
  let loaded = $state(false);
  let error = $state('');
  let filter = $state('');
  let show: Show = $state('all');
  let sortKey: string | null = $state('amount');
  let sortDir: 'asc' | 'desc' = $state('desc');
  let current: string | null = $state(null);
  let summary = $state<Summary | null>(null);
  let menu = $state({ open: false, x: 0, y: 0 });
  let now = $state(Math.floor(Date.now() / 1000));

  const chosen = $derived(new Set(selected.map(keyOf)));

  async function load() {
    try {
      const groups = await api.coins();
      coins = groups.flatMap((g) => g.coins.map((c) => ({ ...c, label: c.change ? '' : c.label, key: keyOf(c) })));
      now = Math.floor(Date.now() / 1000);
      error = '';
      // Locked coins (unless proven), and coins spent meanwhile, are no longer chosen.
      const free = new Set(coins.filter((c) => proving || !c.locked).map((c) => c.key));
      if (selected.some((o) => !free.has(keyOf(o)))) selected = selected.filter((o) => free.has(keyOf(o)));
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    }
    loaded = true;
  }

  // Again with each transaction of the wallet and each block (the confirmations).
  $effect(() => {
    void wallets.txVersion;
    void node.tip?.height;
    void wallet;
    load();
  });

  const toSend = $derived(amounts.reduce((s, a) => s + (a > 0n ? a : 0n), 0n));
  const chosenTotal = $derived(coins.reduce((s, c) => (chosen.has(c.key) ? s + c.sats : s), 0n));
  const lockedCount = $derived(coins.reduce((n, c) => n + (c.locked ? 1 : 0), 0));
  /** The coins chosen hold less than the amounts to send. */
  const short = $derived(selected.length > 0 && chosenTotal < toSend);
  /** Enough for the amounts, not with the fee (the host's estimate). */
  const shortOfFee = $derived(selected.length > 0 && !short && !!summary?.insufficient);

  // The estimate of the transaction with the coins chosen (coinControlUpdateLabels).
  $effect(() => {
    const list = selected;
    const a = amounts;
    const s = subtractFee;
    const f = fee;
    if (list.length === 0 || proving) {
      summary = null;
      return;
    }
    const timer = setTimeout(() => {
      api
        .coinSummary({ selected: list }, a, s, f)
        .then((r) => (summary = r))
        .catch(() => (summary = null));
    }, 150);
    return () => clearTimeout(timer);
  });

  const columns: Column<Row>[] = $derived([
    { key: 'pick', label: '', value: (c) => chosen.has(c.key), width: '2.6rem', sortable: false, noFilter: true },
    { key: 'amount', label: `Amount (${prefs.unit})`, value: (c) => c.sats, width: '9.5rem', align: 'end', noFilter: true },
    { key: 'address', label: 'Address', value: (c) => c.address, width: 'minmax(11rem, 2fr)', mono: true },
    { key: 'label', label: 'Label', value: (c) => c.label, width: 'minmax(5rem, 1fr)' },
    { key: 'depth', label: 'Confirmations', value: (c) => c.depth, width: '7.5rem', align: 'end', noFilter: true },
    { key: 'age', label: 'Age', value: (c) => -c.time, width: '5rem', align: 'end', noFilter: true },
    { key: 'locked', label: 'Lock', value: (c) => c.locked, width: '4rem', align: 'center', noFilter: true },
    { key: 'txid', label: 'Transaction', value: (c) => c.key, sortable: false },
  ]);
  // The transaction is searched (here), not shown.
  const shownColumns = $derived(columns.filter((c) => c.key !== 'txid'));

  const rows = $derived(
    coins.filter((c) => (show === 'free' ? !c.locked : show === 'locked' ? c.locked : show === 'unconfirmed' ? c.depth === 0 : true)),
  );
  /** The rows shown (the filter typed too; DataTable sorts them): what "Select all" chooses. */
  const visible = $derived(filterAndSort(rows, columns, filter, null, 'asc'));

  function setChosen(next: Set<string>) {
    selected = coins.filter((c) => next.has(c.key)).map((c) => ({ txid: c.txid, vout: c.vout }));
  }

  function toggle(c: Row) {
    if (c.locked && !proving) return;
    const next = new Set(chosen);
    if (next.has(c.key)) next.delete(c.key);
    else next.add(c.key);
    setChosen(next);
  }

  function selectAll() {
    const next = new Set(chosen);
    for (const c of visible) if (proving || !c.locked) next.add(c.key);
    setChosen(next);
  }

  async function setLocked(c: Row, lock: boolean) {
    try {
      await api.lockCoin(c, lock);
      if (lock && chosen.has(c.key)) selected = selected.filter((o) => keyOf(o) !== c.key);
      await load();
    } catch (e) {
      toast('error', t('Coin control'), e instanceof Error ? e.message : String(e));
    }
  }

  async function copy(text: string) {
    await host.copy(text).catch(() => undefined);
    toast('success', t('Copied'), text.length > 80 ? undefined : text);
  }

  const at = $derived(coins.find((c) => c.key === current) ?? null);
  const menuItems: ContextItem[] = $derived(
    at
      ? [
          { label: 'Copy address', action: () => copy(at.address) },
          { label: 'Copy label', action: () => copy(at.label), disabled: !at.label },
          { label: 'Copy amount', action: () => copy(formatAmount(at.sats, prefs.unit, { separators: false })) },
          { label: 'Copy transaction ID and output index', action: () => copy(at.key), separator: true },
          { label: 'Lock unspent', action: () => setLocked(at, true), disabled: at.locked, separator: true },
          { label: 'Unlock unspent', action: () => setLocked(at, false), disabled: !at.locked },
        ]
      : [],
  );
</script>

<div class="coin-panel">
  <div class="row toolbar">
    <input type="search" class="grow" placeholder="Search address, label or transaction" aria-label="Search the coins" bind:value={filter} />
    <select aria-label="Coins shown" bind:value={show}>
      <option value="all">{t('All coins')}</option>
      <option value="free">{t('Spendable')}</option>
      <option value="unconfirmed">{t('Unconfirmed')}</option>
      <option value="locked">{t('Locked')}</option>
    </select>
    <button type="button" class="btn small" disabled={visible.length === 0} onclick={selectAll}>Select all</button>
    <button type="button" class="btn small" disabled={selected.length === 0} onclick={() => (selected = [])}>Select none</button>
  </div>

  <div class="totals" aria-live="polite">
    <div class="total"><span class="muted">Chosen</span><strong>{coins.length ? `${selected.length} / ${coins.length}` : '0'}</strong></div>
    <div class="total"><span class="muted">The coins chosen hold</span><strong><Amount sats={chosenTotal} /></strong></div>
    {#if !proving}<div class="total"><span class="muted">To send</span><strong><Amount sats={toSend} /></strong></div>{/if}
    {#if lockedCount}<div class="total"><span class="muted">Locked</span><strong class="num">{lockedCount}</strong></div>{/if}
  </div>
  {#if proving}
    <p class="muted note">{selected.length === 0 ? t('No coin chosen yet: tick the coins to prove.') : t('The proof lists these %1 coin(s), and is signed with the addresses they are on.', selected.length)}</p>
  {:else if short}
    <p class="warning" role="alert"><Icon name="alert" size={16} /> {t('Not enough: the coins chosen hold %1, less than the %2 to send. Choose more coins.', formatAmount(chosenTotal, prefs.unit, { withUnit: true }), formatAmount(toSend, prefs.unit, { withUnit: true }))}</p>
  {:else if shortOfFee}
    <p class="warning" role="alert"><Icon name="alert" size={16} /> {t('Not enough: the coins chosen do not pay for the fee too. Choose more coins.')}</p>
  {/if}
  {#if proving}
    <!-- Nothing is spent. -->
  {:else if selected.length === 0}
    <p class="muted note">No coin chosen yet: tick the coins to spend. Only the coins chosen are spent; with none, the wallet chooses.</p>
  {:else}
    <p class="muted note">Only these {selected.length} coin(s) are spent.</p>
    <CoinSummary {summary} />
  {/if}
  {#if error}<p class="error">{error}</p>{/if}

  <div class="holder">
    <DataTable
      rows={visible}
      columns={shownColumns}
      key={(c) => c.key}
      bind:selected={current}
      bind:sortKey
      bind:sortDir
      label={proving ? 'Coins to prove' : 'Coins of the wallet'}
      empty={!loaded ? 'Loading…' : coins.length ? 'Nothing matches the filter' : 'No coins to spend'}
      onactivate={(c) => toggle(c)}
      oncontext={(c, e) => (menu = { open: true, x: e.clientX, y: e.clientY })}
    >
      {#snippet cell(c, column)}
        {#if column.key === 'pick'}
          {#if c.locked && !proving}
            <span class="lock" title="Locked: unlock it to spend it"><Icon name="lock" size={15} label="Locked" /></span>
          {:else}
             <input type="checkbox" aria-label={proving ? t('Prove this coin') : t('Spend this coin')} checked={chosen.has(c.key)} onclick={(e) => e.stopPropagation()} onchange={() => toggle(c)} />
          {/if}
        {:else if column.key === 'amount'}<span class:faint={c.locked}><Amount sats={c.sats} showUnit={false} /></span>
        {:else if column.key === 'address'}<SafeText text={c.address} />
        {:else if column.key === 'label'}{#if c.change}<span class="faint">(change)</span>{:else if c.label}<SafeText text={c.label} />{:else}<span class="faint">(no label)</span>{/if}
        {:else if column.key === 'depth'}{#if c.depth === 0}<span class="status wait">{t('Unconfirmed')}</span>{:else}{c.depth}{/if}
        {:else if column.key === 'age'}<span title={new Date(c.time * 1000).toLocaleString()}>{shortDuration(now - c.time)}</span>
        {:else}
          <button type="button" class="btn ghost small lockbtn" title={c.locked ? 'Unlock this coin: it can be spent again' : 'Lock this coin: the wallet does not spend it'} aria-label={c.locked ? 'Unlock coin' : 'Lock coin'} onclick={(e) => { e.stopPropagation(); setLocked(c, !c.locked); }}>
            <Icon name={c.locked ? 'lock' : 'unlock'} size={15} />
          </button>
        {/if}
      {/snippet}
    </DataTable>
  </div>
</div>

<ContextMenu items={menuItems} x={menu.x} y={menu.y} bind:open={menu.open} label="Coin" />

<style>
  .coin-panel {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
  }
  .toolbar {
    flex-wrap: wrap;
  }
  .toolbar input {
    min-width: 12rem;
  }
  .toolbar select {
    width: auto;
  }
  .totals {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem 1.4rem;
  }
  .total {
    display: flex;
    flex-direction: column;
    gap: 0.1rem;
  }
  .total span {
    font-size: 0.8rem;
  }
  .total strong {
    font-weight: 600;
  }
  .warning {
    display: flex;
    align-items: center;
    gap: 0.45rem;
    margin: 0;
    padding: 0.55rem 0.8rem;
    border: 1px solid var(--negative);
    border-radius: var(--radius-s);
    color: var(--negative);
    font-weight: 600;
  }
  .note {
    margin: 0;
    font-size: 0.9rem;
  }
  .holder {
    height: 20rem;
  }
  .lock {
    color: var(--text-muted);
    display: inline-flex;
  }
  .lockbtn {
    padding: 0.15rem 0.4rem;
  }
</style>
