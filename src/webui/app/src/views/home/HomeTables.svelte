<script lang="ts">
  // The tables of the dashboard's home, in tabs: the recent transactions, the sidechains, the latest
  // blocks, the peers. Sortable headers, the state of each row as a coloured pill, a small trend,
  // and the actions of each row. The name of a row opens it in the detail panel.
  import { t } from '../../lib/i18n.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import Sparkline from '../../components/Sparkline.svelte';
  import { pageDisabled } from '../../lib/commands.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { detail, showDetail } from '../../lib/detail.svelte';
  import { formatCount, formatTime, niceTimeOffset, shortHash } from '../../lib/format';
  import { go, goTo } from '../../lib/nav.svelte';
  import { host } from '../../lib/rpc';
  import { prefs, toast, wallets } from '../../lib/state.svelte';
  import { bumpFee } from '../../lib/txactions';
  import { stacked } from '../../lib/actions';
  import { coinsToSats } from '../../lib/units';
  import { addressText, typeText, walletApi, type TxRecord } from '../../lib/wallet';
  import { current } from '../../lib/walletdata.svelte';

  interface Peer {
    id: number;
    addr: string;
    inbound: boolean;
    pingtime?: number;
    synced_blocks: number;
  }
  interface Sidechain {
    slot: number;
    title: string;
    escrow?: { amount: number };
  }
  interface Bundle {
    slot: number;
    hash: string;
    score: number;
    blocksleft: number;
  }
  interface BlockRow {
    height: number;
    hash: string;
    time: number;
    txs: number;
  }

  let {
    recent,
    history,
    sidechains,
    bundles,
    bmm,
    bmmSeries,
    bmmWindow,
    blocks,
    peers,
    now,
    hide = [],
  }: {
    recent: TxRecord[];
    history: { time: number; balance: bigint }[];
    sidechains: Sidechain[];
    bundles: Bundle[];
    bmm: Record<number, number>;
    bmmSeries: Record<number, number[]>;
    bmmWindow: number;
    blocks: BlockRow[];
    peers: Peer[];
    now: number;
    /** Tabs not shown (a sidechain has no sidechains of its own). */
    hide?: string[];
  } = $props();

  type Tab = 'transactions' | 'sidechains' | 'blocks' | 'peers';
  const ALL_TABS: { id: Tab; label: string; open: string }[] = [
    { id: 'transactions', label: 'Transactions', open: 'All transactions' },
    { id: 'sidechains', label: 'Sidechains', open: 'Open the sidechains' },
    { id: 'blocks', label: 'Blocks', open: 'Block explorer' },
    { id: 'peers', label: 'Peers', open: 'All peers' },
  ];
  const TABS = $derived(ALL_TABS.filter((tb) => !hide.includes(tb.id)));
  let tab: Tab = $state('transactions');
  const counts = $derived({ transactions: recent.length, sidechains: sidechains.length, blocks: blocks.length, peers: peers.length });

  function openTab() {
    if (tab === 'transactions') go('transactions');
    else if (tab === 'sidechains') go('sidechains');
    else if (tab === 'blocks') go('explorer');
    else go('node', 'peers');
  }

  function tabKey(e: KeyboardEvent, i: number) {
    if (e.key !== 'ArrowRight' && e.key !== 'ArrowLeft') return;
    e.preventDefault();
    const next = TABS[(i + (e.key === 'ArrowRight' ? 1 : -1) + TABS.length) % TABS.length];
    tab = next.id;
    queueMicrotask(() => document.getElementById(`home-tab-${next.id}`)?.focus());
  }

  // --- Sorting: one column of each table, up or down ---
  let sort: Record<Tab, { key: string; dir: 1 | -1 }> = $state({
    transactions: { key: 'date', dir: -1 },
    sidechains: { key: 'slot', dir: 1 },
    blocks: { key: 'height', dir: -1 },
    peers: { key: 'ping', dir: 1 },
  });
  function sortBy(t: Tab, key: string) {
    const s = sort[t];
    sort[t] = s.key === key ? { key, dir: s.dir === 1 ? -1 : 1 } : { key, dir: key === 'date' || key === 'amount' || key === 'height' ? -1 : 1 };
  }
  function ariaSort(t: Tab, key: string): 'ascending' | 'descending' | 'none' {
    return sort[t].key === key ? (sort[t].dir === 1 ? 'ascending' : 'descending') : 'none';
  }
  function sorted<T>(rows: T[], t: Tab, value: (row: T, key: string) => number | string | bigint): T[] {
    const { key, dir } = sort[t];
    return [...rows].sort((a, b) => {
      const va = value(a, key);
      const vb = value(b, key);
      return (va < vb ? -1 : va > vb ? 1 : 0) * dir;
    });
  }

  // --- Transactions ---
  function txState(r: TxRecord): { label: string; tone: 'ok' | 'wait' | 'bad' } {
    switch (r.status) {
      case 'confirmed':
        return { label: 'Confirmed', tone: 'ok' };
      case 'immature':
        return { label: 'Immature', tone: 'wait' };
      case 'conflicted':
        return { label: 'Conflicted', tone: 'bad' };
      case 'abandoned':
        return { label: 'Abandoned', tone: 'bad' };
      case 'notaccepted':
        return { label: 'Not accepted', tone: 'bad' };
      case 'confirming':
        return { label: `${r.depth} of 6`, tone: 'wait' };
      default:
        return { label: 'Pending', tone: 'wait' };
    }
  }
  /** The balance of the wallet up to the transaction: its last steps. */
  function trend(r: TxRecord): number[] {
    const before = history.filter((p) => p.time <= r.time).slice(-12);
    return before.map((p) => Number(p.balance));
  }
  const txRows = $derived(sorted(recent, 'transactions', (r, k) => (k === 'amount' ? r.amount : k === 'status' ? txState(r).label : r.time)));
  const bumpable = (r: TxRecord) => r.depth === 0 && r.amount < 0n && r.status !== 'abandoned' && r.status !== 'conflicted';

  function details(r: TxRecord) {
    if (wallets.current !== null) showDetail({ kind: 'tx', wallet: wallets.current, txid: r.txid, idx: r.idx, address: r.address });
  }
  async function bump(r: TxRecord) {
    const wallet = wallets.current;
    if (wallet === null) return;
    try {
      const can = await walletApi(wallet).txActions(r.txid);
      if (!can.canBump) {
        toast('info', t('Increase transaction fee'), t('The fee of this transaction cannot be increased (it is confirmed, or does not signal replacement).'));
        return;
      }
    } catch (e) {
      toast('error', t('Increase transaction fee'), errorText(e));
      return;
    }
    await bumpFee(wallet, r.txid);
  }
  async function copy(text: string, what: string) {
    try {
      await host.copy(text);
      toast('success', t('Copied'), what);
    } catch (e) {
      toast('error', t('Could not copy'), errorText(e));
    }
  }

  // --- Sidechains, blocks, peers ---
  const scRows = $derived(sorted(sidechains, 'sidechains', (s, k) => (k === 'escrow' ? (s.escrow?.amount ?? 0) : k === 'name' ? s.title.toLowerCase() : k === 'bmm' ? (bmm[s.slot] ?? 0) : s.slot)));
  const blockRows = $derived(sorted(blocks, 'blocks', (b, k) => (k === 'txs' ? b.txs : b.height)));
  const peerRows = $derived(sorted(peers, 'peers', (p, k) => (k === 'addr' ? p.addr : k === 'synced' ? p.synced_blocks : (p.pingtime ?? 1e9))));

  const selected = $derived(detail.item);
  function isSelected(kind: string, key: string | number): boolean {
    if (!selected || selected.kind !== kind) return false;
    if (selected.kind === 'tx') return selected.txid === key;
    if (selected.kind === 'block') return selected.hash === key;
    if (selected.kind === 'peer') return selected.id === key;
    return selected.slot === key;
  }
