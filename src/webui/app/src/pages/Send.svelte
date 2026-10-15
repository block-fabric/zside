<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  import { CHAIN } from '../chain/config';
  import SafeText from '../components/SafeText.svelte';
  // Send coins (SendCoinsDialog, SendCoinsEntry): several recipients, the fee, coin control ("Choose
  // coins": the coins of the wallet in the page), a confirmation with a delay, unsigned transactions
  // (PSBT) and hardware wallets. No sending address book (user decision, 2026-10-15).
  import AddressInput from '../components/AddressInput.svelte';
  import Amount from '../components/Amount.svelte';
  import AmountInput from '../components/AmountInput.svelte';
  import CoinPanel from '../components/CoinPanel.svelte';
  import Dialog from '../components/Dialog.svelte';
  import Icon from '../components/Icon.svelte';
  import { checkAddress } from '../lib/address';
  import { ask, errorText, inform, withUnlock, type Choice } from '../lib/dialogs.svelte';
  import { FILTERS, saveBase64As, safeFileName } from '../lib/files';
  import { niceTimeOffset } from '../lib/format';
  import { go, nav } from '../lib/nav.svelte';
  import { host } from '../lib/rpc';
  import { app, node, prefs, savePrefs, toast, wallets } from '../lib/state.svelte';
  import { formatAmount, UNITS, type Unit } from '../lib/units';
  import { parsePaymentUri } from '../lib/uri';
  import { CONF_TARGETS, isDust, nearestConfTarget, psbtFileName, walletApi, walletDisplayName, type Outpoint, type Prepared } from '../lib/wallet';
  import { current, refreshInfo } from '../lib/walletdata.svelte';

  interface Entry {
    id: number;
    address: string;
    valid: boolean;
    label: string;
    amount: bigint | null;
    unit: Unit;
    subtractFee: boolean;
    message: string;
    /** What is wrong with the amount typed (AmountInput). */
    amountError: string;
    /** The amount to the address would be dust (GUIUtil::isDust). */
    dust: boolean;
  }

  /** Seconds before the send button of the confirmation can be pressed (SEND_CONFIRM_DELAY). */
  const SEND_CONFIRM_DELAY = 3;

  let nextId = 1;
  function newEntry(): Entry {
    return { id: nextId++, address: '', valid: false, label: '', amount: null, unit: prefs.unit, subtractFee: prefs.subtractFeeDefault, message: '', amountError: '', dust: false };
  }

  let entries: Entry[] = $state([newEntry()]);
  let selected: Outpoint[] = $state([]);
  let customChange = $state(false);
  let changeAddress = $state('');
  let changeStatus = $state({ text: '', bad: false, accepted: '' });
  let uriDialog = $state(false);
  let uriText = $state('');
  let uriError = $state('');
  let busy = $state(false);
  let smart = $state<{ feePerK: number; fallback: boolean; target?: number; requiredFeePerK: number } | null>(null);
  let customFee: bigint | null = $state(BigInt(prefs.customFee || 0));

  const wallet = $derived(wallets.current);
  const api = $derived(wallet === null ? null : walletApi(wallet));
  const info = $derived(current.info);
  const balance = $derived(wallet !== null ? wallets.balances[wallet]?.trusted : undefined);
  const spacing = $derived(app.info?.blockSpacing ?? 600);
  const confTarget = $derived(nearestConfTarget(prefs.confTarget || info?.confirmTarget || 6));
  const requiredFee = $derived(BigInt(smart?.requiredFeePerK ?? info?.requiredFeePerK ?? 1000));
  const feeChoice = $derived(prefs.feeMode === 'custom' && customFee !== null ? { confTarget, feeRate: Number(customFee) } : { confTarget });
  // A hardware wallet needs the path of its signer script (SendCoinsDialog::setModel).
  const signerMissing = $derived(!!info?.externalSigner && info.signerConfigured === false);
  const canSend = $derived(!!info && (!info.privateKeysDisabled || info.externalSigner) && !signerMissing);
  const sendLabel = $derived(info?.externalSigner ? 'Sign on device' : info?.privateKeysDisabled ? 'Create Unsigned' : 'Send');
  const sendTip = $derived(
    info?.externalSigner
      ? signerMissing
        ? 'Set external signer script path in Options -> Wallet'
        : 'Connect your hardware wallet first.'
      : info?.privateKeysDisabled
        ? `Creates a Partially Signed Transaction (PSBT) for use with e.g. an offline ${app.info?.name ?? CHAIN.name} wallet, or a PSBT-compatible hardware wallet.`
        : 'Confirm the send action',
  );
  /** A custom fee that is not an amount, or below the minimum: nothing is sent with it (the smart fee is not taken in its place). */
  const customFeeInvalid = $derived(prefs.feeMode === 'custom' && customFee === null);

  // Dust, checked with the node as the amount and the address are typed (SendCoinsEntry::validate).
  $effect(() => {
    const checks = entries.map((e) => ({ e, address: e.address.trim(), amount: e.amount, valid: e.valid }));
    const timer = setTimeout(() => {
      for (const { e, address, amount, valid } of checks) {
        if (!valid || amount === null || amount <= 0n) {
          e.dust = false;
          continue;
        }
        isDust(address, amount)
          .then((d) => {
            if (e.address.trim() === address && e.amount === amount) e.dust = d;
          })
          .catch(() => undefined);
      }
    }, 250);
    return () => clearTimeout(timer);
  });

  $effect(() => {
    if (wallet !== null) refreshInfo();
  });

  // The smart fee of the target (updateSmartFeeLabel), again after each block.
  $effect(() => {
    const target = confTarget;
    void node.tip?.height;
    if (!api) return;
    api
      .smartFee(target)
      .then((s) => (smart = s))
      .catch(() => (smart = null));
  });

  // The custom fee is never below the minimum the wallet requires.
  $effect(() => {
    if (customFee === null && prefs.customFee === 0 && smart) customFee = BigInt(smart.requiredFeePerK);
  });
  $effect(() => {
    if (customFee !== null && Number(customFee) !== prefs.customFee) savePrefs({ customFee: Number(customFee) });
  });

  // "Choose coins" turned off: nothing chosen any more, the wallet chooses.
  $effect(() => {
    if (!prefs.coinControl) {
      selected = [];
      customChange = false;
    }
  });

  // A payment URI (from a link, a second start of the app, or Open URI).
  // Each URI once (nav.uriSeq), also one given before this page was shown.
  $effect(() => {
    const seq = nav.uriSeq;
    const uri = nav.pendingUri;
    if (!uri || seq === nav.uriHandled) return;
    nav.uriHandled = seq;
    pasteUri(uri);
  });
  $effect(() => {
    if (nav.page === 'send' && nav.tab === 'uri') {
      uriText = '';
      uriError = '';
      uriDialog = true;
      nav.tab = '';
    }
  });

  async function pasteUri(uri: string): Promise<boolean> {
    const parsed = parsePaymentUri(uri);
    if (!parsed.ok) {
      await inform(t('Payment request error'), parsed.error, 'error');
      return false;
    }
    const check = await checkAddress(parsed.request.address, app.info?.network ?? 'main');
    if (!check.valid) {
      // PaymentServer::handleURIOrFile: an address of another network, or no address.
      await inform(t('URI handling'), t('Invalid payment address %1', parsed.request.address), 'error');
      return false;
    }
    const first = entries.length === 1 ? entries[0] : null;
    const isClear = first && !first.address && first.amount === null && !first.label;
    const e = isClear ? first : newEntry();
    e.address = parsed.request.address;
    e.label = parsed.request.label ?? '';
    e.amount = parsed.request.amount ?? null;
    e.message = parsed.request.message ?? '';
    if (!isClear) entries.push(e);
    return true;
  }

  async function openUri() {
    uriError = '';
    const parsed = parsePaymentUri(uriText);
    if (!parsed.ok) {
      uriError = parsed.error;
      return;
    }
    uriDialog = false;
    await pasteUri(uriText);
  }

  function addEntry() {
    entries.push(newEntry());
    queueMicrotask(() => document.getElementById(`pay-to-${entries[entries.length - 1].id}`)?.focus());
  }

  function removeEntry(e: Entry) {
    entries = entries.filter((x) => x !== e);
    if (entries.length === 0) entries = [newEntry()];
  }

  function clearAll() {
    entries = [newEntry()];
    selected = [];
    customChange = false;
    changeAddress = '';
    changeStatus = { text: '', bad: false, accepted: '' };
  }

  async function useAvailable(e: Entry) {
    if (!api) return;
    try {
      let amount = await api.available(prefs.coinControl ? selected : undefined);
      for (const other of entries) if (other !== e) amount -= other.amount ?? 0n;
      if (amount > 0n) {
        e.subtractFee = true;
        e.amount = amount;
      } else {
        e.amount = 0n;
      }
    } catch (err) {
      toast('error', t('Use available balance'), errorText(err));
    }
  }

  // Coin control: a custom change address, checked as it is typed (coinControlChangeEdited).
  let changeCheck = 0;
  $effect(() => {
    const text = changeAddress.trim();
    const on = customChange;
    const mine = ++changeCheck;
    if (!on || !api) {
      changeStatus = { text: '', bad: false, accepted: '' };
      return;
    }
    if (!text) {
      changeStatus = { text: '', bad: false, accepted: '' };
      return;
    }
    const timer = setTimeout(async () => {
      const r = await api.checkChange(text).catch(() => null);
      if (mine !== changeCheck || !r) return;
      if (!r.valid) {
        changeStatus = { text: t('Warning: Invalid %1 address', CHAIN.name), bad: true, accepted: '' };
      } else if (!r.mine) {
        changeStatus = { text: t('Warning: Unknown change address'), bad: true, accepted: '' };
        const yes = await ask({
          title: t('Confirm custom change address'),
          message: t('The address you selected for change is not part of this wallet. Any or all funds in your wallet may be sent to this address. Are you sure?'),
          level: 'warning',
          choices: [
            { id: 'cancel', label: 'Cancel', kind: 'primary' },
            { id: 'yes', label: 'Yes', kind: 'danger' },
          ],
        });
        if (mine !== changeCheck) return;
        if (yes.choice === 'yes') changeStatus = { text: t('Warning: Unknown change address'), bad: true, accepted: text };
        else {
          changeAddress = '';
          changeStatus = { text: '', bad: false, accepted: '' };
        }
      } else {
        changeStatus = { text: r.label || t('(no label)'), bad: false, accepted: text };
      }
    }, 250);
    return () => clearTimeout(timer);
  });

  const smartLabel = $derived(smart ? `${formatAmount(BigInt(smart.feePerK), prefs.unit, { withUnit: true })}/kvB` : '–');
  const feeSummary = $derived(prefs.feeMode === 'smart' ? smartLabel : customFee === null ? 'Custom fee not valid' : `${formatAmount(customFee, prefs.unit, { withUnit: true })}/kvB`);

  function alternatives(total: bigint): string {
    return UNITS.filter((u) => u.id !== prefs.unit)
      .map((u) => formatAmount(total, u.id, { withUnit: true }))
      .join(' or ');
  }

  async function presentPsbt(psbt: string, recipients: { address: string; label?: string; amount: bigint }[]) {
    await host.copy(psbt).catch(() => undefined);
    const r = await ask({
      title: t('Unsigned Transaction'),
      message: t('The PSBT has been copied to the clipboard. You can also save it.'),
      code: psbt,
      level: 'info',
      choices: [
        { id: 'discard', label: 'Discard' },
        { id: 'save', label: 'Save…', kind: 'primary' },
      ],
    });
    if (r.choice !== 'save') return;
    const path = await saveBase64As({ title: 'Save Transaction Data', name: safeFileName(psbtFileName(recipients, prefs.unit)), filters: [FILTERS.psbt] }, psbt);
    if (path) toast('success', t('PSBT saved'), t('PSBT saved to disk'));
  }

  async function send() {
    if (!api || wallet === null || busy) return;
    // Every entry checked (SendCoinsEntry::validate): an address, and an amount above zero.
    for (const e of entries) {
      if (!e.valid) {
        document.getElementById(`pay-to-${e.id}`)?.focus();
        toast('warning', t('Send Coins'), e.address ? t('The recipient address is not valid. Please recheck.') : t('Enter an address to pay to.'));
        return;
      }
      if (e.amount === null || e.amount <= 0n) {
        document.getElementById(`amount-${e.id}`)?.focus();
        // What the field says of the text typed (too many decimals, out of range...), or an amount of zero.
        toast('warning', t('Send Coins'), e.amount === null && e.amountError ? e.amountError : t('The amount to pay must be larger than 0.'));
        return;
      }
      if (await isDust(e.address.trim(), e.amount).catch(() => false)) {
        e.dust = true;
        document.getElementById(`amount-${e.id}`)?.focus();
        toast('warning', t('Send Coins'), t('The amount is too small to send: the output would be dust.'));
        return;
      }
    }
    if (customFeeInvalid) {
      // QDoubleSpinBox of chains-qt cannot go below the minimum: here the send waits for a valid fee.
      if (prefs.feeMinimized) savePrefs({ feeMinimized: false });
      queueMicrotask(() => document.getElementById('custom-fee')?.focus());
      toast('warning', t('Send Coins'), t('Enter a custom fee of at least %1/kvB, or choose the recommended fee.', formatAmount(requiredFee, prefs.unit, { withUnit: true })));
      return;
    }
    const recipients = entries.map((e) => ({ address: e.address.trim(), label: e.label, amount: e.amount!, subtractFee: e.subtractFee, message: e.message }));
    const cc = prefs.coinControl ? { selected, changeAddress: customChange && changeStatus.accepted ? changeStatus.accepted : undefined } : {};
    busy = true;
    let prepared: Prepared | null = null;
    try {
      prepared = await withUnlock(wallet, (passphrase) => api.prepare(recipients, cc, feeChoice, passphrase));
    } catch (e) {
      busy = false;
      await inform(t('Send Coins'), errorText(e), 'warning');
      return;
    }
    busy = false;
    if (!prepared) return;

    const info = current.info ?? (await refreshInfo());
    const multi = wallets.list.length > 1;
    const lines = prepared.recipients.map((r) => {
      let amount = formatAmount(r.amount, prefs.unit, { withUnit: true });
      if (multi) amount = t("%1 from wallet '%2'", amount, walletDisplayName(wallet));
      return r.label ? `${t("%1 to '%2'", amount, r.label)} (${r.address})` : t('%1 to %2', amount, r.address);
    });
    const enableSend = !prepared.privateKeysDisabled || prepared.externalSigner;
    let review = t('Please, review your transaction.');
    if (prepared.privateKeysDisabled && !prepared.externalSigner) {
      review = t('Please, review your transaction proposal. This will produce a Partially Signed Transaction (PSBT) which you can save or copy and then sign with e.g. an offline %1 wallet, or a PSBT-compatible hardware wallet.', app.info?.name ?? CHAIN.name);
    } else if (prefs.psbtControls) {
      review = t('Please, review your transaction. You can create and send this transaction or create a Partially Signed Transaction (PSBT), which you can save or copy and then sign with, e.g., an offline %1 wallet, or a PSBT-compatible hardware wallet.', app.info?.name ?? CHAIN.name);
    }
    const kvb = (prepared.vsize / 1000).toPrecision(3);
    const informative = [
      review,
      prepared.fee > 0n ? t('Transaction fee (%1 kvB): %2', kvb, formatAmount(prepared.fee, prefs.unit, { withUnit: true })) : '',
      t('You can increase the fee later.'),
      t('Total Amount: %1', formatAmount(prepared.total, prefs.unit, { withUnit: true })),
      `(= ${alternatives(prepared.total)})`,
      lines.length === 1 ? lines[0] : t('To review recipient list click "Show Details…"'),
    ]
      .filter(Boolean)
      .join('\n');
    const choices: Choice[] = [{ id: 'cancel', label: 'Cancel' }];
    if (prefs.psbtControls || !enableSend) choices.push({ id: 'psbt', label: 'Create Unsigned', delay: SEND_CONFIRM_DELAY });
    if (enableSend) choices.push({ id: 'send', label: info?.externalSigner ? 'Sign on device' : 'Send', kind: 'primary', delay: SEND_CONFIRM_DELAY });
    const answer = await ask({
      title: t('Confirm send coins'),
      message: t('Do you want to create this transaction?'),
      informative,
      details: lines.length > 1 ? lines : undefined,
      level: 'question',
      choices,
    });
    if (answer.choice !== 'send' && answer.choice !== 'psbt') {
      api.discard(prepared.token).catch(() => undefined);
      return;
    }
    busy = true;
    try {
      if (answer.choice === 'psbt') {
        const { psbt } = await api.createPsbt(prepared.token);
        busy = false;
        await presentPsbt(psbt, prepared.recipients);
      } else {
        const result = await api.commit(prepared.token);
        busy = false;
        if (!result.complete && result.psbt) {
          // A hardware wallet signed only part of it (a multisig wallet).
          await presentPsbt(result.psbt, prepared.recipients);
        } else {
          toast('success', t('Sent'), t('Transaction %1…', result.txid?.slice(0, 16)));
          nav.highlightTx = result.txid ?? null;
        }
      }
      clearAll();
    } catch (e) {
      busy = false;
      await inform(t('Send Coins'), errorText(e), 'error');
    }
  }
