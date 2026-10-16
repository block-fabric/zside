<script lang="ts">
  // The sidechains of Chains (SidechainPage of chains-qt): the active sidechains and their escrows,
  // deposits to them, the proposals of new sidechains with their acks, and the withdrawal bundles
  // with the votes of the miners. Richer than Qt: an overview of the slots, the progress of each
  // proposal and bundle, and the deposits of a sidechain.
  import { t } from '../lib/i18n.svelte';
  import Pager from '../components/Pager.svelte';
  import SafeText from '../components/SafeText.svelte';
  import AmountInput from '../components/AmountInput.svelte';
  import Amount from '../components/Amount.svelte';
  import CopyButton from '../components/CopyButton.svelte';
  import Icon from '../components/Icon.svelte';
  import Meter from '../components/Meter.svelte';
  import { selectable } from '../lib/actions';
  import {
    bundleCanPass,
    loadDrivechain,
    proposalErrors,
    proposalNeeds,
    slotOverview,
    summaryText,
    voteText,
    type ActiveSidechain,
    type Bundle,
    type Deposit,
    type DrivechainState,
    type PendingProposal,
  } from '../lib/chain';
  import { ask, confirm, errorText, inform, walletRpc } from '../lib/dialogs.svelte';
  import { formatCount, formatTime, shortHash } from '../lib/format';
  import { detail, showDetail } from '../lib/detail.svelte';
  import { nav, takeFocus } from '../lib/nav.svelte';
  import SidechainNodes from './SidechainNodes.svelte';
  import MoveCoins from './MoveCoins.svelte';
  import Term from '../components/Term.svelte';
  import { simpleMode } from '../lib/uimode.svelte';
  import { host, rpc } from '../lib/rpc';
  import { node, noWalletText, prefs, toast, wallets } from '../lib/state.svelte';
  import { coinsToSats, formatAmount, satsToCoins, unitInfo } from '../lib/units';

  const TABS = [
    { id: 'sidechains', label: 'Sidechains' },
    { id: 'proposals', label: 'Proposals' },
    { id: 'withdrawals', label: 'Withdrawals' },
    // The nodes of sidechains last (user decision).
    { id: 'nodes', label: 'Nodes' },
  ];
  const tab = $derived(TABS.some((t) => t.id === nav.tab) ? nav.tab : 'sidechains');
  // Simple mode (lib/uimode.svelte.ts): the first tab moves coins (MoveCoins), the nodes are the apps of the
  // sidechains; proposals and withdrawals are Advanced's, shown only when asked for (the palette, a link).
  const simple = $derived(simpleMode());
  const SIMPLE_LABELS: Record<string, string> = { sidechains: 'Move coins', nodes: 'Sidechain apps' };
  const tabs = $derived(simple ? TABS.filter((tb) => tb.id === 'sidechains' || tb.id === 'nodes' || tb.id === tab).map((tb) => ({ ...tb, label: SIMPLE_LABELS[tb.id] ?? tb.label })) : TABS);

  let dc = $state<DrivechainState | null>(null);
  let loadError = $state('');

  // Only the refresh started last is shown: one a new block started before a vote, answered after the
  // vote's own, would show the old vote again (user report: the vote did not change at once).
  let refreshes = 0;
  async function refresh() {
    const mine = ++refreshes;
    try {
      const loaded = await loadDrivechain();
      if (mine !== refreshes) return;
      dc = loaded;
      loadError = '';
    } catch (e) {
      if (mine === refreshes) loadError = errorText(e);
    }
  }
  // Again with every block (SidechainPage: numBlocksChanged while it shows).
  $effect(() => {
    void node.tip?.height;
    refresh();
  });

  const escrowTotal = $derived(dc ? coinsToSats(dc.info.escrowtotal) : 0n);
  const slots = $derived(dc ? slotOverview(dc) : []);
  const usedSlots = $derived(slots.filter((s) => s.state !== 'free'));

  // --- Sidechains and deposits ---
  // The sidechain the page was opened for (the Deposit and Withdrawals of the dashboard, the detail panel).
  const focused = takeFocus('sidechains');
  let focusSlot: number | null = focused === null ? null : Number(focused);
  let selectedSlot = $state<number | null>(focusSlot);
  /** A sidechain chosen in the page: in the workspace layout, its details in the panel too. */
  function pick(slot: number) {
    selectedSlot = slot;
    if (detail.follow) showDetail({ kind: 'sidechain', slot });
  }
  const selected = $derived(dc?.sidechains.find((s) => s.slot === selectedSlot) ?? null);
  let deposits = $state.raw<Deposit[]>([]);
  // Pages (user decisions): the active sidechains 10 at a time; of the latest deposits, how many
  // (10 to 500, never more), shown 10, 25 or 50 a page.
  const SIDECHAINS_PAGE = 10;
  const DEPOSIT_POINTS = [10, 25, 50, 100, 250, 500] as const;
  const DEPOSIT_SIZES = [10, 25, 50] as const;
  let depositPoints = $state<number>(100);
  let sidechainsPage = $state(0);
  let depositsPage = $state(0);
  let depositsSize = $state(10);
  const sidechainsShown = $derived(dc ? dc.sidechains.slice(sidechainsPage * SIDECHAINS_PAGE, (sidechainsPage + 1) * SIDECHAINS_PAGE) : []);
  const depositsKept = $derived(deposits.slice(0, depositPoints));
  const depositsShown = $derived(depositsKept.slice(depositsPage * depositsSize, (depositsPage + 1) * depositsSize));
  // Another sidechain: its deposits from the first page.
  $effect(() => {
    void selectedSlot;
    depositsPage = 0;
  });
  $effect(() => {
    const slot = selectedSlot;
    void node.tip?.height;
    if (slot === null || !dc?.sidechains.some((s) => s.slot === slot)) {
      deposits = [];
      return;
    }
    rpc
      .raw<Deposit[]>('listsidechaindeposits', [slot])
      .then((d) => (deposits = d.slice(-500).reverse()))
      .catch(() => (deposits = []));
  });
  $effect(() => {
    // The first active sidechain is selected, as a deposit needs one.
    if (selectedSlot === null && dc && dc.sidechains.length) selectedSlot = dc.sidechains[0].slot;
  });

  let destination = $state('');
  let amount = $state<bigint | null>(null);
  let amountKey = $state(0);
  let destCheck = $state<{ kind: 'plain' | 'valid' | 'invalid'; slot?: number } | null>(null);
  let depositing = $state(false);

  // A deposit address says which sidechain it is for: select it, or say what is wrong with it.
  $effect(() => {
    const text = destination.trim();
    if (!text) {
      destCheck = null;
      return;
    }
    const t = setTimeout(async () => {
      try {
        const r = await host.depositAddress(text);
        destCheck = r;
        if (r.kind === 'valid' && dc?.sidechains.some((s) => s.slot === r.slot)) selectedSlot = r.slot ?? selectedSlot;
      } catch {
        destCheck = null;
      }
    }, 200);
    return () => clearTimeout(t);
  });
  const destMessage = $derived.by(() => {
    if (!destCheck) return '';
    if (destCheck.kind === 'invalid') return 'This deposit address has a typing error in it.';
    if (destCheck.kind === 'valid' && dc && !dc.sidechains.some((s) => s.slot === destCheck!.slot)) return `This deposit address is for slot ${destCheck.slot}, which has no active sidechain.`;
    if (destCheck.kind === 'valid') return `A deposit address for slot ${destCheck.slot}.`;
    return 'Not a deposit address: it cannot be checked.';
  });

  async function deposit() {
    const wallet = wallets.current;
    if (!selected || depositing) return;
    if (wallet === null) {
      await inform(t('Sidechains'), noWalletText(t('the deposit is paid from it.')));
      return;
    }
    const dest = destination.trim();
    if (!dest) {
      await inform(t('Sidechains'), t('Enter the deposit address that the wallet of the sidechain gives.'), 'warning');
      return;
    }
    let check;
    try {
      check = await host.depositAddress(dest);
    } catch (e) {
      await inform(t('Sidechains'), errorText(e), 'error');
      return;
    }
    if (check.kind === 'invalid') {
      await inform(t('Sidechains'), t('This deposit address has a typing error in it. Copy it again from the wallet of the sidechain.'), 'warning');
      return;
    }
    if (check.kind === 'valid' && check.slot !== selected.slot) {
      await inform(t('Sidechains'), t('This deposit address is for the sidechain in slot %1, and the sidechain selected is the one in slot %2.', check.slot, selected.slot), 'warning');
      return;
    }
    // Without the checks of a deposit address, a destination the sidechain does not understand is lost there.
    if (
      check.kind === 'plain' &&
      !(await confirm(t('Sidechains'), t('"%1" is not a deposit address (s<slot>_<address>_<checksum>), so it cannot be checked. If the sidechain does not understand it, the coins are lost. Deposit anyway?', dest), { yes: 'Deposit anyway', level: 'warning' }))
    )
      return;
    if (amount === null || amount <= 0n) {
      await inform(t('Sidechains'), t('Enter the amount to deposit.'), 'warning');
      return;
    }
    const sats = amount;
    const r = await ask({
      title: t('Confirm sidechain deposit'),
      message: t('Deposit %1 to the sidechain "%2" (slot %3), to be credited there to:', formatAmount(sats, prefs.unit, { withUnit: true }), selected.title, selected.slot),
      code: dest,
      informative: t('Coins deposited to a sidechain can only come back through a withdrawal approved by the miners.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Deposit', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    depositing = true;
    try {
      const result = await walletRpc<{ txid: string }>(wallet, 'createsidechaindeposit', [selected.slot, dest, satsToCoins(sats)]);
      if (!result) return;
      await inform(t('Sidechains'), t('Deposit sent in transaction %1.', result.txid), 'info');
      destination = '';
      amount = null;
      amountKey++;
      refresh();
    } catch (e) {
      await inform(t('Sidechains'), errorText(e), 'error');
    } finally {
      depositing = false;
    }
  }

  // --- Proposals ---
  let selectedProposal = $state<string | null>(null);
  let selectedQueued = $state<string | null>(null);
  let form = $state({ slot: 0, title: '', description: '', hash1: '', hash2: '' });
  let formErrors = $state<string[]>([]);

  async function call(method: string, params: unknown[]) {
    try {
      await rpc.raw(method, params);
      await refresh();
      return true;
    } catch (e) {
      await inform(t('Sidechains'), errorText(e), 'warning');
      return false;
    }
  }

  async function setAck(p: PendingProposal, ack: boolean) {
    await call('acksidechain', [p.proposalhash, ack]);
  }

  async function propose() {
    if (!dc) return;
    formErrors = proposalErrors(form, dc.info.maxsidechains);
    if (formErrors.length) return;
    const ok = await call('createsidechainproposal', [form.slot, form.title.trim(), form.description.trim(), form.hash1.trim() || '0'.repeat(64), form.hash2.trim() || '0'.repeat(40)]);
    if (ok) {
      toast('success', t('Proposal queued'), t('It goes into the next block this node mines.'));
      form = { slot: form.slot, title: '', description: '', hash1: '', hash2: '' };
    }
  }

  // --- Withdrawals ---
  let selectedBundle = $state<string | null>(null);
  // The bundles opened to show who they pay (user decision), and the page of payees of each.
  const PAYEES_PAGE = 25;
  let expanded = $state(new Set<string>());
  let payeePages = $state<Record<string, number>>({});
  function toggle(hash: string) {
    const next = new Set(expanded);
    if (!next.delete(hash)) next.add(hash);
    expanded = next;
  }
  const payeePage = (hash: string) => payeePages[hash] ?? 0;
  function setPayeePage(hash: string, page: number) {
    payeePages = { ...payeePages, [hash]: page };
  }
  function payeesShown(b: Bundle) {
    const page = payeePage(b.hash);
    return (b.payouts ?? []).slice(page * PAYEES_PAGE, (page + 1) * PAYEES_PAGE);
  }
  // Opened for a sidechain: its first bundle chosen on the Withdrawals tab, the deposit form ready on the first.
  $effect(() => {
    if (focusSlot === null || !dc) return;
    const slot = focusSlot;
    focusSlot = null;
    if (tab === 'withdrawals') selectedBundle = dc.bundles.find((b) => b.slot === slot)?.hash ?? selectedBundle;
    else if (tab === 'sidechains') queueMicrotask(() => document.getElementById('deposit-destination')?.focus());
  });
  async function vote(b: Bundle, v: 'upvote' | 'downvote' | 'abstain' | 'default') {
    const params = v === 'upvote' ? [b.slot, v, b.hash] : [b.slot, v];
    try {
      await rpc.raw('setwithdrawalvote', params);
    } catch (e) {
      await inform(t('Sidechains'), errorText(e), 'warning');
      return;
    }
    // The new vote at once, as the node counts it (listwithdrawalbundles): upvoting a bundle downvotes the
    // others of its sidechain. The default depends on the node: the refresh says.
    if (dc && v !== 'default') {
      const bundles = dc.bundles.map((x) => (x.slot !== b.slot ? x : { ...x, vote: v === 'upvote' ? (x.hash === b.hash ? 'upvote' : 'downvote') : v }));
      dc = { ...dc, bundles };
    }
    await refresh();
  }
  async function defaultVote(v: string) {
    await call('setdefaultwithdrawalvote', [v]);
  }

  function sidechainTitle(slot: number): string {
    return dc?.sidechains.find((s) => s.slot === slot)?.title ?? `Slot ${slot}`;
  }
  function escrowOf(s: ActiveSidechain): bigint | null {
    return s.escrow ? coinsToSats(s.escrow.amount) : null;
  }
</script>

<div class="page sidechains">
  <header class="page-head"><h1>Sidechains</h1></header>
  {#if simple}
    <p class="summary muted">Move CHN to a <Term id="sidechain">sidechain</Term> to use what it adds (markets, names, private payments…), and bring it back when you are done.</p>
  {:else if dc}
    <p class="summary muted">{summaryText(dc.info, formatAmount(escrowTotal, prefs.unit, { withUnit: true }))}</p>
  {:else}
    <p class="summary muted">{loadError || 'The drivechain state is not available yet.'}</p>
  {/if}
  <div class="tabs" role="tablist" aria-label="Sidechains">
    {#each tabs as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>
        {t(tb.label)}
        {#if tb.id === 'proposals' && dc && dc.proposals.pending.length + dc.proposals.queued.length}<span class="badge accent">{dc.proposals.pending.length + dc.proposals.queued.length}</span>{/if}
        {#if tb.id === 'withdrawals' && dc && dc.bundles.length}<span class="badge accent">{dc.bundles.length}</span>{/if}
      </button>
    {/each}
  </div>

  {#if tab === 'nodes'}
    <!-- The nodes of sidechains run from here (chains-qt's Sidechain Nodes window), as a tab of this page. -->
    <SidechainNodes embedded />
  {:else if simple && tab === 'sidechains'}
    <MoveCoins {dc} {refresh} />
  {:else if dc && tab === 'sidechains'}
    <section class="stats">
      <div class="stat panel"><span class="muted">Active sidechains</span><strong class="num">{dc.info.activesidechains}</strong><span class="faint">of {dc.info.maxsidechains} slots</span></div>
      <div class="stat panel"><span class="muted">In escrow</span><strong><Amount sats={escrowTotal} /></strong><span class="faint">held for the sidechains</span></div>
      <div class="stat panel"><span class="muted">Proposals</span><strong class="num">{dc.proposals.pending.length}</strong><span class="faint">{dc.proposals.queued.length} queued here</span></div>
      <div class="stat panel"><span class="muted">Pending withdrawals</span><strong class="num">{dc.info.pendingbundles}</strong><span class="faint">bundles being voted on</span></div>
    </section>

    <section class="panel slots-panel">
      <div class="panel-head"><h2>Slots</h2><span class="legend"><span class="key active"></span>active <span class="key proposed"></span>proposed <span class="key queued"></span>queued here <span class="key free"></span>free</span></div>
      <div class="panel-body slots" role="list" aria-label="Sidechain slots">
        {#each slots as s (s.slot)}
          <button
            type="button"
            
            class="slot {s.state}"
            class:chosen={s.slot === selectedSlot}
            class:replacing={s.replacing}
            title={s.state === 'free' ? `Slot ${s.slot}: free` : `Slot ${s.slot}: ${s.title} (${s.state}${s.replacing ? ', a replacement is proposed' : ''})`}
            aria-label={s.state === 'free' ? `Slot ${s.slot}, free` : `Slot ${s.slot}, ${s.title}, ${s.state}`}
            onclick={() => {
              if (s.state === 'active') selectedSlot = s.slot;
              else if (s.state === 'proposed' || s.state === 'queued') nav.tab = 'proposals';
              else form.slot = s.slot;
            }}
          ></button>
        {/each}
      </div>
      {#if usedSlots.length === 0}<p class="panel-body muted">No slot is in use yet. Propose a sidechain on the Proposals tab.</p>{/if}
    </section>

    <div class="cols">
      <section class="panel list">
        <div class="panel-head"><h2>Active sidechains</h2></div>
        {#if dc.sidechains.length === 0}
          <p class="panel-body muted">No sidechain is active on this chain yet.</p>
        {:else}
          <table class="grid" aria-label="Active sidechains">
            <thead><tr><th>Slot</th><th>Title</th><th class="end">In escrow</th><th class="end">Pending withdrawals</th><th class="end">Active since block</th></tr></thead>
            <tbody>
              {#each sidechainsShown as s (s.slot)}
                <tr class:chosen={s.slot === selectedSlot} onclick={() => pick(s.slot)} use:selectable={() => pick(s.slot)}>
                  <td class="num">{s.slot}</td>
                  <td><button type="button" class="link" onclick={() => pick(s.slot)} translate="no">{s.title}</button><span class="desc muted" translate="no" title={s.description}>{s.description}</span></td>
                  <td class="end">{#if s.escrow}<Amount sats={escrowOf(s)} />{:else}<span class="muted">no deposits yet</span>{/if}</td>
                  <td class="end num">{s.pendingbundles}</td>
                  <td class="end num">{formatCount(s.activationheight)}</td>
                </tr>
              {/each}
            </tbody>
          </table>
          <Pager total={dc.sidechains.length} bind:page={sidechainsPage} size={SIDECHAINS_PAGE} label="active sidechains" />
        {/if}
      </section>

      <section class="panel deposit">
        <div class="panel-head"><h2>Deposit to the selected sidechain</h2></div>
        <form class="panel-body form" onsubmit={(e) => { e.preventDefault(); deposit(); }}>
          <!-- Every active sidechain, to choose the one to deposit to (user decision); the list selects it too. -->
          <div class="field">
            <label for="deposit-sidechain">Sidechain</label>
            <select id="deposit-sidechain" value={selectedSlot ?? ''} onchange={(e) => pick(Number((e.currentTarget as HTMLSelectElement).value))}>
              {#if selectedSlot === null}<option value="" disabled>Select a sidechain</option>{/if}
              {#each dc.sidechains as s (s.slot)}
                <option value={s.slot} translate="no">{s.slot} · {s.title}</option>
              {/each}
            </select>
          </div>
          <div class="field">
            <label for="deposit-destination">Destination</label>
            <input id="deposit-destination" type="text" class="mono" maxlength="100" spellcheck="false" autocomplete="off" placeholder="The deposit address from the wallet of the sidechain: s<slot>_<address>_<checksum>" bind:value={destination} aria-invalid={destCheck?.kind === 'invalid' ? 'true' : undefined} aria-describedby="deposit-destination-check" />
            <span id="deposit-destination-check" class:error={destCheck?.kind === 'invalid'} class="hint">{destMessage}</span>
          </div>
          {#key amountKey}<AmountInput id="deposit-amount" bind:value={amount} label="Amount" />{/key}
          <div class="row">
            <button type="submit" class="btn primary" disabled={!selected || depositing || wallets.current === null}>{depositing ? 'Depositing…' : 'Deposit'}</button>
            {#if wallets.current === null}<span class="muted">Open a wallet to deposit from it.</span>{/if}
          </div>
        </form>
      </section>
    </div>

    {#if selected}
      <section class="panel detail">
        <div class="panel-head">
          <h2><SafeText text={selected.title} /> <span class="muted">slot {selected.slot}</span></h2>
          <span class="badge accent">version {selected.version}</span>
        </div>
        <div class="panel-body detail-grid">
          <dl class="facts">
            <dt>Description</dt><dd>{#if selected.description}<SafeText text={selected.description} />{:else}–{/if}</dd>
            <dt>Active since</dt><dd class="num">block {formatCount(selected.activationheight)}</dd>
            <dt>Escrow</dt><dd>{#if selected.escrow}<Amount sats={escrowOf(selected)} /> <span class="mono faint">{shortHash(selected.escrow.txid)}:{selected.escrow.n}</span>{:else}no deposits yet{/if}</dd>
            <dt>Escrow script</dt><dd class="mono">{selected.escrowscript}</dd>
            <dt>Proposal hash</dt><dd class="mono break">{selected.proposalhash} <CopyButton text={selected.proposalhash} label="Copy the proposal hash" /></dd>
            <dt>Release tarball hash</dt><dd class="mono break">{selected.hashid1}</dd>
            <dt>Build commit hash</dt><dd class="mono break">{selected.hashid2}</dd>
          </dl>
          <div>
            <div class="deposits-head">
              <h3>Latest deposits</h3>
              <label class="points">
                <span class="faint">Latest</span>
                <select aria-label="How many of the latest deposits" value={depositPoints} onchange={(e) => { depositPoints = Number((e.currentTarget as HTMLSelectElement).value); depositsPage = 0; }}>
                  {#each DEPOSIT_POINTS as n (n)}<option value={n}>{n}</option>{/each}
                </select>
              </label>
            </div>
            {#if deposits.length === 0}
              <p class="muted">No deposits yet.</p>
            {:else}
              <div class="scroll-x">
              <table class="grid compact" aria-label="Deposits">
                <thead><tr><th>Destination</th><th class="end">Amount</th><th class="end">Escrow after</th><th class="end">Confirmations</th></tr></thead>
                <tbody>
                  {#each depositsShown as d (d.txid + d.burnindex)}
                    <tr title={d.txid}>
                      <!-- Shortened in the middle (the slot and the checksum show); all of it on hover and copied. -->
                      <td class="mono dest" title={d.destination}><span translate="no">{shortHash(d.destination, 10)}</span> <CopyButton text={d.destination} label="Copy the destination" /></td>
                      <td class="end"><Amount sats={coinsToSats(d.amount)} /></td>
                      <td class="end"><Amount sats={coinsToSats(d.total)} /></td>
                      <td class="end num">{d.confirmations}</td>
                    </tr>
                  {/each}
                </tbody>
              </table>
              </div>
              <Pager total={depositsKept.length} bind:page={depositsPage} bind:size={depositsSize} sizes={DEPOSIT_SIZES} label="deposits" />
            {/if}
          </div>
        </div>
      </section>
    {/if}
  {:else if dc && tab === 'proposals'}
    <p class="muted">Proposals in the chain. A proposal activates if enough blocks ack it; blocks this node mines ack the proposals marked below.</p>
    <section class="panel list">
      <div class="panel-head"><h2>Proposals in the chain</h2></div>
      {#if dc.proposals.pending.length === 0}
        <p class="panel-body muted">No proposal is pending.</p>
      {:else}
        <table class="grid" aria-label="Proposals">
          <thead><tr><th>Slot</th><th>Title</th><th class="end">Age</th><th>Acks</th><th class="end">Failures</th><th class="end">Blocks left</th><th>Replaces a sidechain</th><th>Acked by this node</th><th>Proposal hash</th></tr></thead>
          <tbody>
            {#each dc.proposals.pending as p (p.proposalhash)}
              <tr class:chosen={p.proposalhash === selectedProposal} onclick={() => (selectedProposal = p.proposalhash)} use:selectable={() => (selectedProposal = p.proposalhash)}>
                <td class="num">{p.slot}</td>
                <td translate="no">{p.title}{#if p.description}<span class="desc muted">{p.description}</span>{/if}</td>
                <td class="end num">{p.age}</td>
                <td><Meter value={p.acks} max={proposalNeeds(p, dc.info)} label="Acks of {p.title}" text="{p.acks} / {proposalNeeds(p, dc.info)}" tone={p.ack ? 'positive' : 'accent'} /></td>
                <td class="end num" class:bad={p.failures > 0}>{p.failures} / {dc.info.activationmaxfailures}</td>
                <td class="end num">{p.blocksleft}</td>
                <td>{p.replacement ? 'yes' : 'no'}</td>
                <td>{#if p.ack}<span class="yes"><Icon name="check" size={14} /> yes</span>{:else}<span class="no">no</span>{/if}</td>
                <td class="mono">{shortHash(p.proposalhash)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      {/if}
      <div class="panel-body row">
        {#each [dc.proposals.pending.find((x) => x.proposalhash === selectedProposal)] as chosen (chosen?.proposalhash ?? '-')}
          <button type="button" class="btn" disabled={!chosen} onclick={() => chosen && setAck(chosen, true)}>Ack</button>
          <button type="button" class="btn" disabled={!chosen} onclick={() => chosen && setAck(chosen, false)}>Stop acking</button>
          {#if chosen}<CopyButton text={chosen.proposalhash} label="Copy the proposal hash" />{/if}
        {/each}
      </div>
    </section>

    <section class="panel list">
      <div class="panel-head"><h2>Proposals this node will make in the next block it mines</h2></div>
      {#if dc.proposals.queued.length === 0}
        <p class="panel-body muted">None.</p>
      {:else}
        <table class="grid" aria-label="Queued proposals">
          <thead><tr><th>Slot</th><th>Title</th><th>Description</th><th>Proposal hash</th></tr></thead>
          <tbody>
            {#each dc.proposals.queued as q (q.proposalhash)}
              <tr class:chosen={q.proposalhash === selectedQueued} onclick={() => (selectedQueued = q.proposalhash)} use:selectable={() => (selectedQueued = q.proposalhash)}>
                <td class="num">{q.slot}</td><td translate="no">{q.title}</td><td class="muted" translate="no">{q.description}</td><td class="mono">{shortHash(q.proposalhash)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      {/if}
      <div class="panel-body row">
        <button type="button" class="btn" disabled={!selectedQueued} onclick={async () => { if (selectedQueued && (await call('removesidechainproposal', [selectedQueued]))) selectedQueued = null; }}>Remove</button>
      </div>
    </section>

    <section class="panel">
      <div class="panel-head"><h2>Propose a sidechain</h2></div>
      <form class="panel-body form propose" onsubmit={(e) => { e.preventDefault(); propose(); }}>
        <div class="field small">
          <label for="proposal-slot">Slot</label>
          <input id="proposal-slot" type="number" min="0" max={dc.info.maxsidechains - 1} bind:value={form.slot} />
        </div>
        <div class="field">
          <label for="proposal-title">Title</label>
          <input id="proposal-title" type="text" maxlength="255" bind:value={form.title} />
        </div>
        <div class="field wide">
          <label for="proposal-description">Description</label>
          <input id="proposal-description" type="text" maxlength="1024" bind:value={form.description} />
        </div>
        <div class="field wide">
          <label for="proposal-hash1">Release tarball hash</label>
          <input id="proposal-hash1" type="text" class="mono" maxlength="64" placeholder="64 hexadecimal characters (optional)" bind:value={form.hash1} />
        </div>
        <div class="field wide">
          <label for="proposal-hash2">Build commit hash</label>
          <input id="proposal-hash2" type="text" class="mono" maxlength="40" placeholder="40 hexadecimal characters (optional)" bind:value={form.hash2} />
        </div>
        {#if formErrors.length}<ul class="errors wide" role="alert">{#each formErrors as err (err)}<li>{err}</li>{/each}</ul>{/if}
        <div class="row wide"><button type="submit" class="btn primary">Propose</button>
          {#if slots[form.slot] && slots[form.slot].state !== 'free'}<span class="warn">Slot {form.slot} holds <SafeText text={slots[form.slot].title} />: this proposal would replace it.</span>{/if}
        </div>
      </form>
    </section>
  {:else if dc && tab === 'withdrawals'}
    <p class="muted">Withdrawal bundles that miners are voting on. A bundle is paid out once its score reaches the minimum; upvoting one bundle of a sidechain downvotes its others.</p>
    <section class="panel list">
      <div class="panel-head">
        <h2>Withdrawal bundles</h2>
        <label class="row default-vote">
          <span class="muted">Default vote of this node</span>
          <select value={dc.info.defaultwithdrawalvote} onchange={(e) => defaultVote((e.currentTarget as HTMLSelectElement).value)} aria-label="Default withdrawal vote">
            <option value="upvote">Upvote the bundle a sidechain node gave this node</option>
            <option value="follow">Follow the miners who check bundles</option>
            <option value="abstain">Abstain</option>
            <option value="downvote">Downvote</option>
          </select>
        </label>
      </div>
      {#if dc.bundles.length === 0}
        <p class="panel-body muted">No withdrawal is pending.</p>
      {:else}
        <table class="grid" aria-label="Withdrawal bundles">
          <thead><tr><th><span class="sr-only">Payees</span></th><th>Slot</th><th>Score</th><th class="end">Blocks left</th><th>Payable</th><th>Transaction known</th><th>Vote of this node</th><th>Bundle hash</th></tr></thead>
          <tbody>
            {#each dc.bundles as b (b.hash)}
              <tr class:chosen={b.hash === selectedBundle} onclick={() => (selectedBundle = b.hash)} use:selectable={() => (selectedBundle = b.hash)}>
                <td class="expand-cell">
                  <button type="button" class="btn small ghost icon expand" class:open={expanded.has(b.hash)} aria-expanded={expanded.has(b.hash)} aria-label="Who bundle {shortHash(b.hash)} pays" onclick={(e) => { e.stopPropagation(); toggle(b.hash); }}><Icon name="chevron-right" size={15} /></button>
                </td>
                <td><span class="num">{b.slot}</span> <SafeText class="muted" text={sidechainTitle(b.slot)} /></td>
                <td><Meter value={b.score} max={dc.info.withdrawalminscore} label="Score of bundle {shortHash(b.hash)}" text="{b.score} of {dc.info.withdrawalminscore}" tone={b.payable ? 'positive' : bundleCanPass(b, dc.info) ? 'accent' : 'negative'} /></td>
                <td class="end num">{b.blocksleft}</td>
                <td>{#if b.payable}<span class="yes"><Icon name="check" size={14} /> yes</span>{:else}no{/if}</td>
                <td>{b.known ? 'yes' : 'no'}</td>
                <td><span class="badge" class:accent={b.vote === 'upvote'} class:warning={b.vote === 'downvote'}>{voteText(b.vote)}</span></td>
                <td class="mono">{shortHash(b.hash)}</td>
              </tr>
              {#if expanded.has(b.hash)}
                <tr class="payouts"><td colspan="8">
                  {#if b.payouts}
                    <!-- Who the bundle pays and how much (user decision), as this node has its transaction. -->
                    <table class="grid compact payees" aria-label="Payees of bundle {shortHash(b.hash)}">
                      <thead><tr><th class="end">#</th><th>Pays to</th><th class="end">Amount</th></tr></thead>
                      <tbody>
                        {#each payeesShown(b) as p, i (i)}
                          <tr>
                            <td class="end num faint">{payeePage(b.hash) * PAYEES_PAGE + i + 1}</td>
                            <td class="mono">{#if p.address}<span translate="no">{p.address}</span> <CopyButton text={p.address} label="Copy the address" />{:else}<span class="faint">script</span> <span translate="no">{shortHash(p.script, 16)}</span>{/if}</td>
                            <td class="end"><Amount sats={coinsToSats(p.amount)} /></td>
                          </tr>
                        {/each}
                      </tbody>
                      <tfoot>
                        <tr><td></td><td class="faint">{b.payouts.length === 1 ? '1 payee' : `${formatCount(b.payouts.length)} payees`}, in all</td><td class="end"><strong><Amount sats={b.payouts.reduce((sum, p) => sum + coinsToSats(p.amount), 0n)} /></strong></td></tr>
                        {#if b.fee !== undefined}<tr><td></td><td class="faint">Fee to the miner who pays it out</td><td class="end"><Amount sats={coinsToSats(b.fee)} /></td></tr>{/if}
                      </tfoot>
                    </table>
                    <Pager total={b.payouts.length} page={payeePage(b.hash)} size={PAYEES_PAGE} label="payees" onpage={(n) => setPayeePage(b.hash, n)} />
                  {:else}
                    <span class="muted">This node does not have the transaction of this bundle, so it cannot say who it pays. A node of the sidechain hands it over.</span>
                  {/if}
                </td></tr>
              {/if}
            {/each}
          </tbody>
        </table>
      {/if}
      <div class="panel-body row">
        {#each [dc.bundles.find((x) => x.hash === selectedBundle)] as chosen (chosen?.hash ?? '-')}
          <button type="button" class="btn" disabled={!chosen} onclick={() => chosen && vote(chosen, 'upvote')}>Upvote</button>
          <button type="button" class="btn" disabled={!chosen} onclick={() => chosen && vote(chosen, 'downvote')}>Downvote sidechain</button>
          <button type="button" class="btn" disabled={!chosen} onclick={() => chosen && vote(chosen, 'abstain')}>Abstain</button>
          <button type="button" class="btn" disabled={!chosen} onclick={() => chosen && vote(chosen, 'default')}>Use default</button>
        {/each}
      </div>
    </section>
  {/if}
  <div class="row foot"><button type="button" class="btn ghost small" onclick={refresh}>Refresh</button><span class="faint">Unit: {unitInfo(prefs.unit).short}</span></div>
</div>

<style>
  .summary {
    margin-top: 0.4rem;
    max-width: 90ch;
  }
  .stats {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(13rem, 100%), 1fr));
    gap: 0.8rem;
    margin-bottom: 1rem;
  }
  .stat {
    display: flex;
    flex-direction: column;
    gap: 0.15rem;
    padding: 0.8rem 1rem;
  }
  .stat strong {
    font-size: 1.4rem;
  }
  .stat .faint {
    font-size: 0.82rem;
  }
  .slots-panel {
    margin-bottom: 1rem;
  }
  .legend {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
    font-size: 0.82rem;
    color: var(--text-muted);
  }
  .key {
    width: 0.75rem;
    height: 0.75rem;
    border-radius: 3px;
    margin-left: 0.5rem;
  }
  .slots {
    display: grid;
    grid-template-columns: repeat(auto-fill, 1.05rem);
    gap: 4px;
  }
  .slot {
    width: 1.05rem;
    height: 1.05rem;
    padding: 0;
    border: 0;
    border-radius: 3px;
    cursor: pointer;
  }
  .slot.free,
  .key.free {
    background: var(--raised);
  }
  .slot.active,
  .key.active {
    background: var(--positive);
  }
  .slot.proposed,
  .key.proposed {
    background: var(--warning);
  }
  .slot.queued,
  .key.queued {
    background: transparent;
    box-shadow: inset 0 0 0 2px var(--warning);
  }
  .slot.replacing {
    background: repeating-linear-gradient(45deg, var(--positive), var(--positive) 3px, var(--warning) 3px, var(--warning) 6px);
  }
  .slot.chosen {
    outline: 2px solid var(--text);
    outline-offset: 1px;
  }
  .cols {
    display: grid;
    grid-template-columns: minmax(0, 1.5fr) minmax(20rem, 1fr);
    gap: 1rem;
    margin-bottom: 1rem;
  }
  /* Wide tables scroll sideways; the box grows with its rows (10 a page), never a scroll bar of its own
     up and down (user decision). */
  .list {
    margin-bottom: 1rem;
    overflow-x: auto;
    overflow-y: hidden;
  }
  .cols .list {
    margin-bottom: 0;
  }
  table.grid {
    width: 100%;
    border-collapse: collapse;
    font-size: 0.92rem;
  }
  table.grid th {
    text-align: left;
    font-size: 0.82rem;
    font-weight: 600;
    color: var(--text-muted);
    padding: 0.45rem 0.8rem;
    border-bottom: 1px solid var(--line);
    white-space: nowrap;
  }
  table.grid td {
    padding: 0.5rem 0.8rem;
    border-bottom: 1px solid var(--line);
    vertical-align: middle;
  }
  table.grid tbody tr {
    cursor: pointer;
  }
  table.grid tbody tr:hover {
    background: var(--raised);
  }
  table.grid tr.chosen {
    background: var(--accent-soft);
  }
  table.grid .end {
    text-align: right;
  }
  table.compact td {
    padding: 0.3rem 0.5rem;
  }
  .desc {
    display: block;
    font-size: 0.82rem;
  }
  /* The description of an active sidechain: two lines at most (all of it on hover, and below the list). */
  .list td .desc {
    display: -webkit-box;
    -webkit-box-orient: vertical;
    -webkit-line-clamp: 2;
    line-clamp: 2;
    overflow: hidden;
    min-width: 12rem;
  }
  .dest {
    white-space: nowrap;
  }
  .deposits-head {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 1rem;
  }
  .points {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
    font-size: 0.86rem;
  }
  .points select {
    width: auto;
    min-height: 1.9rem;
  }
  .expand-cell {
    width: 2.2rem;
  }
  .expand :global(svg) {
    transition: transform 0.15s;
  }
  .expand.open :global(svg) {
    transform: rotate(90deg);
  }
  .payees {
    margin: 0.25rem 0;
  }
  .payees tfoot td {
    border-top: 1px solid var(--line);
    padding-top: 0.4rem;
  }
  /* A narrow window: the deposits scroll sideways in their column, not the page. */
  .scroll-x {
    overflow-x: auto;
    overflow-y: hidden;
  }
  .link {
    padding: 0;
    border: 0;
    background: none;
    color: var(--text);
    font: inherit;
    font-weight: 600;
    cursor: pointer;
  }
  .form {
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
  }
  .target {
    font-weight: 600;
  }
  .detail {
    margin-bottom: 1rem;
  }
  .detail h2 .muted {
    font-weight: 400;
    font-size: 0.95rem;
  }
  /* The facts of the sidechain from the top down, whatever the height of the deposits beside them (no
     spacing to fill the height: user decision). */
  .detail-grid {
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
    align-items: start;
    gap: 1.5rem;
  }
  .detail-grid h3 {
    margin-bottom: 0.5rem;
  }
  .propose {
    display: grid;
    grid-template-columns: 8rem minmax(0, 1fr);
    gap: 0.9rem 1rem;
  }
  .propose .wide {
    grid-column: 1 / -1;
  }
  .errors {
    margin: 0;
    color: var(--negative);
  }
  .warn {
    color: var(--warning);
    font-size: 0.9rem;
  }
  .yes {
    display: inline-flex;
    align-items: center;
    gap: 0.25rem;
    color: var(--positive);
    font-weight: 600;
  }
  .no {
    color: var(--text-muted);
  }
  .bad {
    color: var(--negative);
  }
  .payouts td {
    background: var(--bg);
    font-size: 0.88rem;
    cursor: default;
  }
  .default-vote select {
    width: auto;
    min-height: 2rem;
  }
  .foot {
    margin-top: 0.5rem;
  }
  @media (max-width: 1100px) {
    .cols,
    .detail-grid {
      grid-template-columns: minmax(0, 1fr);
    }
  }
</style>