</script>

{#snippet head(t: Tab, key: string, label: string, end = false)}
  <th scope="col" aria-sort={ariaSort(t, key)} class:end>
    <button type="button" class="sort" onclick={() => sortBy(t, key)}>
      {label}
      <span class="arrow" class:on={sort[t].key === key} aria-hidden="true"><Icon name={sort[t].key === key && sort[t].dir === 1 ? 'up' : 'down'} size={12} /></span>
    </button>
  </th>
{/snippet}

<section class="panel tables" aria-label="Activity">
  <header>
    <div class="tabs-row" role="tablist" aria-label="Activity">
      {#each TABS as tb, i (tb.id)}
        <button
          type="button"
          role="tab"
          id="home-tab-{tb.id}"
          aria-selected={tab === tb.id}
          aria-controls="home-panel"
          tabindex={tab === tb.id ? 0 : -1}
          onclick={() => (tab = tb.id)}
          onkeydown={(e) => tabKey(e, i)}
        >
          {t(tb.label)}
          {#if counts[tb.id]}<span class="count">{counts[tb.id]}</span>{/if}
        </button>
      {/each}
    </div>
    <button type="button" class="btn ghost small open" onclick={openTab}>{TABS.find((t) => t.id === tab)?.open} <Icon name="arrow" size={15} /></button>
  </header>

  <div class="scroll" id="home-panel" role="tabpanel" aria-labelledby="home-tab-{tab}">
    {#if tab === 'transactions'}
      {#if wallets.current === null}
        <p class="muted empty">No wallet loaded.</p>
      {:else if recent.length === 0}
        <p class="muted empty">{current.recordsLoaded ? 'No transactions yet. Receive coins to see them here.' : 'Loading…'}</p>
      {:else}
        <table use:stacked aria-label="Recent transactions">
          <thead>
            <tr>
              <th scope="col">Transaction</th>
              {@render head('transactions', 'date', 'Date')}
              {@render head('transactions', 'status', 'Status')}
              <th scope="col" class="trend-col"><span class="sr-only">Balance before</span></th>
              {@render head('transactions', 'amount', 'Amount', true)}
              <th scope="col" class="end"><span class="sr-only">Actions</span></th>
            </tr>
          </thead>
          <tbody>
            {#each txRows as r (r.key)}
              {@const st = txState(r)}
              <tr class:selected={isSelected('tx', r.txid)}>
                <td class="name">
                  <button type="button" class="row-name" disabled={prefs.mask} title={prefs.mask ? 'Hidden: privacy mode' : 'Show the details'} onclick={() => details(r)}>
                    <span class="bubble" class:in={r.amount >= 0n}><Icon name={r.type === 'mined' ? 'mine' : r.amount < 0n ? 'send' : 'receive'} size={16} /></span>
                    <span class="what">
                      <span class="ellipsis strong">{#if addressText(r)}<SafeText text={addressText(r)} />{:else}{typeText(r.type, r.role) || 'Other'}{/if}</span>
                      <span class="faint small">{typeText(r.type, r.role) || 'Other'}</span>
                    </span>
                  </button>
                </td>
                <td class="muted small nowrap">{formatTime(r.time)}</td>
                <td><span class="status {st.tone}">{t(st.label)}</span></td>
                <td class="trend-col"><Sparkline values={prefs.mask ? [] : trend(r)} tone={r.amount >= 0n ? 'up' : 'down'} /></td>
                <td class="end"><Amount sats={r.amount} colored plus /></td>
                <td class="end actions">
                  <button type="button" class="btn small ghost" disabled={prefs.mask} onclick={() => details(r)}>Details</button>
                  <button type="button" class="btn small ghost" disabled={!bumpable(r)} title="Increase the fee of this transaction" onclick={() => bump(r)}>Bump fee</button>
                  <button type="button" class="btn small ghost icon" aria-label="Copy transaction ID" title="Copy transaction ID" onclick={() => copy(r.txid, 'Transaction ID')}><Icon name="copy" size={15} /></button>
                </td>
              </tr>
            {/each}
          </tbody>
        </table>
      {/if}
    {:else if tab === 'sidechains'}
      {#if sidechains.length === 0}
        <p class="muted empty">No sidechain is active yet. Proposals and votes are on the Sidechains page.</p>
      {:else}
        <table use:stacked aria-label="Active sidechains">
          <thead>
            <tr>
              {@render head('sidechains', 'name', 'Sidechain')}
              {@render head('sidechains', 'escrow', 'Escrow', true)}
              <th scope="col">Bundles</th>
              {@render head('sidechains', 'bmm', 'Merged mining')}
              <th scope="col" class="end"><span class="sr-only">Actions</span></th>
            </tr>
          </thead>
          <tbody>
            {#each scRows as s (s.slot)}
              {@const pending = bundles.filter((b) => b.slot === s.slot)}
              <tr class:selected={isSelected('sidechain', s.slot)}>
                <td class="name">
                  <button type="button" class="row-name" onclick={() => showDetail({ kind: 'sidechain', slot: s.slot })}>
                    <span class="bubble slot num">{s.slot}</span>
                    <span class="what"><span class="ellipsis strong"><SafeText text={s.title} /></span><span class="faint small">Slot {s.slot}</span></span>
                  </button>
                </td>
                <td class="end"><Amount sats={coinsToSats(s.escrow?.amount ?? 0)} /></td>
                <td>
                  {#if pending.length === 0}
                    <span class="status">None</span>
                  {:else}
                    {#each pending as b (b.hash)}
                      <span class="status {b.score > 0 ? 'ok' : 'bad'}" title="Withdrawal bundle {shortHash(b.hash, 6)}: {b.blocksleft} blocks left to vote">
                        <Icon name={b.score > 0 ? 'up' : 'down'} size={11} />{b.score} vote{b.score === 1 ? '' : 's'}
                      </span>
                    {/each}
                  {/if}
                </td>
                <td>
                  <span class="bmm">
                    <Sparkline values={bmmSeries[s.slot] ?? []} tone={(bmm[s.slot] ?? 0) > 0 ? 'up' : 'flat'} />
                    <span class="num small">{bmm[s.slot] ?? 0}/{bmmWindow}</span>
                  </span>
                </td>
                <td class="end actions">
                  <button type="button" class="btn small" disabled={pageDisabled('sidechains')} onclick={() => goTo('sidechains', 'sidechains', String(s.slot))}>Deposit</button>
                  <button type="button" class="btn small ghost" disabled={pageDisabled('sidechains')} onclick={() => goTo('sidechains', 'withdrawals', String(s.slot))}>Withdrawals</button>
                  <button type="button" class="btn small ghost" onclick={() => showDetail({ kind: 'sidechain', slot: s.slot })}>View</button>
                </td>
              </tr>
            {/each}
          </tbody>
        </table>
      {/if}
    {:else if tab === 'blocks'}
      {#if blocks.length === 0}
        <p class="muted empty">Waiting for the node…</p>
      {:else}
        <table use:stacked aria-label="Latest blocks">
          <thead>
            <tr>
              {@render head('blocks', 'height', 'Block')}
              <th scope="col">Hash</th>
              {@render head('blocks', 'txs', 'Transactions', true)}
              <th scope="col" class="end">Age</th>
              <th scope="col" class="end"><span class="sr-only">Actions</span></th>
            </tr>
          </thead>
          <tbody>
            {#each blockRows as b (b.hash)}
              <tr class:selected={isSelected('block', b.hash)}>
                <td class="name">
                  <button type="button" class="row-name" onclick={() => showDetail({ kind: 'block', hash: b.hash })}>
                    <span class="bubble"><Icon name="block" size={15} /></span>
                    <span class="strong num">{formatCount(b.height)}</span>
                  </button>
                </td>
                <td class="mono muted small">{shortHash(b.hash, 8)}</td>
                <td class="end num">{b.txs} tx{b.txs === 1 ? '' : 's'}</td>
                <td class="end muted small nowrap">{niceTimeOffset(Math.max(0, now - b.time))} ago</td>
                <td class="end actions">
                  <button type="button" class="btn small ghost" onclick={() => showDetail({ kind: 'block', hash: b.hash })}>View</button>
                  <button type="button" class="btn small ghost icon" aria-label="Copy block hash" title="Copy block hash" onclick={() => copy(b.hash, 'Block hash')}><Icon name="copy" size={15} /></button>
                </td>
              </tr>
            {/each}
          </tbody>
        </table>
      {/if}
    {:else if peers.length === 0}
      <p class="muted empty">No peers connected.</p>
    {:else}
      <table use:stacked aria-label="Peers">
        <thead>
          <tr>
            {@render head('peers', 'addr', 'Peer')}
            <th scope="col">Direction</th>
            {@render head('peers', 'ping', 'Ping', true)}
            {@render head('peers', 'synced', 'Synced blocks', true)}
            <th scope="col" class="end"><span class="sr-only">Actions</span></th>
          </tr>
        </thead>
        <tbody>
          {#each peerRows as p (p.id)}
            <tr class:selected={isSelected('peer', p.id)}>
              <td class="name">
                <button type="button" class="row-name" onclick={() => showDetail({ kind: 'peer', id: p.id })}>
                  <span class="bubble"><Icon name="peers" size={15} /></span>
                  <span class="mono ellipsis">{p.addr}</span>
                </button>
              </td>
              <td><span class="status {p.inbound ? '' : 'ok'}">{p.inbound ? 'Inbound' : 'Outbound'}</span></td>
              <td class="end num">{p.pingtime !== undefined ? `${Math.round(p.pingtime * 1000)} ms` : '–'}</td>
              <td class="end num">{formatCount(p.synced_blocks)}</td>
              <td class="end actions">
                <button type="button" class="btn small ghost" onclick={() => showDetail({ kind: 'peer', id: p.id })}>View</button>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    {/if}
  </div>
</section>

<style>
  .tables {
    min-width: 0;
  }
  header {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
    padding: 0.9rem 1rem 0.3rem 1.1rem;
  }
  .tabs-row {
    display: flex;
    gap: 0.2rem;
    overflow-x: auto;
  }
  .tabs-row button {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    padding: 0.45rem 0.85rem;
    border: 0;
    border-bottom: 2px solid transparent;
    background: none;
    color: var(--text-muted);
    font: inherit;
    font-weight: 600;
    cursor: pointer;
    white-space: nowrap;
  }
  .tabs-row button:hover {
    color: var(--text);
  }
  .tabs-row button[aria-selected='true'] {
    color: var(--text);
    border-bottom-color: var(--accent);
  }
  .count {
    min-width: 1.4rem;
    padding: 0 0.4rem;
    border-radius: 999px;
    background: var(--raised);
    color: var(--text-muted);
    font-size: 0.75rem;
    text-align: center;
  }
  .open {
    color: var(--text-muted);
    border-radius: 999px;
  }
  .scroll {
    overflow-x: auto;
    padding: 0 0.8rem 0.9rem;
    container-type: inline-size;
  }
  /* A panel too narrow for the columns (a window of 700 or 480 px): each row is a card, every cell under the
     words of its column (use:stacked), none hidden behind a sideways scroll (audit port-2 #14). */
  @container (max-width: 52rem) {
    table,
    tbody,
    tr {
      display: block;
    }
    thead {
      display: none;
    }
    tr {
      margin: 0.4rem 0;
      padding: 0.35rem 0;
      border: 1px solid var(--line);
      border-radius: var(--radius-l);
      background: var(--surface-2);
    }
    tr.selected {
      border-color: var(--accent);
      background: var(--accent-soft);
    }
    td,
    td:first-child,
    td:last-child {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 1rem;
      min-width: 0;
      padding: 0.25rem 0.8rem;
      border: 0;
      border-radius: 0;
      background: none;
      text-align: right;
      overflow-wrap: anywhere;
    }
    tr:hover td,
    tr.selected td {
      background: none;
    }
    td:global([data-label])::before {
      content: attr(data-label);
      flex: none;
      color: var(--text-muted);
      font-size: 0.8rem;
      text-align: left;
    }
    td.trend-col {
      display: none;
    }
    .row-name {
      max-width: 100%;
    }
    .actions {
      flex-wrap: wrap;
      justify-content: flex-end;
      white-space: normal;
    }
  }
  table {
    width: 100%;
    border-collapse: separate;
    border-spacing: 0 0.35rem;
    font-size: 0.93rem;
  }
  th {
    padding: 0.3rem 0.6rem;
    color: var(--text-faint);
    font-size: 0.78rem;
    font-weight: 600;
    text-align: left;
    white-space: nowrap;
  }
  .sort {
    display: inline-flex;
    align-items: center;
    gap: 0.25rem;
    padding: 0;
    border: 0;
    background: none;
    color: inherit;
    font: inherit;
    cursor: pointer;
  }
  .sort:hover {
    color: var(--text);
  }
  .arrow {
    display: inline-flex;
    opacity: 0;
  }
  .arrow.on,
  .sort:hover .arrow {
    opacity: 1;
  }
  .end {
    text-align: right;
  }
  th.end .sort {
    flex-direction: row-reverse;
  }
  td {
    padding: 0.5rem 0.6rem;
    background: var(--surface-2);
    border-top: 1px solid var(--line);
    border-bottom: 1px solid var(--line);
    vertical-align: middle;
  }
  td:first-child {
    border-left: 1px solid var(--line);
    border-radius: var(--radius-l) 0 0 var(--radius-l);
  }
  td:last-child {
    border-right: 1px solid var(--line);
    border-radius: 0 var(--radius-l) var(--radius-l) 0;
  }
  tr:hover td {
    background: var(--raised);
  }
  tr.selected td {
    border-color: var(--accent);
    background: var(--accent-soft);
  }
  .row-name {
    display: flex;
    align-items: center;
    gap: 0.7rem;
    min-width: 0;
    max-width: 20rem;
    padding: 0;
    border: 0;
    background: none;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .row-name:disabled {
    cursor: default;
  }
  .bubble {
    display: inline-grid;
    place-items: center;
    width: 2.2rem;
    height: 2.2rem;
    flex: none;
    border-radius: 50%;
    background: var(--raised);
    color: var(--text-muted);
  }
  .bubble.in,
  .bubble.slot {
    background: var(--accent-soft);
    color: var(--accent-strong);
  }
  .slot {
    font-weight: 700;
  }
  .what {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }
  .strong {
    font-weight: 600;
  }
  .ellipsis {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .small {
    font-size: 0.84rem;
  }
  .nowrap {
    white-space: nowrap;
  }
  .trend-col {
    width: 6.5rem;
  }
  .bmm {
    display: inline-flex;
    align-items: center;
    gap: 0.5rem;
  }
  .actions {
    white-space: nowrap;
  }
  .actions :global(.btn.small) {
    min-height: 1.85rem;
    padding: 0 0.6rem;
    font-size: 0.84rem;
  }
  .actions :global(.btn.small.icon) {
    width: 1.85rem;
    padding: 0;
  }
  .status + .status {
    margin-left: 0.25rem;
  }
  .empty {
    margin: 0;
    padding: 1rem 0.4rem 0.6rem;
  }
  @media (max-width: 900px) {
    .trend-col {
      display: none;
    }
  }
</style>