</script>

<div class="page send">
  <header class="page-head">
    <h1>Send</h1>
    <div class="row">
      <button type="button" class="btn ghost small" onclick={() => (nav.tab = 'uri')}><Icon name="paste" size={16} /> Open URI…</button>
    </div>
  </header>

  {#if node.tip?.ibd}
    <button type="button" class="out-of-sync" onclick={() => go('node', 'info')}>
      <Icon name="alert" size={16} /> The displayed information may be out of date. Your wallet automatically synchronizes with the network after a connection is established, but this process has not completed yet.
    </button>
  {/if}

  <div class="entries">
    {#each entries as e, i (e.id)}
      <section class="panel entry" aria-label="Recipient {i + 1}">
        <div class="entry-grid">
          <AddressInput id="pay-to-{e.id}" label="Pay to" bind:value={e.address} bind:valid={e.valid} />
          <div class="field">
            <label for="label-{e.id}">Label</label>
            <input id="label-{e.id}" type="text" bind:value={e.label} placeholder="Enter a label for this address to add it to the list of used addresses" autocomplete="off" />
          </div>
          <div class="amount-row">
            <div class="grow"><AmountInput id="amount-{e.id}" label="Amount" bind:value={e.amount} bind:unit={e.unit} bind:invalid={e.amountError} extraError={e.dust ? 'The amount is too small to send: the output would be dust.' : ''} /></div>
            <label class="check" title="The fee will be deducted from the amount being sent. The recipient will receive less coins than you enter in the amount field. If multiple recipients are selected, the fee is split equally.">
              <input type="checkbox" bind:checked={e.subtractFee} />
              <span>Subtract fee from amount</span>
            </label>
            <button type="button" class="btn small" onclick={() => useAvailable(e)}>Use available balance</button>
          </div>
          {#if e.message}
            <p class="message" title="A message that was attached to the {CHAIN.uriScheme}: URI which will be stored with the transaction for your reference. Note: This message will not be sent over the {CHAIN.name} network.">
              <strong>Message:</strong> <span class="break" translate="no">{e.message}</span>
            </p>
          {/if}
        </div>
        <button type="button" class="btn ghost icon small remove" title="Remove this entry" aria-label="Remove recipient {i + 1}" onclick={() => removeEntry(e)}>
          <Icon name="x" size={16} />
        </button>
      </section>
    {/each}
  </div>

  <section class="panel coins" aria-labelledby="coins-title">
    <div class="panel-head">
      <h2 id="coins-title">Coins to spend</h2>
      <label class="switch" title="See the coins of the wallet and choose the ones this transaction spends">
        <input type="checkbox" role="switch" checked={prefs.coinControl} onchange={(ev) => savePrefs({ coinControl: (ev.currentTarget as HTMLInputElement).checked })} />
        <span class="track" aria-hidden="true"></span>
        <span>Choose coins</span>
      </label>
    </div>
    <div class="panel-body cc-body">
      {#if !prefs.coinControl || wallet === null}
        <p class="muted auto">Coins are chosen automatically. Turn on "Choose coins" to see the coins of the wallet and pick the ones to spend.</p>
      {:else}
        <CoinPanel {wallet} bind:selected amounts={entries.map((e) => e.amount ?? 0n)} subtractFee={entries.some((e) => e.subtractFee)} fee={feeChoice} />
        <div class="change">
          <label class="check" title="If this is activated, but the change address is empty or invalid, change will be sent to a newly generated address.">
            <input type="checkbox" bind:checked={customChange} />
            <span>Custom change address</span>
          </label>
          <div class="row grow">
            <input type="text" class="mono grow" aria-label="Custom change address" placeholder={t('Enter a %1 address', CHAIN.name)} spellcheck="false" autocomplete="off" disabled={!customChange} bind:value={changeAddress} />
            {#if changeStatus.text}<span class:error={changeStatus.bad} class="change-label"><SafeText text={changeStatus.text} /></span>{/if}
          </div>
        </div>
      {/if}
    </div>
  </section>

  <section class="panel fee" aria-labelledby="fee-title">
    <div class="panel-head">
      <h2 id="fee-title">Transaction fee</h2>
      {#if prefs.feeMinimized}
        <div class="row">
          <span class="muted" class:warn={(smart?.fallback && prefs.feeMode === 'smart') || customFeeInvalid}>{feeSummary}</span>
          <button type="button" class="btn small" onclick={() => savePrefs({ feeMinimized: false })}>Choose…</button>
        </div>
      {:else}
        <button type="button" class="btn ghost small" title="Hide transaction fee settings" onclick={() => savePrefs({ feeMinimized: true })}>Hide</button>
      {/if}
    </div>
    {#if !prefs.feeMinimized}
      <div class="panel-body fee-body">
        {#if smart?.fallback && prefs.feeMode === 'smart'}
          <p class="warn-box" role="alert">
            <strong>Warning: Fee estimation is currently not possible.</strong>
            Using the fallbackfee can result in sending a transaction that will take several hours or days (or never) to confirm. Consider choosing your fee manually or wait until you have validated the complete chain.
          </p>
        {/if}
        <label class="choice">
          <input type="radio" name="fee-mode" checked={prefs.feeMode === 'smart'} onchange={() => savePrefs({ feeMode: 'smart' })} />
          <span class="grow">
            <strong>Recommended:</strong> <span class="num">{smartLabel}</span>
            {#if smart?.fallback}<span class="muted">(Smart fee not initialized yet. This usually takes a few blocks…)</span>
            {:else if smart?.target}<span class="muted">Estimated to begin confirmation within {smart.target} block{smart.target === 1 ? '' : 's'}.</span>{/if}
          </span>
        </label>
        <div class="field target" class:off={prefs.feeMode !== 'smart'}>
          <label for="conf-target">Confirmation time target</label>
          <select id="conf-target" disabled={prefs.feeMode !== 'smart'} value={confTarget} onchange={(ev) => savePrefs({ confTarget: Number((ev.currentTarget as HTMLSelectElement).value) })}>
            {#each CONF_TARGETS as t (t)}
              <option value={t}>{niceTimeOffset(t * spacing)} ({t} blocks)</option>
            {/each}
          </select>
        </div>
        <label class="choice">
          <input type="radio" name="fee-mode" checked={prefs.feeMode === 'custom'} onchange={() => savePrefs({ feeMode: 'custom' })} />
          <span><strong>Custom:</strong> per kilobyte</span>
        </label>
        <div class="custom" class:off={prefs.feeMode !== 'custom'}>
          <AmountInput id="custom-fee" label="Fee per 1000 virtual bytes" bind:value={customFee} disabled={prefs.feeMode !== 'custom'} min={requiredFee} minError={`At least the minimum fee of ${formatAmount(requiredFee, prefs.unit, { withUnit: true })}/kvB`} />
          <p class="hint" title="When there is less transaction volume than space in the blocks, miners as well as relaying nodes may enforce a minimum fee. Paying only this minimum fee is just fine, but be aware that this can result in a never confirming transaction once there is more demand for transactions than the network can process.">
            A too low fee might result in a never confirming transaction (read the tooltip)
          </p>
        </div>
      </div>
    {/if}
  </section>

  <footer class="actions panel">
    <button type="button" class="btn primary" disabled={busy || !info || signerMissing || (!canSend && !info.privateKeysDisabled)} onclick={send} title={sendTip}>
      <Icon name="send" size={17} /> {busy ? 'Working…' : sendLabel}
    </button>
    <button type="button" class="btn" onclick={clearAll} title="Clear all fields of the form.">Clear all</button>
    <button type="button" class="btn" onclick={addEntry} title="Send to multiple recipients at once"><Icon name="plus" size={16} /> Add recipient</button>
    {#if signerMissing}<span class="hint warn" id="signer-hint">Set external signer script path in Options -> Wallet</span>{/if}
    <span class="grow"></span>
    <span class="balance">{info?.externalSigner ? 'External balance:' : 'Balance:'} <strong><Amount sats={balance} /></strong></span>
  </footer>
</div>


<Dialog bind:open={uriDialog} title="Open URI">
  <form
    id="uri-form"
    class="stack"
    onsubmit={(e) => {
      e.preventDefault();
      openUri();
    }}
  >
    <p>Open payment request from URI or file</p>
    <div class="field">
      <label for="uri-input">URI</label>
      <input id="uri-input" type="text" class="mono" placeholder="{CHAIN.uriScheme}:" bind:value={uriText} autocomplete="off" spellcheck="false" aria-invalid={uriError ? 'true' : undefined} />
      {#if uriError}<span class="error">{uriError}</span>{/if}
    </div>
  </form>
  {#snippet actions()}
    <button type="button" class="btn" onclick={() => (uriDialog = false)}>Cancel</button>
    <button type="submit" form="uri-form" class="btn primary">OK</button>
  {/snippet}
</Dialog>

<style>
  .send {
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
    max-width: 68rem;
  }
  .out-of-sync {
    display: flex;
    gap: 0.5rem;
    align-items: flex-start;
    padding: 0.6rem 0.8rem;
    border: 1px solid var(--warning);
    border-radius: var(--radius-m);
    background: var(--warning-soft);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .entries {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
  }
  .entry {
    position: relative;
    padding: 1rem 3rem 1rem 1.1rem;
  }
  .entry-grid {
    display: grid;
    grid-template-columns: minmax(0, 1fr);
    gap: 0.75rem;
  }
  .amount-row {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-end;
    gap: 0.8rem;
  }
  .amount-row .grow {
    min-width: 14rem;
    max-width: 22rem;
  }
  .amount-row .check {
    padding-bottom: 0.5rem;
  }
  .remove {
    position: absolute;
    top: 0.7rem;
    right: 0.7rem;
  }
  .message {
    margin: 0;
    color: var(--text-muted);
  }
  .cc-body {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
  }
  .auto {
    margin: 0;
  }
  .change {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.6rem;
  }
  .change-label {
    white-space: nowrap;
    font-size: 0.9rem;
  }
  .fee-body {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
  }
  .choice {
    display: flex;
    gap: 0.55rem;
    align-items: baseline;
    cursor: pointer;
  }
  .target,
  .custom {
    margin-left: 1.6rem;
    max-width: 24rem;
  }
  .off {
    opacity: 0.55;
  }
  .warn {
    color: var(--warning);
  }
  .warn-box {
    margin: 0;
    padding: 0.6rem 0.8rem;
    border-radius: var(--radius-s);
    background: var(--warning-soft);
    font-size: 0.92rem;
  }
  .hint {
    color: var(--text-muted);
    font-size: 0.86rem;
    margin: 0.3rem 0 0;
  }
  .actions {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.6rem;
    padding: 0.8rem 1rem;
    position: sticky;
    bottom: 0;
    z-index: 2;
  }
  .balance {
    color: var(--text-muted);
  }
  .balance strong {
    color: var(--text);
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
  }
</style>
