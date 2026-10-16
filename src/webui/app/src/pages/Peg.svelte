<script lang="ts">
  // The peg of a sidechain to its mainchain (the Mainchain page of the template's Qt GUI, redone):
  //  - Deposit: a deposit address of this wallet, to give the wallet of the mainchain; or the helper,
  //    which has the wallet of the mainchain node make the deposit; the deposits to this chain, credited
  //    or on their way.
  //  - Withdraw: to an address of the mainchain, for a fee to its miners; the withdrawals waiting for a
  //    bundle, the ones of this wallet taken back while they wait.
  //  - Bundle: the bundle of withdrawals the miners of the mainchain vote on, its votes and the blocks it
  //    has left; the bundles before it, paid or failed.
  import { t } from '../lib/i18n.svelte';
  import { stacked } from '../lib/actions';
  import SafeText from '../components/SafeText.svelte';
  import Amount from '../components/Amount.svelte';
  import AmountInput from '../components/AmountInput.svelte';
  import AddressInput from '../components/AddressInput.svelte';
  import CopyButton from '../components/CopyButton.svelte';
  import Icon from '../components/Icon.svelte';
  import Meter from '../components/Meter.svelte';
  import NoWallet from '../components/NoWallet.svelte';
  import QrCode from '../components/QrCode.svelte';
  import { CHAIN } from '../chain/config';
  import { ask, errorText, inform, walletRpc } from '../lib/dialogs.svelte';
  import { formatCount, formatTime, shortHash } from '../lib/format';
  import { nav, PAGES, takeFocus } from '../lib/nav.svelte';
  import { rpc } from '../lib/rpc';
  import { node, prefs, toast, wallets, walletsEnabled } from '../lib/state.svelte';
  import { coinsToSats, formatAmount, satsToCoins } from '../lib/units';
  import { bundleCanPass, bundleHistory, declined, depositsOf, depositTarget, ownWithdrawals, peg, voteProgress, withdrawalKey, type Withdrawal } from '../lib/side';
  import { refreshSide, side, startSide } from '../lib/sidestate.svelte';
  import Bmm from './Bmm.svelte';
  import Pager from '../components/Pager.svelte';
  import { simpleMode } from '../lib/uimode.svelte';

  const mainName = CHAIN.mainchain?.name ?? '';
  const TABS = [
    { id: 'deposit', label: 'Deposit' },
    { id: 'withdraw', label: 'Withdraw' },
    { id: 'bundle', label: 'Bundle' },
    // Merged mining is a tab of Drivechains (user decision, 2026-10-15); go('bmm') leads here.
    { id: 'bmm', label: 'Merged mining' },
  ];
  const tab = $derived(TABS.some((x) => x.id === nav.tab) ? nav.tab : 'deposit');
  // Simple mode (lib/uimode.svelte.ts): coins in and coins back, in plain words; the bundle and merged mining
  // are Advanced's, shown only when asked for (a link, the palette). The tables leave out blocks and ids.
  const simple = $derived(simpleMode());
  const SIMPLE_LABELS: Record<string, string> = { deposit: 'Bring coins in', withdraw: 'Send coins back' };
  const tabs = $derived(simple ? TABS.filter((tb) => tb.id === 'deposit' || tb.id === 'withdraw' || tb.id === tab).map((tb) => ({ ...tb, label: SIMPLE_LABELS[tb.id] ?? tb.label })) : TABS);

  $effect(() => {
    startSide();
    void refreshSide(true);
  });
  // Again with every block of this chain.
  $effect(() => {
    void node.tip?.height;
    void refreshSide(true);
  });

  const info = $derived(side.info);
  const connected = $derived(!!info?.connected);
  const slot = $derived(info?.slot ?? null);
  const wallet = $derived(wallets.current);
  const noWallet = $derived(!walletsEnabled() || wallet === null);

  // --- Deposits ---
  let depositAddress = $state('');
  let depositFor = $state('');
  let gettingAddress = $state(false);
  async function newDepositAddress() {
    if (wallet === null) return;
    gettingAddress = true;
    try {
      const r = await rpc.raw<{ depositaddress: string; address: string }>('getdepositaddress', [], { wallet });
      depositAddress = r.depositaddress;
      depositFor = r.address;
    } catch (e) {
      await inform(t('Drivechains'), errorText(e), 'warning');
    } finally {
      gettingAddress = false;
    }
  }

  // The helper: the wallet of the mainchain node deposits to the address above.
  let mainBalance = $state<bigint | null>(null);
  let mainBalanceError = $state('');
  let helperAmount = $state<bigint | null>(null);
  let helperKey = $state(0);
  let depositing = $state(false);
  async function loadMainBalance() {
    if (!connected) return;
    try {
      const b = await peg.call<{ mine: { trusted: number } }>('getbalances');
      mainBalance = coinsToSats(b.mine.trusted);
      mainBalanceError = '';
    } catch (e) {
      mainBalance = null;
      mainBalanceError = errorText(e);
    }
  }
  $effect(() => {
    void side.main.height;
    if (tab === 'deposit' && connected) void loadMainBalance();
  });
  async function depositFromMainchain() {
    if (!depositAddress) {
      await inform(t('Drivechains'), t('Get a deposit address first: the deposit is credited to it.'), 'warning');
      return;
    }
    if (helperAmount === null || helperAmount <= 0n) {
      await inform(t('Drivechains'), t('Enter the amount to deposit.'), 'warning');
      return;
    }
    // The host asks the user on the desktop, with the amount first: the wallet of the mainchain node pays.
    depositing = true;
    try {
      const done = await peg.deposit(depositAddress, satsToCoins(helperAmount));
      toast('success', t('Deposit sent'), t('In the transaction %1 of %2.', shortHash(done.txid, 12), mainName));
      helperAmount = null;
      helperKey++;
      void loadMainBalance();
      void refreshSide(true);
    } catch (e) {
      if (!declined(e)) await inform(t('Deposit from %1', mainName), errorText(e), 'error');
    } finally {
      depositing = false;
    }
  }

  const deposits = $derived(depositsOf(side.main.events, info?.tipheight ?? -1));
  const pendingDeposits = $derived(deposits.filter((d) => !d.credited));
  // The deposits as the other long lists (user decision, 2026-10-15): 25, 50 or 100 a page, and how many of
  // the latest are kept (0: all of the mainchain blocks on record).
  const DEPOSIT_SIZES = [25, 50, 100] as const;
  const DEPOSIT_POINTS = [25, 50, 100, 250, 500, 1000, 0] as const;
  let depositPage = $state(0);
  let depositSize = $state(25);
  let depositPoints = $state<number>(100);
  const depositsKept = $derived(depositPoints === 0 ? deposits : deposits.slice(0, depositPoints));
  const depositRows = $derived(depositsKept.slice(depositPage * depositSize, (depositPage + 1) * depositSize));
  /** The deposits to an address of this wallet. */
  let ownTargets = $state<Set<string>>(new Set());
  const addressMine = new Map<string, boolean>();
  $effect(() => {
    const list = deposits;
    const w = wallet;
    const s = slot;
    if (w === null || s === null) return;
    (async () => {
      const out = new Set<string>();
      for (const d of list.slice(0, 200)) {
        const target = depositTarget(d.destination, s);
        if (!target) continue;
        let is = addressMine.get(target);
        if (is === undefined) {
          is = await rpc
            .raw<{ ismine: boolean }>('getaddressinfo', [target], { wallet: w })
            .then((i) => !!i.ismine)
            .catch(() => false);
          addressMine.set(target, is);
        }
        if (is) out.add(`${d.txid}:${d.burnindex}`);
      }
      ownTargets = out;
    })();
  });

  // --- Withdrawals ---
  let mainAddress = $state('');
  let mainAddressValid = $state(false);
  let amount = $state<bigint | null>(null);
  let fee = $state<bigint | null>(10_000n);
  let formKey = $state(0);
  let withdrawing = $state(false);
  const total = $derived(amount !== null && fee !== null ? amount + fee : null);
  async function addressOfMainWallet() {
    try {
      mainAddress = await peg.call<string>('getnewaddress', ['withdrawal']);
    } catch (e) {
      await inform(t('Drivechains'), errorText(e), 'warning');
    }
  }
  async function withdraw() {
    if (wallet === null) return;
    if (!mainAddressValid) {
      await inform(t('Withdraw'), t('Enter an address of %1 to pay.', mainName), 'warning');
      return;
    }
    if (amount === null || amount <= 0n || fee === null || fee < 0n) {
      await inform(t('Withdraw'), t('Enter the amount and the fee for the miners of %1.', mainName), 'warning');
      return;
    }
    const r = await ask({
      title: t('Confirm the withdrawal'),
      message: t('Withdraw %1 to this address of %2, offering its miners %3:', formatAmount(amount, prefs.unit, { withUnit: true }), mainName, formatAmount(fee, prefs.unit, { withUnit: true })),
      code: mainAddress.trim(),
      informative: t('%1 leave this wallet now. %2 pays them once its miners have voted a bundle with the withdrawal through, which takes long; until the withdrawal is in a bundle, it can be taken back.', formatAmount(total ?? 0n, prefs.unit, { withUnit: true }), mainName),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Withdraw', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    withdrawing = true;
    try {
      const done = await walletRpc<{ txid: string; vout: number; refundaddress: string }>(wallet, 'createwithdrawal', [mainAddress.trim(), satsToCoins(amount), satsToCoins(fee)]);
      if (!done) return;
      toast('success', t('Withdrawal made'), t('In the transaction %1; it shows in the list once the transaction is in a block.', shortHash(done.txid, 12)));
      mainAddress = '';
      amount = null;
      formKey++;
      void refreshSide(true);
    } catch (e) {
      await inform(t('Withdraw'), errorText(e), 'error');
    } finally {
      withdrawing = false;
    }
  }

  // The withdrawals of this wallet: those whose refund address is one of it.
  let mine = $state<Set<string>>(new Set());
  const refundMine = new Map<string, boolean>();
  $effect(() => {
    const list = side.withdrawals;
    const w = wallet;
    if (w === null) {
      mine = new Set();
      return;
    }
    ownWithdrawals(list, w, refundMine).then((s) => (mine = s));
  });
  let onlyMine = $state(false);
  const shownWithdrawals = $derived(onlyMine ? side.withdrawals.filter((w) => mine.has(withdrawalKey(w))) : side.withdrawals);
  const waitingTotal = $derived(side.withdrawals.reduce((n, w) => n + coinsToSats(w.amount), 0n));
  let refunding = $state<string | null>(null);
  async function refund(w: Withdrawal) {
    if (wallet === null) return;
    const ok = await ask({
      title: t('Take the withdrawal back'),
      message: t('Take back the withdrawal of %1? The block that takes the request pays it, the fee for %2 included, to its refund address:', formatAmount(coinsToSats(w.amount), prefs.unit, { withUnit: true }), mainName),
      code: w.refundaddress,
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Take back', kind: 'primary' },
      ],
    });
    if (ok.choice !== 'yes') return;
    refunding = withdrawalKey(w);
    try {
      const done = await walletRpc<{ txid: string; amount: number }>(wallet, 'refundwithdrawal', [w.txid, w.vout]);
      if (!done) return;
      toast('success', t('Refund asked'), t('%1 come back with the block that takes the transaction %2.', formatAmount(coinsToSats(done.amount), prefs.unit, { withUnit: true }), shortHash(done.txid, 12)));
      void refreshSide(true);
    } catch (e) {
      await inform(t('Take the withdrawal back'), errorText(e), 'error');
    } finally {
      refunding = null;
    }
  }

  // --- The bundle ---
  const bundle = $derived(side.bundle);
  const minScore = $derived(side.main.drivechain?.withdrawalminscore ?? 0);
  const voted = $derived(bundle?.hash ? side.main.bundles.find((b) => b.hash === bundle.hash) : undefined);
  const history = $derived(bundleHistory(side.main.events));

  // Opened from the home (goTo('peg', 'bundle', hash)): that bundle's row.
  let focusBundle = $state<string | null>(takeFocus('peg'));

  function bundleTone(h: { closed?: number; paid?: boolean }): 'ok' | 'wait' | 'bad' {
    return h.closed === undefined ? 'wait' : h.paid ? 'ok' : 'bad';
  }
</script>

<div class="page peg">
  <header class="page-head"><h1>{t(PAGES.peg.title)}</h1></header>

  <section class="panel side-bridge" aria-label="The peg">
    <div class="side-chain">
      <span class="caps">Mainchain</span>
      <strong translate="no">{mainName}</strong>
      <span class="side-link" class:on={connected} class:off={info !== null && !connected}><span class="dot"></span>{info === null ? 'Asking the node…' : connected ? 'Connected' : 'Not reachable'}</span>
      {#if info}<span class="faint small">block {formatCount(Math.max(0, info.height))}{#if side.connection} · <span translate="no">{side.connection.node}</span>{/if}</span>{/if}
    </div>
    <div class="side-flows">
      <div class="side-flow"><span>Deposits</span><span class="line"></span><Icon name="send" size={16} /></div>
      <div class="side-flow up"><Icon name="receive" size={16} /><span class="line"></span><span>Withdrawals</span></div>
    </div>
    <div class="side-chain end">
      <span class="caps">This sidechain</span>
      <strong translate="no">{CHAIN.name}</strong>
      <span class="muted">{slot === null ? 'Slot –' : `Slot ${slot}`}</span>
      {#if info}<span class="faint small">acted on mainchain block {formatCount(Math.max(0, info.tipheight))}</span>{/if}
    </div>
  </section>

  {#if side.error}
    <p class="side-note side-bad" role="alert"><SafeText text={side.error} /></p>
  {:else if info && !connected}
    <p class="side-note side-warn" role="status">
      The node cannot reach the {mainName} node, so it cannot tell which new blocks are valid: <SafeText text={info.error ?? ''} />. Check the connection in Settings → {mainName}.
    </p>
  {/if}

  {#if !simple}
  <section class="side-stats">
    <div class="panel side-stat"><span class="muted">Deposits on their way</span><strong class="num">{pendingDeposits.length}</strong><span class="faint"><Amount sats={pendingDeposits.reduce((n, d) => n + d.sats, 0n)} /></span></div>
    <div class="panel side-stat"><span class="muted">Withdrawals waiting</span><strong class="num">{side.withdrawals.length}</strong><span class="faint"><Amount sats={waitingTotal} /></span></div>
    <div class="panel side-stat">
      <span class="muted">Bundle</span>
      <strong>{bundle?.status === 'pending' ? 'Being voted on' : bundle?.status === 'next' ? 'Next block' : 'None'}</strong>
      <span class="faint">{#if voted && minScore}{voted.score} of {minScore} votes{:else if bundle?.withdrawals}{bundle.withdrawals} withdrawal(s){:else}{bundle ? `${bundle.waiting} waiting` : '–'}{/if}</span>
    </div>
    <div class="panel side-stat"><span class="muted">Mainchain blocks on record</span><strong class="num">{info && info.height >= 0 ? formatCount(info.height) : '–'}</strong><span class="faint">{side.main.error ? 'not reachable' : 'followed by this node'}</span></div>
  </section>
  {/if}

  <div class="tabs" role="tablist" aria-label="Drivechains">
    {#each tabs as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>
        {t(tb.label)}
        {#if tb.id === 'deposit' && pendingDeposits.length}<span class="badge accent">{pendingDeposits.length}</span>{/if}
        {#if tb.id === 'withdraw' && side.withdrawals.length}<span class="badge accent">{side.withdrawals.length}</span>{/if}
        {#if tb.id === 'bundle' && bundle?.status === 'pending'}<span class="badge warning">1</span>{/if}
      </button>
    {/each}
  </div>

  {#if tab === 'bmm'}
    <Bmm embedded />
  {:else if tab === 'deposit'}
    <div class="side-cols">
      <section class="panel">
        <div class="panel-head"><h2>Deposit address</h2></div>
        <div class="panel-body side-form">
          <p class="side-note">
            Coins come to this chain from {mainName}. Give a deposit address to the wallet of {mainName} as the destination of a deposit (Sidechains, Deposit). It names this sidechain{slot === null ? '' : `, slot ${slot},`} and ends in a checksum, so the
            wallet of {mainName} refuses it if it is mistyped or used for another sidechain. The coins arrive with the next block of this chain after the deposit is mined on {mainName}.
          </p>
          {#if noWallet}
            <NoWallet page={PAGES.peg} />
          {:else}
            {#if depositAddress}
              <div class="side-code" id="deposit-address"><span class="mono" translate="no">{depositAddress}</span><CopyButton text={depositAddress} label="Copy the deposit address" /></div>
              <div class="qr-row">
                <QrCode text={depositAddress} size={168} label="QR code of the deposit address" />
                <p class="side-note">Credited to <span class="mono" translate="no">{depositFor}</span>, an address of this wallet labelled “deposit”.</p>
              </div>
            {/if}
            <div class="row">
              <button type="button" class="btn primary" id="deposit-new" disabled={gettingAddress} onclick={newDepositAddress}><Icon name="plus" size={15} /> New deposit address</button>
            </div>
          {/if}
        </div>
      </section>

      <section class="panel">
        <div class="panel-head"><h2>Deposit from the {mainName} wallet</h2></div>
        <form class="panel-body side-form" onsubmit={(e) => { e.preventDefault(); depositFromMainchain(); }}>
          <p class="side-note">The wallet of the {mainName} node this node follows can make the deposit to the address on the left, if it holds coins.</p>
          {#if !connected}
            <p class="side-note side-warn">The {mainName} node is not reachable.</p>
          {:else}
            <dl class="facts">
              <dt>Wallet</dt><dd translate="no">{side.connection?.walletSet ? side.connection.wallet || '(default wallet)' : 'its only wallet loaded'}</dd>
              <dt>Available</dt><dd>{#if mainBalance !== null}<Amount sats={mainBalance} />{:else}<span class="muted"><SafeText text={mainBalanceError || '–'} /></span>{/if}</dd>
            </dl>
            {#key helperKey}<AmountInput id="deposit-amount" label="Amount" bind:value={helperAmount} max={mainBalance ?? undefined} />{/key}
            <div class="row">
              <button type="submit" class="btn primary" id="deposit-helper" disabled={depositing || !depositAddress}><Icon name="down" size={15} /> Deposit</button>
              {#if !depositAddress}<span class="muted small">Get a deposit address first.</span>{/if}
            </div>
          {/if}
        </form>
      </section>
    </div>

    <section class="panel">
      <div class="panel-head">
        <h2>Deposits to this chain</h2>
        <span class="row deposits-ctl">
          <span class="muted small">in the last {formatCount(side.main.events.length)} blocks of {mainName}</span>
          {#if !simple}<label class="points">
            <span class="faint small">Latest</span>
            <select aria-label="How many of the latest deposits" value={depositPoints} onchange={(e) => { depositPoints = Number((e.currentTarget as HTMLSelectElement).value); depositPage = 0; }}>
              {#each DEPOSIT_POINTS as n (n)}<option value={n}>{n === 0 ? t('All') : formatCount(n)}</option>{/each}
            </select>
          </label>{/if}
        </span>
      </div>
      {#if deposits.length === 0}
        <p class="side-empty">{side.main.error ? side.main.error : `No deposit to this chain in the blocks of ${mainName} on record.`}</p>
      {:else}
        <div class="side-table-wrap">
          <table class="side-table" use:stacked aria-label="Deposits to this chain">
            <thead><tr><th>State</th>{#if !simple}<th>Mainchain block</th>{/if}<th>Time</th><th class="end">Amount</th>{#if !simple}<th>Destination</th><th>Transaction</th>{/if}</tr></thead>
            <tbody>
              {#each depositRows as d (`${d.txid}:${d.burnindex}`)}
                <tr>
                  <td><span class="status {d.credited ? 'ok' : 'wait'}">{d.credited ? 'Credited' : 'On its way'}</span>{#if ownTargets.has(`${d.txid}:${d.burnindex}`)} <span class="badge accent">yours</span>{/if}</td>
                  {#if !simple}<td class="num">{formatCount(d.height)}</td>{/if}
                  <td>{formatTime(d.time, true)}</td>
                  <td class="end"><Amount sats={d.sats} /></td>
                  {#if !simple}
                    <td class="cut mono" title={d.destination}><SafeText text={d.destination} /></td>
                    <td class="mono"><span translate="no">{shortHash(d.txid, 10)}</span></td>
                  {/if}
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
        <Pager total={depositsKept.length} bind:page={depositPage} bind:size={depositSize} sizes={DEPOSIT_SIZES} label="deposits" />
      {/if}
    </section>
  {:else if tab === 'withdraw'}
    <div class="side-cols">
      <section class="panel">
        <div class="panel-head"><h2>Withdraw to {mainName}</h2></div>
        {#if noWallet}
          <div class="panel-body"><NoWallet page={PAGES.peg} /></div>
        {:else}
          <form class="panel-body side-form" onsubmit={(e) => { e.preventDefault(); withdraw(); }}>
            <p class="side-note">
              The coins leave this wallet at once. {mainName} pays them once its miners have voted the withdrawal through, in a bundle with those of others, which takes long. Until the withdrawal is in a bundle being voted on, it can be taken back.
            </p>
            {#key formKey}
              <AddressInput id="withdraw-address" label="Address of {mainName}" of="mainchain" bind:value={mainAddress} bind:valid={mainAddressValid} />
            {/key}
            {#if connected}<div><button type="button" class="btn small ghost" onclick={addressOfMainWallet}><Icon name="wallet" size={14} /> An address of the {mainName} wallet</button></div>{/if}
            {#key formKey}<AmountInput id="withdraw-amount" label="Amount" bind:value={amount} />{/key}
            <AmountInput id="withdraw-fee" label="Fee for the miners of {mainName}" bind:value={fee} title="Paid on top of the amount. Withdrawals that offer more are bundled first." />
            <dl class="facts"><dt>Leaves this wallet</dt><dd><Amount sats={total} /></dd></dl>
            <div class="row"><button type="submit" class="btn primary" id="withdraw-button" disabled={withdrawing}><Icon name="up" size={15} /> Withdraw</button></div>
          </form>
        {/if}
      </section>

      <section class="panel">
        <div class="panel-head"><h2>How a withdrawal is paid</h2></div>
        <ol class="panel-body steps">
          <li><strong>Waiting</strong> The withdrawal is in a block of this chain, with the others that wait. It can be taken back.</li>
          <li><strong>In a bundle</strong> A block of this chain puts the waiting withdrawals, those that offer the most first, in a bundle: one transaction of {mainName} that pays them all.</li>
          <li><strong>Voted on</strong> The miners of {mainName} vote on the bundle, block after block{minScore ? `; it needs ${minScore} votes` : ''}{side.main.drivechain ? ` within ${side.main.drivechain.withdrawalperiod} blocks` : ''}.</li>
          <li><strong>Paid</strong> With the votes it needs, the bundle is paid out of the escrow of this sidechain. If it fails, its withdrawals go in a later one.</li>
        </ol>
        {#if simple && bundle?.status === 'pending' && voted && minScore}
          <!-- Simple mode: how far the withdrawals being voted on are, in a line. -->
          <div class="panel-body simple-votes">
            <Meter value={voted.score} max={minScore} label="Approval of the withdrawals" text="{Math.round(voteProgress(voted.score, minScore))}%" tone={voted.payable ? 'positive' : 'accent'} />
            <p class="side-note">{voted.payable ? 'Approved: paid with the next block.' : `The withdrawals being voted on are ${Math.round(voteProgress(voted.score, minScore))}% approved, ${voted.blocksleft} blocks left.`}</p>
          </div>
        {/if}
      </section>
    </div>

    <section class="panel">
      <div class="panel-head">
        <h2>Withdrawals not paid yet</h2>
        <label class="check small"><input type="checkbox" bind:checked={onlyMine} disabled={noWallet} /> <span>Only those of this wallet</span></label>
      </div>
      {#if shownWithdrawals.length === 0}
        <p class="side-empty">{onlyMine ? 'This wallet has no withdrawal waiting.' : 'No withdrawal is waiting.'}</p>
      {:else}
        <div class="side-table-wrap">
          <table class="side-table" use:stacked aria-label="Withdrawals not paid yet">
            <thead><tr><th>State</th><th class="end">Amount</th>{#if !simple}<th class="end">Mainchain fee</th><th>Block</th><th>Refund address</th><th>Transaction</th>{/if}<th></th></tr></thead>
            <tbody>
              {#each shownWithdrawals as w (withdrawalKey(w))}
                <tr>
                  <td><span class="status {w.status === 'bundled' ? 'wait' : ''}">{w.status === 'bundled' ? 'Being voted on' : 'Waiting'}</span>{#if mine.has(withdrawalKey(w))} <span class="badge accent">yours</span>{/if}</td>
                  <td class="end"><Amount sats={coinsToSats(w.amount)} /></td>
                  {#if !simple}
                    <td class="end"><Amount sats={coinsToSats(w.mainchainfee)} /></td>
                    <td class="num">{formatCount(w.height)}</td>
                    <td class="cut mono"><span translate="no">{w.refundaddress}</span></td>
                    <td class="mono"><span translate="no">{shortHash(w.txid, 10)}:{w.vout}</span></td>
                  {/if}
                  <td class="end">
                    {#if mine.has(withdrawalKey(w)) && w.status === 'waiting'}
                      <button type="button" class="btn small" disabled={refunding !== null} onclick={() => refund(w)}>Take back</button>
                    {/if}
                  </td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>
  {:else}
    <section class="panel bundle">
      <div class="panel-head"><h2>Current bundle</h2>{#if bundle?.hash}<span class="mono small" translate="no">{shortHash(bundle.hash, 12)}</span>{/if}</div>
      <div class="panel-body side-form">
        {#if !bundle}
          <p class="muted">Asking the node…</p>
        {:else if bundle.status === 'none'}
          <p>No bundle is being voted on{bundle.waiting ? `, and the ${bundle.waiting} waiting withdrawal(s) cannot be bundled yet` : ''}.</p>
          {#if bundle.lastfailureheight !== undefined}<p class="side-note side-warn">The last bundle failed (learned at block {formatCount(bundle.lastfailureheight)}): a new one can be made after a waiting time.</p>{/if}
        {:else}
          <dl class="facts">
            <dt>State</dt><dd>{bundle.status === 'pending' ? `Being voted on by the miners of ${mainName}` : 'The next block of this chain can start it'}</dd>
            <dt>Withdrawals</dt><dd class="num">{bundle.withdrawals ?? 0}{bundle.waiting ? ` (and ${bundle.waiting} waiting for a later bundle)` : ''}</dd>
            <dt>Pays out</dt><dd><Amount sats={bundle.amount === undefined ? null : coinsToSats(bundle.amount)} /></dd>
            <dt>Fees for the miners</dt><dd><Amount sats={bundle.mainchainfee === undefined ? null : coinsToSats(bundle.mainchainfee)} /></dd>
            {#if bundle.height !== undefined}<dt>Committed in block</dt><dd class="num">{formatCount(bundle.height)}</dd>{/if}
            <dt>Hash</dt><dd class="mono wrap"><span translate="no">{bundle.hash}</span></dd>
          </dl>
          {#if bundle.status === 'pending'}
            {#if voted && minScore}
              <div class="votes">
                <Meter value={voted.score} max={minScore} label="Votes for the bundle" text="{voted.score} of {minScore}" tone={voted.payable ? 'positive' : bundleCanPass(voted, minScore) ? 'accent' : 'negative'} />
                <p class="side-note">
                  {voted.payable ? `It has the votes it needs: the next block of ${mainName} can pay it.` : `${Math.round(voteProgress(voted.score, minScore))}% of the votes it needs, ${voted.blocksleft} blocks left.`}
                  {#if !voted.payable && !bundleCanPass(voted, minScore)}It cannot reach the score any more: it will fail, and its withdrawals go in a later bundle.{/if}
                </p>
              </div>
            {:else}
              <p class="side-note">{side.main.error ? `The votes are on ${mainName}, which is not reachable.` : `${mainName} has not proposed it yet: it does once a block of ${mainName} takes it.`}</p>
            {/if}
          {/if}
        {/if}
      </div>
    </section>

    <section class="panel">
      <div class="panel-head"><h2>Bundles</h2><span class="muted small">the last {formatCount(side.main.events.length)} blocks of {mainName}</span></div>
      {#if history.length === 0}
        <p class="side-empty">{side.main.error ? side.main.error : `No bundle of this chain in the blocks of ${mainName} on record.`}</p>
      {:else}
        <div class="side-table-wrap">
          <table class="side-table" use:stacked aria-label="Bundles">
            <thead><tr><th>Result</th><th>Bundle</th><th>Proposed in block</th><th>Closed in block</th><th class="end">Last score</th></tr></thead>
            <tbody>
              {#each history as h (h.hash)}
                <tr class:chosen={focusBundle === h.hash} onclick={() => (focusBundle = h.hash)}>
                  <td><span class="status {bundleTone(h)}">{h.closed === undefined ? 'Being voted on' : h.paid ? 'Paid' : 'Failed'}</span></td>
                  <td class="mono"><span translate="no">{shortHash(h.hash, 12)}</span></td>
                  <td class="num">{h.proposed === undefined ? '–' : formatCount(h.proposed)}{#if h.proposedTime} <span class="faint small">{formatTime(h.proposedTime, true)}</span>{/if}</td>
                  <td class="num">{h.closed === undefined ? '–' : formatCount(h.closed)}{#if h.closedTime} <span class="faint small">{formatTime(h.closedTime, true)}</span>{/if}</td>
                  <td class="end num">{h.score}{minScore ? ` / ${minScore}` : ''}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>
  {/if}
</div>

<style>
  .qr-row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 1rem;
  }
  .steps {
    margin: 0;
    padding-left: 2.4rem;
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    color: var(--text-muted);
  }
  .steps strong {
    display: block;
    color: var(--text);
  }
  .bundle {
    margin-bottom: 1rem;
  }
  .votes {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    max-width: 36rem;
  }
  .small {
    font-size: 0.85rem;
  }
  .wrap {
    overflow-wrap: anywhere;
    min-width: 0;
  }
  .deposits-ctl {
    display: inline-flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem 0.8rem;
  }
  .points {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
  }
  .points select {
    width: auto;
    min-height: 1.9rem;
  }
</style>
