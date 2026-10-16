<script lang="ts">
  // Private: the shielded wallet of zSide (the Private page of zside-qt, redone).
  //  - Overview: the transparent and the shielded balances side by side, each with its privacy, the moves
  //    between them, how much of the wallet is shielded, and the latest of the shielded account.
  //  - Pay: privately, out of the shielded balance, to one or more addresses, with a memo the shielded
  //    recipients alone read; to a transparent address it unshields, and says so.
  //  - Shield & unshield: transparent coins into the pool (to the wallet's account or another's), and out
  //    of it (to a new address of the wallet or another).
  //  - Receive: the shielded address of the account, and new ones nobody can link to it, with QR codes.
  //  - Keys: the viewing key of the account, exported after a warning; one watched in a wallet without keys.
  // Every proof runs in the node, on a lane of its own, while ProvingPanel says how far along it seems.
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import QrCode from '../../components/QrCode.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { ask, confirm, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { formatTime, shortHash } from '../../lib/format';
  import { t, tn } from '../../lib/i18n.svelte';
  import { go, nav, PAGES } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { request } from '../../lib/bridge';
  import { declined } from '../../lib/side';
  import { app, prefs, toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import { coinsToSats, formatAmount, satsToCoins } from '../../lib/units';
  import { current, refreshInfo } from '../../lib/walletdata.svelte';
  import '../zs/zs.css';
  import Privacy from '../zs/Privacy.svelte';
  import ProvingPanel from '../zs/ProvingPanel.svelte';
  import Ring from '../zs/Ring.svelte';
  import ZAddressInput from '../zs/ZAddressInput.svelte';
  import { describeTx, isViewingKey, MEMO_BYTES, memoBytes, viewingKeyHrp } from '../zs/model';
  import { prove, proving, refreshZs, startZs, zs } from '../zs/store.svelte';
  import Term from '../../components/Term.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';

  $effect(() => startZs());
  $effect(() => {
    void wallets.current;
    void refreshInfo();
  });

  const TABS = [
    { id: 'overview', label: 'Overview' },
    { id: 'pay', label: 'Pay' },
    { id: 'move', label: 'Shield & unshield' },
    { id: 'receive', label: 'Receive' },
    { id: 'keys', label: 'Viewing keys' },
  ];
  const tab = $derived(TABS.some((x) => x.id === nav.tab) ? nav.tab : 'overview');
  // Simple mode (lib/uimode.svelte.ts): the private balance, making coins private (or public again) and the
  // private address, in plain words; paying is the Send page's "Send privately", the viewing keys are
  // Advanced's (both shown when asked for: a link, the palette). The same calls in both modes.
  const simple = $derived(simpleMode());
  const SIMPLE_LABELS: Record<string, string> = { overview: 'Private balance', move: 'Make private', receive: 'Private address' };
  const tabs = $derived(simple ? TABS.filter((x) => x.id in SIMPLE_LABELS || x.id === tab).map((x) => ({ ...x, label: SIMPLE_LABELS[x.id] ?? x.label })) : TABS);
  const select = (id: string) => (nav.tab = id);

  const wallet = $derived(wallets.current);
  const noWallet = $derived(!walletsEnabled() || wallet === null);
  const network = $derived(app.info?.network ?? 'main');
  const busy = $derived(proving.now !== null);
  /** A wallet without private keys: it watches viewing keys, it pays nobody. */
  const watchOnly = $derived(!!current.info?.privateKeysDisabled);

  const available = $derived(coinsToSats(zs.balance?.available ?? 0));
  const pending = $derived(coinsToSats(zs.balance?.pending ?? 0));
  const watched = $derived(coinsToSats(zs.balance?.watchonly ?? 0));
  const transparent = $derived(coinsToSats(zs.transparent?.trusted ?? 0));
  const transparentPending = $derived(coinsToSats((zs.transparent?.pending ?? 0) + (zs.transparent?.immature ?? 0)));
  const shieldedAll = $derived(available + pending);
  const share = $derived(shieldedAll + transparent + transparentPending > 0n ? Number(shieldedAll) / Number(shieldedAll + transparent + transparentPending) : 0);
  const latest = $derived(zs.txs.slice(0, 5).map(describeTx));

  /** The shielded account is not set up: the wallet is locked and was never unlocked since it was made. */
  async function setUp() {
    if (wallet === null) return;
    try {
      const address = await walletRpc<string>(wallet, 'getshieldedaddress', []);
      if (address !== null) await refreshZs();
    } catch (e) {
      await inform(t('Private'), errorText(e), 'warning');
    }
  }

  // --- Pay ---
  interface Recipient {
    key: number;
    address: string;
    valid: boolean;
    kind: 'shielded' | 'transparent' | '';
    amount: bigint | null;
  }
  let nextKey = 1;
  let recipients = $state<Recipient[]>([{ key: 0, address: '', valid: false, kind: '', amount: null }]);
  let memo = $state('');
  let customFee = $state(false);
  let fee = $state<bigint | null>(null);
  let payKey = $state(0);
  const memoSize = $derived(memoBytes(memo));
  const anyShielded = $derived(recipients.some((r) => r.kind === 'shielded'));
  const anyTransparent = $derived(recipients.some((r) => r.kind === 'transparent'));
  const payTotal = $derived(recipients.reduce((n, r) => n + (r.amount ?? 0n), 0n));
  const payProblem = $derived.by(() => {
    if (recipients.some((r) => !r.valid || r.amount === null || r.amount <= 0n)) return '';
    const seen = new Set<string>();
    for (const r of recipients) {
      const a = r.address.trim();
      if (seen.has(a)) return t('An address is twice in the list: one amount each.');
      seen.add(a);
    }
    if (memoSize > MEMO_BYTES) return t('The memo is longer than %1 bytes.', String(MEMO_BYTES));
    if (payTotal > available) return t('More than the shielded balance in blocks (%1).', formatAmount(available, prefs.unit));
    if (customFee && (fee === null || fee <= 0n)) return t('Enter the fee.');
    return '';
  });
  const payReady = $derived(!payProblem && recipients.every((r) => r.valid && r.amount !== null && r.amount > 0n) && !busy && !watchOnly);

  function addRecipient() {
    recipients = [...recipients, { key: nextKey++, address: '', valid: false, kind: '', amount: null }];
  }
  function removeRecipient(key: number) {
    recipients = recipients.filter((r) => r.key !== key);
  }
  function payAll() {
    if (recipients.length !== 1) return;
    recipients[0].amount = available;
    payKey++;
  }

  async function pay() {
    if (wallet === null || !payReady) return;
    const lines = recipients.map((r) => `${formatAmount(r.amount ?? 0n, prefs.unit)} ${t('to')} ${r.address.trim()}${r.kind === 'transparent' ? ` (${t('public')})` : ''}`);
    const r = await ask({
      title: anyTransparent && !anyShielded ? t('Unshield') : t('Pay privately'),
      message: anyTransparent
        ? t('Pay out of the shielded balance? The amounts and the transparent addresses will be public; where the coins came from will not.')
        : t('Pay out of the shielded balance, privately? Only the recipients see the amounts and the memo.'),
      code: lines.join('\n'),
      informative: memo && anyShielded ? t('The memo goes to the shielded recipients.') : undefined,
      level: anyTransparent ? 'warning' : 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: anyTransparent && !anyShielded ? 'Unshield' : 'Pay', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    const amounts: Record<string, string> = {};
    for (const x of recipients) amounts[x.address.trim()] = satsToCoins(x.amount ?? 0n);
    const params: unknown[] = [amounts, anyShielded ? memo : ''];
    if (customFee && fee !== null) params.push(satsToCoins(fee));
    try {
      const done = await prove('pay', 'Proving the payment', () => walletRpc<{ txid: string }>(wallet, 'sendshielded', params));
      if (!done) return;
      toast('success', t('Paid'), t('With the next block. Transaction %1.', shortHash(done.txid, 10)));
      recipients = [{ key: nextKey++, address: '', valid: false, kind: '', amount: null }];
      memo = '';
      fee = null;
      customFee = false;
      payKey++;
    } catch (e) {
      await inform(t('Pay privately'), errorText(e), 'error');
    }
  }

  // --- Shield ---
  let shieldAmount = $state<bigint | null>(null);
  let shieldMemo = $state('');
  let shieldElsewhere = $state(false);
  let shieldTo = $state('');
  let shieldToValid = $state(false);
  let shieldKey = $state(0);
  /** Left for the fee when shielding all (as zside-qt did). */
  const SHIELD_FEE_ROOM = 100_000n;
  const shieldProblem = $derived.by(() => {
    if (shieldAmount === null) return '';
    if (shieldAmount <= 0n) return t('Enter an amount above zero.');
    if (shieldAmount > transparent) return t('More than the transparent balance in blocks (%1).', formatAmount(transparent, prefs.unit));
    if (memoBytes(shieldMemo) > MEMO_BYTES) return t('The memo is longer than %1 bytes.', String(MEMO_BYTES));
    return '';
  });
  function shieldAll() {
    shieldAmount = transparent > SHIELD_FEE_ROOM ? transparent - SHIELD_FEE_ROOM : null;
    shieldKey++;
  }
  async function shield() {
    if (wallet === null || shieldAmount === null || shieldProblem || busy) return;
    if (shieldElsewhere && !shieldToValid) return;
    const amount = formatAmount(shieldAmount, prefs.unit);
    const yes = await confirm(
      t('Shield'),
      shieldElsewhere ? t('Move %1 of transparent coins into the shielded address below?', amount) : t('Move %1 of transparent coins into the shielded balance?', amount),
      { yes: t('Shield'), informative: t('The move into the pool is public: the amount shows on the chain. What you do with the coins after is not.') },
    );
    if (!yes) return;
    try {
      const done = await prove('shield', 'Proving the shielding', () =>
        walletRpc<{ txid: string }>(wallet, 'shieldcoins', [satsToCoins(shieldAmount ?? 0n), shieldElsewhere ? shieldTo.trim() : null, shieldMemo]),
      );
      if (!done) return;
      toast('success', t('Shielding %1', amount), t('Spendable after the next block.'));
      shieldAmount = null;
      shieldMemo = '';
      shieldTo = '';
      shieldKey++;
    } catch (e) {
      await inform(t('Shield'), errorText(e), 'error');
    }
  }

  // --- Unshield ---
  let unshieldAmount = $state<bigint | null>(null);
  let unshieldEverything = $state(false);
  let unshieldTo = $state('');
  let unshieldToValid = $state(false);
  let subtractFee = $state(false);
  let unshieldKey = $state(0);
  $effect(() => {
    // Typing another amount undoes "all".
    if (unshieldEverything && unshieldAmount !== available) unshieldEverything = false;
  });
  const unshieldProblem = $derived.by(() => {
    if (unshieldEverything) return available > 0n ? '' : t('Nothing in the shielded balance in blocks yet.');
    if (unshieldAmount === null) return '';
    if (unshieldAmount <= 0n) return t('Enter an amount above zero.');
    if (unshieldAmount > available) return t('More than the shielded balance in blocks (%1).', formatAmount(available, prefs.unit));
    return '';
  });
  function unshieldAll() {
    unshieldAmount = available;
    unshieldEverything = true;
    unshieldKey++;
  }
  async function unshield() {
    if (wallet === null || unshieldProblem || busy) return;
    if (!unshieldEverything && unshieldAmount === null) return;
    if (unshieldTo.trim() && !unshieldToValid) return;
    const what = unshieldEverything ? t('the whole shielded balance in blocks (less the fee)') : formatAmount(unshieldAmount ?? 0n, prefs.unit);
    const r = await ask({
      title: t('Unshield'),
      message: t('Move %1 out of the shielded balance?', what),
      code: unshieldTo.trim() || undefined,
      informative: unshieldTo.trim()
        ? t('The amount and the address will be public; where the coins came from will not. For amounts nobody can link to what went in, use Melt & cast.')
        : t('To a new transparent address of this wallet. The amount and the address will be public; where the coins came from will not.'),
      level: 'warning',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Unshield', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    const amount = unshieldEverything ? 'all' : satsToCoins(unshieldAmount ?? 0n);
    try {
      const done = await prove('unshield', 'Proving the unshielding', () =>
        walletRpc<{ txid: string; address: string; amount: number }>(wallet, 'unshieldcoins', [amount, unshieldTo.trim() || null, unshieldEverything || subtractFee]),
      );
      if (!done) return;
      toast('success', t('Unshielding %1', formatAmount(coinsToSats(done.amount), prefs.unit)), t('To %1, spendable after the next block.', shortHash(done.address, 12)));
      unshieldAmount = null;
      unshieldEverything = false;
      unshieldTo = '';
      subtractFee = false;
      unshieldKey++;
    } catch (e) {
      await inform(t('Unshield'), errorText(e), 'error');
    }
  }

  // --- Receive ---
  /** The addresses made here, newest first: each pays the same account, and none can be linked to another. */
  let fresh = $state<{ address: string; time: number }[]>([]);
  let shown = $state('');
  const receiveAddress = $derived(shown || zs.address);
  async function newAddress() {
    if (wallet === null) return;
    try {
      const address = await rpc.raw<string>('getshieldedaddress', [true], { wallet });
      fresh = [{ address, time: Math.floor(Date.now() / 1000) }, ...fresh].slice(0, 20);
      shown = address;
    } catch (e) {
      await inform(t('Receive'), errorText(e), 'warning');
    }
  }
  $effect(() => {
    void wallet;
    fresh = [];
    shown = '';
  });

  // --- Viewing keys ---
  let exported = $state('');
  let revealed = $state(false);
  async function exportKey() {
    if (wallet === null) return;
    const r = await ask({
      title: t('Export the viewing key'),
      message: t('Whoever holds the viewing key sees every payment this account receives and makes, the amounts and the memos, from its first to its last.'),
      informative: t('It cannot spend, and it cannot be taken back once given. Give it only to someone who is to see all of that, such as an auditor, or to a watch-only wallet of your own.'),
      level: 'warning',
      checkbox: { label: t('I understand that it shows all my private payments to whoever has it'), checked: false },
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Show the key', kind: 'primary', delay: 3 },
      ],
    });
    if (r.choice !== 'yes') return;
    if (!r.checked) {
      await inform(t('Export the viewing key'), t('Tick the box to say you understand what the key shows, then show it again.'), 'info');
      return;
    }
    try {
      // The host shows the key only after the desktop asked (audit port-2 #3): a no is taken quietly.
      exported = await request<string>('host', 'zside.viewingkey.export', { wallet });
      revealed = false;
    } catch (e) {
      if (!declined(e)) await inform(t('Viewing keys'), errorText(e), 'warning');
    }
  }
  $effect(() => {
    void wallet;
    exported = '';
    revealed = false;
  });

  let importText = $state('');
  let importHeight = $state('0');
  let importing = $state(false);
  const importValid = $derived(isViewingKey(importText, network));
  const heightValid = $derived(/^\d{1,9}$/.test(importHeight.trim()));
  async function importKey() {
    if (wallet === null || !importValid || !heightValid || importing) return;
    const yes = await confirm(t('Watch a viewing key'), t('Watch this viewing key in the wallet %1? The blocks from %2 are scanned again for it, which may take a while.', wallet, importHeight.trim()), {
      yes: t('Watch'),
      informative: t('What it sees shows in this wallet as watch-only: it cannot be spent.'),
    });
    if (!yes) return;
    importing = true;
    try {
      await rpc.raw('importshieldedviewingkey', [importText.trim(), Number(importHeight.trim())], { wallet });
      toast('success', t('Viewing key watched'), t('What it sees shows as watch-only.'));
      importText = '';
      await refreshZs();
    } catch (e) {
      await inform(t('Watch a viewing key'), errorText(e), 'error');
    } finally {
      importing = false;
    }
  }

  function kindWord(kind: string): string {
    return kind === 'shield' ? t('Shielded') : kind === 'unshield' ? t('Unshielded') : kind === 'pay' ? t('Paid privately') : t('Received privately');
  }
</script>

<div class="page privatepage zs">
  <header class="page-head">
    <h1>{t(PAGES.private.title)}</h1>
  </header>

  {#if noWallet}
    <NoWallet page={PAGES.private} />
  {:else}
    {#if simple}
      <p class="muted lead-simple">Coins here are <Term id="shielded">private</Term>: nobody can see how much you hold or whom you pay. Deposits arrive public; make them private, then send privately from the Send page.</p>
    {/if}
    <TabRow label="Private" tabs={tabs} current={tab} {select} />

    <ProvingPanel />

    {#if zs.unlockNeeded}
      <div class="zs-note private">
        <Icon name="lock" size={18} />
        <div>
          <p>The shielded account of this wallet is made from its keys the first time the wallet is unlocked. Unlock it once to set it up: nothing more to back up.</p>
          <button type="button" class="btn primary" onclick={setUp}>Unlock to set up</button>
        </div>
      </div>
    {:else if zs.error}
      <div class="zs-note danger"><Icon name="alert" size={18} /><SafeText text={zs.error} /></div>
    {/if}
    {#if zs.balance?.rescanning_from !== undefined}
      <div class="zs-note"><Icon name="refresh" size={18} /><span>{t('The blocks from %1 are being scanned again for the shielded account: what they paid it shows when the scan is done.', String(zs.balance.rescanning_from))}</span></div>
    {/if}

    {#if tab === 'overview'}
      <section class="zs-split" aria-label="Balances">
        <div class="zs-vault public">
          <div class="head"><span class="label">{simple ? t('Public') : t('Transparent')}</span><Privacy kind="public" /></div>
          <span class="big"><Amount sats={transparent} /></span>
          <span class="sub">
            {#if transparentPending > 0n}<Amount sats={transparentPending} /> waiting for a block or maturing ·{/if}
            Deposits arrive here; withdrawals leave from here.
          </span>
        </div>
        <div class="zs-moves">
          <button type="button" class="btn primary" disabled={watchOnly} onclick={() => select('move')}><Icon name="shield" size={16} /> {simple ? t('Make private') : t('Shield')}</button>
          <button type="button" class="btn" disabled={watchOnly} onclick={() => select('move')}><Icon name="eye" size={16} /> {simple ? t('Make public') : t('Unshield')}</button>
          {#if !simple}<button type="button" class="btn ghost" onclick={() => go('meltcast')}><Icon name="meltcast" size={16} /> Melt & cast</button>{/if}
        </div>
        <div class="zs-vault private">
          <div class="head"><span class="label">{simple ? t('Private') : t('Shielded')}</span><Privacy kind="private" /></div>
          <span class="big"><Amount sats={available} /></span>
          <span class="sub">
            {#if pending > 0n}<Amount sats={pending} /> waiting for a block ·{/if}
            {#if simple}Only you can see it.{:else}{tn('%n note(s)', zs.balance?.notes ?? 0)}{/if}
            {#if watched > 0n}· <Amount sats={watched} /> watch-only{/if}
          </span>
        </div>
      </section>

      <div class="zs-cols">
        <section class="panel">
          <div class="panel-head"><h2>How private is this wallet</h2></div>
          <div class="panel-body share">
            <Ring {share} label={t('%1% of the wallet is shielded', String(Math.round(share * 100)))} />
            <div>
              <p>{t('%1% of what this wallet holds is in the shielded pool.', String(Math.round(share * 100)))}</p>
              <p class="muted small">Transparent coins show on the chain with their addresses. Shielded ones show nothing: not the amount, not who paid whom.</p>
              <div class="zs-actions">
                <button type="button" class="btn primary" disabled={watchOnly || busy || available <= 0n} onclick={() => (simple ? go('send') : select('pay'))}><Icon name="private" size={16} /> {simple ? t('Send privately') : t('Pay privately')}</button>
                <button type="button" class="btn" onclick={() => select('receive')}><Icon name="qr" size={16} /> Shielded address</button>
              </div>
            </div>
          </div>
        </section>

        <section class="panel">
          <div class="panel-head">
            <h2>Latest</h2>
            <button type="button" class="btn ghost small" onclick={() => go('zhistory')}>All <Icon name="arrow" size={14} /></button>
          </div>
          <div class="panel-body">
            {#if latest.length === 0}
              <p class="muted">Nothing yet: shield some coins to start.</p>
            {:else}
              <ul class="zs-list latest">
                {#each latest as tx (tx.txid)}
                  <li>
                    <span class="what">
                      <strong>{kindWord(tx.kind)}</strong>
                      <span class="faint small">{formatTime(tx.time, true)}{#if tx.confirmations === 0} · {t('waiting for a block')}{/if}</span>
                    </span>
                    <Privacy kind={tx.public ? 'public' : 'private'} />
                    <span class="num end"><Amount sats={tx.amount} plus /></span>
                  </li>
                {/each}
              </ul>
            {/if}
          </div>
        </section>
      </div>
    {:else if tab === 'pay'}
      <div class="zs-cols">
        <section class="panel">
          <div class="panel-head"><h2>Pay privately</h2><Privacy kind={anyTransparent ? 'mixed' : 'private'} /></div>
          <div class="panel-body zs-form">
            {#if watchOnly}
              <div class="zs-note"><Icon name="eye" size={18} /><span>This wallet has no private keys: it watches, it cannot pay.</span></div>
            {/if}
            {#key payKey}
              {#each recipients as r, i (r.key)}
                <div class="recipient">
                  <ZAddressInput id={`zs-pay-to-${r.key}`} label={recipients.length > 1 ? t('Pay to (%1)', String(i + 1)) : t('Pay to')} bind:value={r.address} bind:valid={r.valid} bind:kind={r.kind} disabled={busy} />
                  <div class="amount-row">
                    <AmountInput id={`zs-pay-amount-${r.key}`} label="Amount" bind:value={r.amount} max={available} disabled={busy} />
                    {#if recipients.length === 1}
                      <button type="button" class="btn small" disabled={busy || available <= 0n} onclick={payAll} title="The whole shielded balance in blocks; the fee is taken on top, so leave a little">All</button>
                    {:else}
                      <button type="button" class="btn ghost small" disabled={busy} onclick={() => removeRecipient(r.key)} aria-label="Remove this recipient" title="Remove this recipient"><Icon name="trash" size={15} /></button>
                    {/if}
                  </div>
                </div>
              {/each}
            {/key}
            <div class="zs-actions"><button type="button" class="btn ghost small" disabled={busy || recipients.length >= 10} onclick={addRecipient}><Icon name="plus" size={15} /> Add a recipient</button></div>

            <div class="field">
              <label for="zs-pay-memo">Memo</label>
              <textarea id="zs-pay-memo" bind:value={memo} disabled={busy || !anyShielded} placeholder={anyShielded ? t('A memo only the recipient can read (optional, 512 bytes at most)') : t('A memo goes to a shielded address only')}></textarea>
              <span class="zs-counter" class:over={memoSize > MEMO_BYTES}>{t('%1 / %2 bytes', String(memoSize), String(MEMO_BYTES))}</span>
            </div>

            <label class="check"><input type="checkbox" bind:checked={customFee} disabled={busy} /> Set the fee myself</label>
            {#if customFee}
              <AmountInput id="zs-pay-fee" label="Fee" bind:value={fee} disabled={busy} />
            {/if}

            {#if anyTransparent}
              <div class="zs-note public"><Icon name="eye" size={18} /><span>To a transparent address, this unshields: the amount and the address are public, where the coins came from is not. A transparent recipient gets no memo.</span></div>
            {:else}
              <div class="zs-note private"><Icon name="shield" size={18} /><span>Private: nobody but the recipient sees the amount, the memo, or that you paid.</span></div>
            {/if}
            {#if payProblem}<p class="error" role="alert">{payProblem}</p>{/if}
            <div class="zs-actions">
              <button type="button" id="zs-pay" class="btn primary" disabled={!payReady} onclick={pay}><Icon name="private" size={16} /> Prove and pay</button>
              <span class="muted small">{t('Total')}: <Amount sats={payTotal} /></span>
            </div>
          </div>
        </section>

        <aside class="panel">
          <div class="panel-head"><h2>Spendable</h2><Privacy kind="private" /></div>
          <div class="panel-body facts-side">
            <span class="big num"><Amount sats={available} /></span>
            <p class="muted small">The shielded balance in blocks. Change goes back to the shielded account; the payment has no transparent inputs.</p>
            {#if pending > 0n}<p class="small"><Amount sats={pending} /> more once its block comes.</p>{/if}
            <p class="faint small">A payment proves its notes with Halo 2 (no trusted setup): it takes seconds, in the node, not in this window.</p>
          </div>
        </aside>
      </div>
    {:else if tab === 'move'}
      <div class="zs-cols">
        <section class="panel">
          <div class="panel-head"><h2>{simple ? t('Make private') : t('Shield')}</h2><span class="flow"><Privacy kind="public" bare /><Icon name="chevron-right" size={14} /><Privacy kind="private" bare /></span></div>
          <div class="panel-body zs-form">
            {#if simple}
              <p class="muted small">Move public coins of this wallet into your private balance. The amount moved shows on the chain once; what you do with the coins after does not.</p>
            {:else}
              <p class="muted small">Move transparent coins of this wallet into the shielded pool. That move is public; what you do with the coins after is not.</p>
            {/if}
            {#key shieldKey}
              <div class="amount-row">
                <AmountInput id="zs-shield-amount" label="Amount" bind:value={shieldAmount} max={transparent} disabled={busy || watchOnly} />
                <button type="button" class="btn small" disabled={busy || watchOnly || transparent <= SHIELD_FEE_ROOM} onclick={shieldAll} title="All the transparent balance in blocks, less a little for the fee">All</button>
              </div>
            {/key}
            <span class="muted small">{simple ? t('Public, ready to move') : t('Transparent, in blocks')}: <Amount sats={transparent} /></span>
            {#if !simple}
            <label class="check"><input type="checkbox" bind:checked={shieldElsewhere} disabled={busy || watchOnly} /> To another shielded address</label>
            {#if shieldElsewhere}
              <ZAddressInput id="zs-shield-to" label="To" accept="shielded" bind:value={shieldTo} bind:valid={shieldToValid} disabled={busy} />
            {/if}
            <div class="field">
              <label for="zs-shield-memo">Memo</label>
              <textarea id="zs-shield-memo" bind:value={shieldMemo} disabled={busy || watchOnly} placeholder={shieldElsewhere ? t('A memo only the recipient can read (optional, 512 bytes at most)') : t('A note to yourself (optional)')}></textarea>
              <span class="zs-counter" class:over={memoBytes(shieldMemo) > MEMO_BYTES}>{t('%1 / %2 bytes', String(memoBytes(shieldMemo)), String(MEMO_BYTES))}</span>
            </div>
            {/if}
            {#if shieldProblem}<p class="error" role="alert">{shieldProblem}</p>{/if}
            <div class="zs-actions">
              <button type="button" class="btn primary" disabled={busy || watchOnly || shieldAmount === null || !!shieldProblem || (shieldElsewhere && !shieldToValid)} id="zs-shield" onclick={shield}><Icon name="shield" size={16} /> {simple ? t('Make private') : t('Shield')}</button>
            </div>
          </div>
        </section>

        <section class="panel">
          <div class="panel-head"><h2>{simple ? t('Make public') : t('Unshield')}</h2><span class="flow"><Privacy kind="private" bare /><Icon name="chevron-right" size={14} /><Privacy kind="public" bare /></span></div>
          <div class="panel-body zs-form">
            {#if simple}
              <p class="muted small">Move private coins back to a public address of this wallet, for example to send them back to Chains. The amount becomes public; where the coins came from does not.</p>
            {:else}
              <p class="muted small">Move coins out of the shielded pool. The amount and the address become public; where the coins came from does not. For amounts nobody can link to what went in, use Melt & cast.</p>
            {/if}
            {#key unshieldKey}
              <div class="amount-row">
                <AmountInput id="zs-unshield-amount" label="Amount" bind:value={unshieldAmount} max={available} disabled={busy || watchOnly} />
                <button type="button" class="btn small" disabled={busy || watchOnly || available <= 0n} onclick={unshieldAll} title="The whole shielded balance in blocks, the fee taken out of it">All</button>
              </div>
            {/key}
            {#if unshieldEverything}<span class="muted small">The whole shielded balance in blocks, the fee taken out of it.</span>{/if}
            {#if !simple}<ZAddressInput id="zs-unshield-to" label="To (empty: a new address of this wallet)" accept="transparent" required={false} bind:value={unshieldTo} bind:valid={unshieldToValid} disabled={busy || watchOnly} />{/if}
            {#if !unshieldEverything}
              <label class="check"><input type="checkbox" bind:checked={subtractFee} disabled={busy || watchOnly} /> Take the fee out of the amount</label>
            {/if}
            {#if unshieldProblem}<p class="error" role="alert">{unshieldProblem}</p>{/if}
            <div class="zs-actions">
              <button type="button" class="btn primary" disabled={busy || watchOnly || (!unshieldEverything && unshieldAmount === null) || !!unshieldProblem || (!!unshieldTo.trim() && !unshieldToValid)} id="zs-unshield" onclick={unshield}><Icon name="eye" size={16} /> {simple ? t('Make public') : t('Unshield')}</button>
              {#if !simple}<button type="button" class="btn ghost" onclick={() => go('meltcast', 'cast')}>Cast as bills instead</button>{/if}
            </div>
          </div>
        </section>
      </div>
    {:else if tab === 'receive'}
      <div class="zs-cols">
        <section class="panel">
          <div class="panel-head"><h2>{simple ? t('Your private address') : t('Your shielded address')}</h2><Privacy kind="private" /></div>
          <div class="panel-body receive">
            {#if receiveAddress}
              <div class="qr"><QrCode text={receiveAddress} size={210} label={t('QR code of the shielded address')} /></div>
              <div class="addr">
                <SafeText class="zs-code" text={receiveAddress} />
                <div class="zs-actions">
                  <CopyButton text={receiveAddress} label="Copy" small={false} iconOnly={false} />
                  <button type="button" id="zs-new-address" class="btn" onclick={newAddress} title="Another address of the same account, which nobody can link to the others: one per payer, for privacy"><Icon name="plus" size={16} /> New address</button>
                  {#if shown}<button type="button" class="btn ghost" onclick={() => (shown = '')}>The default one</button>{/if}
                </div>
                <p class="muted small">Every address you make here pays the same shielded balance. Give a new one to each payer: nobody can tell they belong together.</p>
              </div>
            {:else if watchOnly}
              <p class="muted">This wallet has no private keys: it has no shielded address of its own. It shows what the viewing keys it watches see.</p>
            {:else}
              <p class="muted">The shielded account is not set up yet.</p>
            {/if}
          </div>
        </section>
        {#if fresh.length}
          <section class="panel">
            <div class="panel-head"><h2>Made just now</h2></div>
            <div class="panel-body">
              <ul class="zs-list">
                {#each fresh as f (f.address)}
                  <li class="fresh">
                    <button type="button" class="link mono" onclick={() => (shown = f.address)}><SafeText text={shortHash(f.address, 18)} /></button>
                    <span class="faint small">{formatTime(f.time, true)}</span>
                    <CopyButton text={f.address} />
                  </li>
                {/each}
              </ul>
              <p class="faint small">Not kept: each is an address of the account for good, made again from its keys.</p>
            </div>
          </section>
        {/if}
      </div>
    {:else if tab === 'keys'}
      <div class="zs-cols">
        <section class="panel">
          <div class="panel-head"><h2>Export the viewing key</h2><Icon name="viewkey" size={18} /></div>
          <div class="panel-body zs-form">
            <div class="zs-note danger">
              <Icon name="alert" size={18} />
              <span>The viewing key shows every payment this account receives and makes, with the amounts and the memos, from its first to its last. It cannot spend, and once given it cannot be taken back.</span>
            </div>
            {#if exported}
              <SafeText class={revealed ? 'zs-code' : 'zs-code hidden'} text={exported} />
              <div class="zs-actions">
                <button type="button" class="btn" onclick={() => (revealed = !revealed)}><Icon name={revealed ? 'eyeoff' : 'eye'} size={16} /> {revealed ? t('Hide') : t('Show')}</button>
                <CopyButton text={exported} label="Copy" small={false} iconOnly={false} />
                <button type="button" class="btn ghost" onclick={() => (exported = '')}>Done</button>
              </div>
            {:else}
              <div class="zs-actions">
                <button type="button" id="zs-export" class="btn" disabled={!zs.address} onclick={exportKey}><Icon name="viewkey" size={16} /> Show the viewing key…</button>
              </div>
            {/if}
          </div>
        </section>

        <section class="panel">
          <div class="panel-head"><h2>Watch a viewing key</h2><Privacy kind="private" why={t('What a viewing key sees stays private: only this wallet reads it.')} /></div>
          <div class="panel-body zs-form">
            <p class="muted small">{t('A wallet without private keys can watch the viewing key of an account (%1…): what it receives and pays shows there, watch-only.', `${viewingKeyHrp(network)}1`)}</p>
            {#if !watchOnly}
              <div class="zs-note">
                <Icon name="info" size={18} />
                <span>This wallet has private keys: a viewing key is watched in a wallet of its own, so that it is not mixed with the account this wallet spends from. Create a wallet with "Disable private keys" on the Wallets page, then watch the key there.</span>
              </div>
              <div class="zs-actions"><button type="button" class="btn" onclick={() => go('wallets')}><Icon name="wallet" size={16} /> Wallets</button></div>
            {:else}
              <div class="field">
                <label for="zs-import-key">Viewing key</label>
                <textarea id="zs-import-key" class="mono" bind:value={importText} spellcheck="false" autocomplete="off" disabled={importing} placeholder={`${viewingKeyHrp(network)}1…`}></textarea>
                {#if importText.trim() && !importValid}<span class="error">{t('Not a viewing key of this network')}</span>{/if}
              </div>
              <div class="field">
                <label for="zs-import-height">Scan from block</label>
                <input id="zs-import-height" type="text" inputmode="numeric" class="num" bind:value={importHeight} disabled={importing} />
                <span class="hint">The account's first payment is not before this block: the higher, the quicker.</span>
              </div>
              <div class="zs-actions">
                <button type="button" id="zs-import" class="btn primary" disabled={!importValid || !heightValid || importing} onclick={importKey}><Icon name="viewkey" size={16} /> {importing ? t('Scanning…') : t('Watch')}</button>
              </div>
            {/if}
            {#if watched > 0n}
              <p class="small">{t('Watch-only in this wallet')}: <Amount sats={watched} /></p>
            {/if}
          </div>
        </section>
      </div>
    {/if}
  {/if}
</div>

<style>
  .lead-simple {
    margin: 0 0 0.6rem;
    max-width: 52rem;
  }
  .privatepage {
    display: grid;
    gap: 1rem;
    align-content: start;
  }
  .zs-note p {
    margin: 0 0 0.55rem;
  }
  .share {
    display: flex;
    gap: 1.1rem;
    align-items: center;
  }
  .share p {
    margin: 0 0 0.45rem;
  }
  .latest li {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto auto;
    gap: 0.7rem;
    align-items: center;
    padding: 0.55rem 0;
  }
  .what {
    display: flex;
    flex-direction: column;
    min-width: 0;
  }
  .end {
    text-align: end;
  }
  .recipient {
    display: grid;
    gap: 0.5rem;
    padding-bottom: 0.7rem;
    border-bottom: 1px dashed var(--line);
  }
  .amount-row {
    display: flex;
    align-items: flex-end;
    gap: 0.5rem;
  }
  .amount-row :global(.field) {
    flex: 1;
  }
  .amount-row .btn {
    margin-bottom: 0.15rem;
  }
  .facts-side {
    display: grid;
    gap: 0.5rem;
  }
  .facts-side p {
    margin: 0;
  }
  .big {
    font-size: 1.6rem;
    font-weight: 700;
  }
  .flow {
    display: inline-flex;
    align-items: center;
    gap: 0.25rem;
    color: var(--text-muted);
  }
  .receive {
    display: flex;
    flex-wrap: wrap;
    gap: 1.2rem;
    align-items: flex-start;
  }
  .qr {
    padding: 0.6rem;
    border-radius: var(--radius-m, 10px);
    background: #fff;
    flex: none;
  }
  .addr {
    display: grid;
    gap: 0.7rem;
    flex: 1 1 16rem;
    min-width: 0;
  }
  .addr p {
    margin: 0;
  }
  .fresh {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    padding: 0.4rem 0;
  }
  .fresh .link {
    flex: 1;
    min-width: 0;
    text-align: start;
  }
  .error {
    margin: 0;
  }
</style>
