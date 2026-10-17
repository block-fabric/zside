<script lang="ts">
  // Melt & cast (Paul Sztorc's design; the Melt & cast tab of zside-qt's Private page, redone). What goes into
  // the pool and what comes out are public and can be linked (13.47 in, 13.47 out an hour later): melting
  // shields each transparent coin alone, at random blocks; casting takes coins out only as standard bills, at
  // the blocks every wallet uses for that bill. The wallet keeps the plan and carries it out as the blocks
  // come; this page plans, shows each step and when it goes, and cancels what has not gone.
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { stacked } from '../../lib/actions';
  import { ask, confirm, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { shortHash } from '../../lib/format';
  import { t, tn } from '../../lib/i18n.svelte';
  import { nav, PAGES } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { node, prefs, toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import { coinsToSats, formatAmount, satsToCoins } from '../../lib/units';
  import { current, refreshInfo } from '../../lib/walletdata.svelte';
  import '../zs/zs.css';
  import Privacy from '../zs/Privacy.svelte';
  import { BILL_UNIT, billsOf, MAX_CAST, sortPlan, type PlanStep } from '../zs/model';
  import { refreshZs, startZs, zs } from '../zs/store.svelte';

  $effect(() => startZs());
  $effect(() => {
    void wallets.current;
    void refreshInfo();
  });

  const TABS = [
    { id: 'plan', label: 'The plan' },
    { id: 'melt', label: 'Melt' },
    { id: 'cast', label: 'Cast' },
  ];
  const tab = $derived(TABS.some((x) => x.id === nav.tab) ? nav.tab : 'plan');
  const wallet = $derived(wallets.current);
  const noWallet = $derived(!walletsEnabled() || wallet === null);
  const encrypted = $derived(!!current.info?.encrypted);
  const watchOnly = $derived(!!current.info?.privateKeysDisabled);
  const tip = $derived(node.tip?.height ?? 0);

  const plan = $derived(sortPlan(zs.plan));
  const toGo = $derived(plan.filter((s) => s.status !== 'sent'));
  const went = $derived(plan.filter((s) => s.status === 'sent'));
  const available = $derived(coinsToSats(zs.balance?.available ?? 0));
  const transparent = $derived(coinsToSats(zs.transparent?.trusted ?? 0));

  // --- Melt ---
  let spread = $state('36');
  const spreadValid = $derived(/^\d{1,5}$/.test(spread.trim()) && Number(spread) >= 1);
  let melting = $state(false);
  async function melt() {
    if (wallet === null || !spreadValid || melting) return;
    const yes = await confirm(
      t('Melt'),
      t('Shield every transparent coin of this wallet, each one alone, over about the next %1 blocks?', spread.trim()),
      { yes: t('Melt'), informative: t('The coins are locked until each one goes. Keep the wallet running meanwhile: it carries out the plan as the blocks come.') },
    );
    if (!yes) return;
    melting = true;
    try {
      const steps = await walletRpc<PlanStep[]>(wallet, 'meltcoins', [Number(spread.trim())]);
      if (steps === null) return;
      toast('success', t('Melt planned'), tn('%n coin(s) will be melted over the next blocks.', steps.length));
      nav.tab = 'plan';
      await refreshZs();
    } catch (e) {
      await inform(t('Melt'), errorText(e), 'error');
    } finally {
      melting = false;
    }
  }

  // --- Cast ---
  let castAmount = $state<bigint | null>(null);
  let castSpread = $state('1');
  const castSpreadValid = $derived(/^\d{1,4}$/.test(castSpread.trim()));
  const bills = $derived(castAmount !== null ? billsOf(castAmount) : null);
  const castProblem = $derived.by(() => {
    if (castAmount === null) return '';
    if (castAmount < BILL_UNIT) return t('Give an amount of %1 at least.', formatAmount(BILL_UNIT, prefs.unit));
    if (castAmount > MAX_CAST) return t('At most %1 at a time.', formatAmount(MAX_CAST, prefs.unit));
    if (castAmount > available) return t('More than the shielded balance in blocks (%1).', formatAmount(available, prefs.unit));
    return '';
  });
  let casting = $state(false);
  async function cast() {
    if (wallet === null || castAmount === null || castProblem || !castSpreadValid || casting) return;
    const r = await ask({
      title: t('Cast'),
      message: t('Take %1 out of the shielded balance as standard bills, each to a new address of this wallet, at the blocks set for each bill?', formatAmount(castAmount, prefs.unit)),
      code: (bills?.bills ?? []).map((b) => formatAmount(b, prefs.unit)).join(' + '),
      informative: t('The fees come out of the shielded balance too: the same fee for each bill in every wallet. A bill spends notes 3 blocks deep, so coins melted in the last blocks wait. Keep the wallet running meanwhile: it carries out the plan as the blocks come.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Cast', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    casting = true;
    try {
      const steps = await rpc.raw<PlanStep[]>('castcoins', [satsToCoins(castAmount), Number(castSpread.trim())], { wallet });
      toast('success', t('Cast planned'), tn('%n bill(s) will be cast, at their blocks.', steps.length));
      castAmount = null;
      nav.tab = 'plan';
      await refreshZs();
    } catch (e) {
      await inform(t('Cast'), errorText(e), 'error');
    } finally {
      casting = false;
    }
  }

  // --- Cancel ---
  async function cancel(step: PlanStep) {
    if (wallet === null) return;
    const yes = await confirm(t('Cancel the step'), step.kind === 'melt' ? t('Cancel this melt? The coin is unlocked.') : t('Cancel this bill?'), { yes: t('Cancel the step'), no: t('Keep it') });
    if (!yes) return;
    try {
      await rpc.raw<number>('cancelmeltcast', [String(step.id)], { wallet });
      toast('success', t('The step is cancelled.'));
      await refreshZs();
    } catch (e) {
      await inform(t('Melt & cast'), errorText(e), 'error');
    }
  }
  async function cancelAll() {
    if (wallet === null) return;
    const yes = await confirm(t('Cancel all'), t('Cancel every step of the plan that has not gone yet? Coins waiting to melt are unlocked.'), { yes: t('Cancel all'), no: t('Keep them'), danger: true });
    if (!yes) return;
    try {
      const n = await rpc.raw<number>('cancelmeltcast', ['all'], { wallet });
      toast('success', tn('%n step(s) cancelled.', n));
      await refreshZs();
    } catch (e) {
      await inform(t('Melt & cast'), errorText(e), 'error');
    }
  }

  function when(s: PlanStep): string {
    if (s.status === 'sent') return (s.confirmations ?? 0) > 0 ? tn('%n confirmation(s)', s.confirmations ?? 0) : t('waiting for a block');
    const h = s.height ?? 0;
    return h <= tip ? t('now') : tn('in %n block(s)', h - tip);
  }
</script>

<div class="page meltcast zs">
  <header class="page-head"><h1>{t(PAGES.meltcast.title)}</h1></header>

  {#if noWallet}
    <NoWallet page={PAGES.meltcast} />
  {:else}
    <section class="explain panel">
      <div class="lane">
        <div class="step public"><Icon name="coins" size={20} /><span>Transparent coins</span><Privacy kind="public" bare /></div>
        <div class="arrow"><strong>Melt</strong><span class="faint small">each coin alone, at random blocks</span></div>
        <div class="step private"><Icon name="pool" size={20} /><span>The shielded pool</span><Privacy kind="private" bare /></div>
        <div class="arrow"><strong>Cast</strong><span class="faint small">standard bills, at the blocks of each bill</span></div>
        <div class="step public"><Icon name="coins" size={20} /><span>New addresses of this wallet</span><Privacy kind="public" bare /></div>
      </div>
      <p class="muted small">What goes into the pool and what comes out are public, and can be linked: 13.47 in, 13.47 out an hour later. Melt and cast leave nothing to link: the coins go in one by one, and come out as bills that look like everyone else's.</p>
    </section>

    {#if encrypted}
      <div class="zs-note public">
        <Icon name="lock" size={18} />
        <span>This wallet is encrypted. The wallet carries out each step when its block comes, and needs its keys then: a step that finds the wallet locked waits, and says so below, until the wallet is unlocked. This app never leaves the wallet unlocked by itself.</span>
      </div>
    {/if}

    <TabRow label="Melt & cast" tabs={TABS} current={tab} select={(id) => (nav.tab = id)} />

    {#if tab === 'plan'}
      <section class="panel">
        <div class="panel-head">
          <h2>{tn('%n step(s) to go', toGo.length)}</h2>
          <button type="button" id="zs-cancel-all" class="btn ghost small" disabled={toGo.length === 0} onclick={cancelAll}><Icon name="x" size={15} /> Cancel all</button>
        </div>
        {#if plan.length === 0}
          <p class="muted pad">Nothing planned. Melt your transparent coins, or cast an amount as bills.</p>
        {:else}
          <table use:stacked aria-label="The plan">
            <thead><tr><th>Step</th><th class="end">Amount</th><th>Status</th><th>When</th><th>Transaction</th><th></th></tr></thead>
            <tbody>
              {#each [...toGo, ...went] as s (s.id)}
                <tr class:went={s.status === 'sent'}>
                  <td><span class="kind {s.kind}">{s.kind === 'melt' ? t('Melt') : t('Cast')}</span> <span class="faint small num">#{s.id}</span></td>
                  <td class="end num"><Amount sats={coinsToSats(s.amount)} /></td>
                  <td>{s.status === 'sent' ? ((s.confirmations ?? 0) > 0 ? t('done') : t('sent')) : s.status === 'waiting' ? t('waiting') : t('planned')}</td>
                  <td>
                    {when(s)}{#if s.status !== 'sent' && s.height !== undefined}<span class="faint small">{' · '}{t('block %1', String(s.height))}</span>{/if}
                    {#if s.error}<div class="small err"><SafeText text={s.error} /></div>{/if}
                    {#if s.coin}<div class="faint small mono"><SafeText text={shortHash(s.coin, 10)} title={s.coin} /></div>{/if}
                  </td>
                  <td class="mono">{#if s.txid}<SafeText text={shortHash(s.txid, 8)} title={s.txid} /> <CopyButton text={s.txid} label="Copy the transaction id" />{/if}</td>
                  <td class="end">{#if s.status !== 'sent'}<button type="button" class="btn ghost small" onclick={() => cancel(s)}>Cancel</button>{/if}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        {/if}
      </section>
    {:else if tab === 'melt'}
      <section class="panel">
        <div class="panel-head"><h2>Melt my transparent coins</h2></div>
        <div class="panel-body zs-form">
          <p>Every transparent coin of this wallet in a block goes into the shielded balance alone and whole, at a random block over the blocks you choose, one coin a block at most: no two of them go in one transaction or one block. Their timing can still hint, on a quiet chain, that they are one wallet's: a longer spread helps. The coins are locked until each goes.</p>
          <span class="muted small">{t('Transparent, in blocks')}: <Amount sats={transparent} /></span>
          <div class="field">
            <label for="zs-melt-spread">Over how many blocks</label>
            <input id="zs-melt-spread" type="text" inputmode="numeric" class="num" bind:value={spread} disabled={melting || watchOnly} />
            <span class="hint">36 by default: about the next half hour on a chain with a block a minute. The more, the harder to link.</span>
          </div>
          <div class="zs-actions">
            <button type="button" id="zs-melt" class="btn primary" disabled={!spreadValid || melting || watchOnly || transparent <= 0n} onclick={melt}><Icon name="meltcast" size={16} /> Melt my transparent coins</button>
          </div>
        </div>
      </section>
    {:else}
      <section class="panel">
        <div class="panel-head"><h2>Cast as bills</h2></div>
        <div class="panel-body zs-form">
          <p>Take coins out of the shielded balance only as standard bills: 0.01 times a power of two, each to a new transparent address of this wallet, at the blocks every wallet uses for that bill. What is below the smallest bill stays in the pool.</p>
          <span class="muted small">{t('Shielded, in blocks')}: <Amount sats={available} /></span>
          <AmountInput id="zs-cast-amount" label="Amount" bind:value={castAmount} max={available} disabled={casting || watchOnly} />
          {#if bills && !bills.tooMuch && bills.bills.length}
            <div>
              <span class="muted small">{tn('Comes out as %n bill(s)', bills.bills.length)}</span>
              <div class="zs-chips">{#each bills.bills as b, i (i)}<span class="zs-chip num"><Amount sats={b} /></span>{/each}</div>
              {#if bills.rest > 0n}<span class="faint small">{t('%1 stays shielded', formatAmount(bills.rest, prefs.unit))}</span>{/if}
            </div>
          {:else}
            <span class="faint small">{t('Bills: %1, %2, %3 … %4. Each goes at its own blocks, the same for every wallet.', formatAmount(BILL_UNIT, prefs.unit), formatAmount(BILL_UNIT * 2n, prefs.unit), formatAmount(BILL_UNIT * 4n, prefs.unit), formatAmount(BILL_UNIT << 20n, prefs.unit))}</span>
          {/if}
          <div class="field">
            <label for="zs-cast-spread">Windows to choose from</label>
            <input id="zs-cast-spread" type="text" inputmode="numeric" class="num" bind:value={castSpread} disabled={casting || watchOnly} />
            <span class="hint">Each bill goes at one of its next windows, chosen at random: 1 by default.</span>
          </div>
          {#if castProblem}<p class="error" role="alert">{castProblem}</p>{/if}
          <div class="zs-actions">
            <button type="button" id="zs-cast" class="btn primary" disabled={castAmount === null || !!castProblem || !castSpreadValid || casting || watchOnly} onclick={cast}><Icon name="meltcast" size={16} /> Cast</button>
          </div>
        </div>
      </section>
    {/if}
  {/if}
</div>

<style>
  .meltcast {
    display: grid;
    gap: 1rem;
    align-content: start;
  }
  .explain {
    display: grid;
    gap: 0.8rem;
    padding: 1rem 1.1rem;
  }
  .explain p {
    margin: 0;
  }
  .lane {
    display: grid;
    grid-template-columns: minmax(7rem, auto) minmax(6rem, 1fr) minmax(7rem, auto) minmax(6rem, 1fr) minmax(7rem, auto);
    align-items: center;
    gap: 0.6rem;
  }
  .step {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0.55rem 0.8rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m, 10px);
    background: var(--raised);
    font-weight: 600;
    line-height: 1.3;
  }
  .step.private {
    border-color: color-mix(in srgb, var(--brand, var(--accent)) 50%, var(--line));
  }
  .arrow {
    position: relative;
    display: flex;
    flex-direction: column;
    align-items: center;
    padding-bottom: 0.9rem;
    color: var(--text-muted);
    text-align: center;
  }
  .arrow strong {
    color: var(--text);
  }
  .arrow::after {
    content: '';
    position: absolute;
    left: 10%;
    right: 10%;
    bottom: 0.3rem;
    height: 2px;
    border-radius: 2px;
    background: linear-gradient(90deg, var(--line-strong), var(--brand, var(--accent)));
  }
  @media (max-width: 900px) {
    .lane {
      grid-template-columns: minmax(0, 1fr);
    }
  }
  .pad {
    padding: 1rem;
    margin: 0;
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
  .end {
    text-align: end;
  }
  tr.went td {
    color: var(--text-muted);
  }
  .kind {
    display: inline-block;
    padding: 0 0.45rem;
    border-radius: 999px;
    font-size: 0.8rem;
    font-weight: 650;
    border: 1px solid var(--line);
  }
  .kind.melt {
    border-color: color-mix(in srgb, var(--brand, var(--accent)) 55%, var(--line));
  }
  .kind.cast {
    border-color: color-mix(in srgb, var(--warning) 55%, var(--line));
  }
  .err {
    color: var(--text);
  }
  .zs-form p {
    margin: 0;
  }
</style>
