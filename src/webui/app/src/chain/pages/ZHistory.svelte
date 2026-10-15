<script lang="ts">
  // Private activity: the transactions of the shielded account, told in words (shielded, received privately,
  // paid privately, unshielded), each with its privacy, its memos and whom it paid; a payment that waits for
  // a block can pay a higher fee (bumpshieldedfee). And its notes, spent or not, each with its memo and what
  // it is (received, change, watch-only). The History and Notes tabs of zside-qt's Private page, redone.
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { stacked } from '../../lib/actions';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { formatTime, shortHash } from '../../lib/format';
  import { t, tn } from '../../lib/i18n.svelte';
  import { go, nav, PAGES } from '../../lib/nav.svelte';
  import { prefs, toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import { coinsToSats, formatAmount, satsToCoins } from '../../lib/units';
  import '../zs/zs.css';
  import Privacy from '../zs/Privacy.svelte';
  import ProvingPanel from '../zs/ProvingPanel.svelte';
  import { describeTx, type ShieldedTx } from '../zs/model';
  import OlderNote from '../zs/OlderNote.svelte';
  import { HISTORY_POINTS, prove, proving, setHistoryCount, setSpentNotes, startZs, zs, zsView } from '../zs/store.svelte';
  import Pager from '../../components/Pager.svelte';
  import { formatCount } from '../../lib/format';

  $effect(() => startZs());

  const TABS = [
    { id: 'transactions', label: 'Transactions' },
    { id: 'notes', label: 'Notes' },
  ];
  const tab = $derived(nav.tab === 'notes' ? 'notes' : 'transactions');
  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  const busy = $derived(proving.now !== null);

  // --- Transactions ---
  type Filter = 'all' | 'in' | 'out' | 'public';
  let filter = $state<Filter>('all');
  let query = $state('');
  const txs = $derived(zs.txs.map(describeTx));
  const shown = $derived.by(() => {
    const q = query.trim().toLowerCase();
    return txs.filter((x) => {
      if (filter === 'in' && x.kind !== 'receive') return false;
      if (filter === 'out' && x.kind !== 'pay' && x.kind !== 'unshield') return false;
      if (filter === 'public' && !x.public) return false;
      if (!q) return true;
      return x.txid.includes(q) || x.memos.some((m) => m.toLowerCase().includes(q)) || x.to.some((p) => p.address.includes(q) || p.memo.toLowerCase().includes(q));
    });
  });
  // A table of pages (user decision): 25, 50 or 100 rows; how many of the newest are read: HISTORY_POINTS.
  const PAGE_SIZES = [25, 50, 100] as const;
  let page = $state(0);
  let pageSize = $state(25);
  const pageRows = $derived(shown.slice(page * pageSize, (page + 1) * pageSize));
  // Another filter or search: from the first page.
  $effect(() => {
    void filter;
    void query;
    page = 0;
  });
  let openTx = $state('');
  $effect(() => {
    if (nav.tab.startsWith('tx-')) openTx = nav.tab.slice(3);
  });

  function kindWord(kind: ShieldedTx['kind']): string {
    return kind === 'shield' ? t('Shielded') : kind === 'unshield' ? t('Unshielded') : kind === 'pay' ? t('Paid privately') : t('Received privately');
  }
  function kindIcon(kind: ShieldedTx['kind']) {
    return kind === 'shield' ? ('shield' as const) : kind === 'unshield' ? ('eye' as const) : kind === 'pay' ? ('send' as const) : ('receive' as const);
  }

  let bumpFee = $state<bigint | null>(null);
  async function bump(tx: ShieldedTx) {
    const wallet = wallets.current;
    if (wallet === null || busy) return;
    const r = await ask({
      title: t('Pay a higher fee'),
      message: t('Pay the same again with a higher fee? The new transaction replaces this one: it spends the same notes and pays the same recipients, with less change.'),
      informative: bumpFee !== null ? t('The new fee: %1.', formatAmount(bumpFee, prefs.unit)) : t('The new fee: the least a replacement pays, or the wallet\'s fee rate if more.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Pay a higher fee', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    try {
      const done = await prove('bump', 'Proving the replacement', () =>
        walletRpc<{ txid: string; origfee: number; fee: number }>(wallet, 'bumpshieldedfee', bumpFee !== null ? [tx.txid, satsToCoins(bumpFee)] : [tx.txid]),
      );
      if (!done) return;
      toast('success', t('Fee raised'), t('From %1 to %2. Transaction %3.', formatAmount(coinsToSats(done.origfee), prefs.unit), formatAmount(coinsToSats(done.fee), prefs.unit), shortHash(done.txid, 10)));
      openTx = done.txid;
      bumpFee = null;
    } catch (e) {
      await inform(t('Pay a higher fee'), errorText(e), 'error');
    }
  }

  // --- Notes ---
  let showSpent = $state(false);
  // The spent notes are read only while they are shown here (listshieldednotes [true]).
  $effect(() => {
    setSpentNotes(showSpent && tab === 'notes');
  });
  $effect(() => () => setSpentNotes(false));
  const notes = $derived(zs.notes.filter((n) => showSpent || !n.spent_by).slice().sort((a, b) => a.confirmations - b.confirmations || b.amount - a.amount));
  const unspentTotal = $derived(zs.notes.filter((n) => !n.spent_by && !n.watchonly).reduce((s, n) => s + coinsToSats(n.amount), 0n));
  function noteKind(n: (typeof zs.notes)[number]): string {
    if (n.viewonly) return t('watched');
    if (n.watchonly) return t('watch-only');
    if (n.retired) return t('of a former account');
    return n.change ? t('change') : t('received');
  }
</script>

<div class="page zhistory zs">
  <header class="page-head"><h1>{t(PAGES.zhistory.title)}</h1></header>

  {#if noWallet}
    <NoWallet page={PAGES.zhistory} />
  {:else}
    <TabRow label="Private activity" tabs={TABS} current={tab} select={(id) => (nav.tab = id)} />
    <ProvingPanel />

    {#if tab === 'transactions'}
      <div class="bar">
        <div class="seg" role="radiogroup" aria-label="Show">
          {#each [{ id: 'all', label: 'All' }, { id: 'in', label: 'Received' }, { id: 'out', label: 'Paid' }, { id: 'public', label: 'Public' }] as f (f.id)}
            <button type="button" role="radio" aria-checked={filter === f.id} class:on={filter === f.id} onclick={() => (filter = f.id as Filter)}>{t(f.label)}</button>
          {/each}
        </div>
        <input type="search" class="grow" placeholder="Search memos, addresses, transactions" aria-label="Search memos, addresses, transactions" bind:value={query} />
        <label class="points">
          <span class="faint small">Latest</span>
          <select aria-label="How many of the newest transactions to read" value={zsView.historyCount} onchange={(e) => { setHistoryCount(Number((e.currentTarget as HTMLSelectElement).value)); page = 0; }}>
            {#each HISTORY_POINTS as n (n)}<option value={n}>{n === 0 ? t('All') : formatCount(n)}</option>{/each}
            {#if !(HISTORY_POINTS as readonly number[]).includes(zsView.historyCount)}<option value={zsView.historyCount}>{formatCount(zsView.historyCount)}</option>{/if}
          </select>
        </label>
      </div>

      <OlderNote what="transactions" />
      <section class="panel">
        {#if txs.length === 0}
          <p class="muted pad">Nothing yet: shield some coins to start.</p>
        {:else if shown.length === 0}
          <p class="muted pad">No transaction matches.</p>
        {:else}
          <table class="txs" aria-label="Shielded transactions" use:stacked>
            <thead>
              <tr><th class="ex"><span class="sr-only">Details</span></th><th>Type</th><th>Date</th><th>Memos</th><th>Privacy</th><th class="end">Confirmations</th><th class="end">Amount</th></tr>
            </thead>
            <tbody>
              {#each pageRows as tx (tx.txid)}
                {@const isOpen = openTx === tx.txid}
                {@const memoCount = tx.memos.length + tx.to.filter((p) => p.memo).length}
                <tr class:open={isOpen} onclick={() => (openTx = isOpen ? '' : tx.txid)}>
                  <td class="ex"><button type="button" class="btn small ghost icon expand" class:on={isOpen} aria-expanded={isOpen} aria-label={t('Details of %1', shortHash(tx.txid, 8))} onclick={(e) => { e.stopPropagation(); openTx = isOpen ? '' : tx.txid; }}><Icon name="chevron-right" size={15} /></button></td>
                  <td><span class="kind"><span class="ico {tx.kind}"><Icon name={kindIcon(tx.kind)} size={15} /></span><strong>{kindWord(tx.kind)}</strong></span></td>
                  <td class="small">{formatTime(tx.time, true)}</td>
                  <td>{#if memoCount > 0}<span class="memos-count"><Icon name="memo" size={13} />{tn('%n memo(s)', memoCount)}</span>{:else}<span class="faint">–</span>{/if}</td>
                  <td><Privacy kind={tx.public ? 'public' : 'private'} why={tx.kind === 'shield' ? t('A shielding: the amount that went into the pool shows on the chain, and the transparent coins it came from.') : tx.kind === 'unshield' ? t('An unshielding: the amount and the transparent address show on the chain; where the coins came from does not.') : ''} /></td>
                  <td class="end small conf" class:wait={tx.confirmations === 0}>{tx.confirmations === 0 ? t('waiting') : formatCount(tx.confirmations)}</td>
                  <td class="end num"><Amount sats={tx.amount} plus /></td>
                </tr>
                {#if isOpen}
                  <tr class="detail-row"><td colspan="7">
                  <div class="detail">
                        {#if tx.memos.length}
                          <div>
                            <h3>Memos received</h3>
                            {#each tx.memos as m, i (i)}<p class="memo"><SafeText class="zs-memo" text={m} /></p>{/each}
                          </div>
                        {/if}
                        {#if tx.to.length}
                          <div>
                            <h3>Paid to</h3>
                            <table class="paid" use:stacked>
                              <thead><tr><th>Address</th><th class="end">Amount</th><th>Memo</th></tr></thead>
                              <tbody>
                                {#each tx.to as p, i (i)}
                                  <tr>
                                    <td class="mono"><SafeText text={shortHash(p.address, 14)} title={p.address} /> <CopyButton text={p.address} label="Copy the address" /></td>
                                    <td class="end num"><Amount sats={p.amount} /></td>
                                    <td><SafeText class="zs-memo" text={p.memo} /></td>
                                  </tr>
                                {/each}
                              </tbody>
                            </table>
                          </div>
                        {/if}
                        <dl class="facts">
                          <dt>Transaction</dt><dd class="mono"><SafeText text={tx.txid} /> <CopyButton text={tx.txid} label="Copy the transaction id" /></dd>
                          {#if tx.kind === 'pay'}<dt>Fee</dt><dd><Amount sats={tx.fee} /></dd>{/if}
                          <dt>Confirmations</dt><dd class="num">{tx.confirmations}</dd>
                        </dl>
                        {#if tx.confirmations === 0 && (tx.kind === 'pay' || tx.kind === 'unshield')}
                          <div class="bump">
                            <AmountInput id={`zs-bump-${tx.txid}`} label="New fee (empty: the least that replaces it)" bind:value={bumpFee} disabled={busy} />
                            <button type="button" class="btn" disabled={busy} onclick={() => bump(tx)}><Icon name="up" size={15} /> Pay a higher fee</button>
                          </div>
                        {/if}
                      </div>
                  </td></tr>
                {/if}
              {/each}
            </tbody>
          </table>
          <Pager total={shown.length} bind:page bind:size={pageSize} sizes={PAGE_SIZES} label="transactions" />
        {/if}
      </section>
    {:else}
      <div class="bar">
        <label class="check"><input type="checkbox" bind:checked={showSpent} /> Show the spent notes</label>
        <span class="muted small">{t('Unspent')}: <Amount sats={unspentTotal} /></span>
        <span class="grow"></span>
        <button type="button" class="btn ghost small" onclick={() => go('pool')}><Icon name="pool" size={15} /> The pool</button>
      </div>
      <section class="panel">
        {#if notes.length === 0}
          <p class="muted pad">No note: a note is what a shielded payment leaves the account.</p>
        {:else}
          <table class="notes" use:stacked aria-label="Notes">
            <thead><tr><th class="end">Amount</th><th>Kind</th><th>Memo</th><th class="end">Confirmations</th><th>Status</th><th>Transaction</th></tr></thead>
            <tbody>
              {#each notes as n (n.txid + ':' + n.action)}
                <tr class:spent={!!n.spent_by}>
                  <td class="end num"><Amount sats={coinsToSats(n.amount)} /></td>
                  <td>{noteKind(n)}</td>
                  <td><SafeText class="zs-memo" text={n.memo} /></td>
                  <td class="end num">{n.confirmations}</td>
                  <td>{#if n.spent_by}<span class="muted">{t('spent')}</span>{:else if n.watchonly}<span class="muted">{t('cannot be spent')}</span>{:else}<Privacy kind="private" />{/if}</td>
                  <td class="mono"><SafeText text={shortHash(n.txid, 8)} title={`${n.txid}:${n.action}`} /> <CopyButton text={n.txid} label="Copy the transaction id" /></td>
                </tr>
              {/each}
            </tbody>
          </table>
        {/if}
      </section>
    {/if}
  {/if}
</div>

<style>
  .zhistory {
    display: grid;
    gap: 1rem;
    align-content: start;
  }
  .bar {
    display: flex;
    flex-wrap: wrap;
    gap: 0.6rem;
    align-items: center;
  }
  .seg {
    display: inline-flex;
    padding: 0.18rem;
    border: 1px solid var(--line);
    border-radius: 999px;
    background: var(--surface);
  }
  .seg button {
    padding: 0.3rem 0.8rem;
    border: 0;
    border-radius: 999px;
    background: none;
    color: var(--text-muted);
    font: inherit;
    font-size: 0.86rem;
    cursor: pointer;
  }
  .seg button.on {
    background: var(--raised);
    color: var(--text);
    font-weight: 600;
  }
  .pad {
    padding: 1rem;
    margin: 0;
  }
  .ico {
    display: inline-grid;
    place-items: center;
    width: 2rem;
    height: 2rem;
    border-radius: 50%;
    background: color-mix(in srgb, var(--brand, var(--accent)) 14%, transparent);
    color: var(--brand, var(--accent));
  }
  .ico.shield,
  .ico.unshield {
    background: color-mix(in srgb, var(--warning) 16%, transparent);
    color: var(--warning);
  }
  .conf {
    color: var(--text-muted);
  }
  table.txs {
    width: 100%;
    border-collapse: collapse;
    font-size: 0.92rem;
  }
  table.txs th {
    padding: 0.55rem 0.6rem;
    color: var(--text-muted);
    font-weight: 600;
    font-size: 0.82rem;
    text-align: left;
    border-bottom: 1px solid var(--line);
  }
  table.txs td {
    padding: 0.45rem 0.6rem;
    border-bottom: 1px solid var(--line);
    vertical-align: middle;
  }
  table.txs tbody tr:not(.detail-row) {
    cursor: pointer;
  }
  table.txs tbody tr:not(.detail-row):hover,
  table.txs tr.open {
    background: var(--surface-2);
  }
  table.txs .ex {
    width: 2.2rem;
  }
  table.txs th.end {
    text-align: right;
  }
  .kind {
    display: inline-flex;
    align-items: center;
    gap: 0.45rem;
    white-space: nowrap;
  }
  .expand :global(svg) {
    transition: transform 0.15s;
  }
  .expand.on :global(svg) {
    transform: rotate(90deg);
  }
  .detail-row > td {
    background: var(--bg);
    cursor: default;
  }
  .points {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
  }
  .points select {
    width: auto;
  }
  .memos-count {
    display: inline-flex;
    align-items: center;
    gap: 0.25rem;
  }
  .conf.wait {
    color: var(--text);
    font-weight: 600;
  }
  .end {
    text-align: end;
  }
  .detail {
    display: grid;
    gap: 0.9rem;
    padding: 0.4rem 1rem 1rem 3.8rem;
  }
  .detail h3 {
    margin: 0 0 0.3rem;
    font-size: 0.86rem;
    color: var(--text-muted);
  }
  .memo {
    margin: 0 0 0.3rem;
    padding: 0.55rem 0.75rem;
    border-left: 3px solid var(--brand, var(--accent));
    background: var(--raised);
  }
  table {
    width: 100%;
    border-collapse: collapse;
  }
  th,
  td {
    padding: 0.5rem 0.75rem;
    border-bottom: 1px solid var(--line);
    text-align: start;
    vertical-align: top;
  }
  th {
    color: var(--text-muted);
    font-size: 0.82rem;
    font-weight: 600;
  }
  th.end,
  td.end {
    text-align: end;
  }
  tr.spent td {
    color: var(--text-muted);
  }
  .bump {
    display: flex;
    flex-wrap: wrap;
    gap: 0.6rem;
    align-items: flex-end;
  }
  @media (max-width: 760px) {
    .detail {
      padding-left: 1rem;
    }
  }
</style>
