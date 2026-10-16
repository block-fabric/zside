<script lang="ts">
  // Simple mode's Sidechains page: "Move coins". A wizard that moves CHN to a sidechain (which one,
  // how much, where it arrives, confirm), with the same checks and the same call as the deposit of
  // Advanced (createsidechaindeposit), and how the coins come back (from the sidechain's own app).
  // Slots, proposals and the votes on withdrawals stay in Advanced.
  import { t } from '../lib/i18n.svelte';
  import Amount from '../components/Amount.svelte';
  import AmountInput from '../components/AmountInput.svelte';
  import Icon from '../components/Icon.svelte';
  import SafeText from '../components/SafeText.svelte';
  import Term from '../components/Term.svelte';
  import CopyButton from '../components/CopyButton.svelte';
  import { css } from '../lib/actions';
  import type { DrivechainState } from '../lib/chain';
  import { errorText, inform, walletRpc } from '../lib/dialogs.svelte';
  import { niceTimeOffset } from '../lib/format';
  import { go } from '../lib/nav.svelte';
  import { app, noWalletText, prefs, wallets } from '../lib/state.svelte';
  import { host } from '../lib/rpc';
  import { formatAmount, satsToCoins } from '../lib/units';
  import { setUiMode } from '../lib/uimode.svelte';

  let { dc, refresh }: { dc: DrivechainState | null; refresh: () => void } = $props();

  const STEPS = ['Sidechain', 'Amount', 'Destination', 'Confirm'] as const;
  let step = $state(0);
  let slot = $state<number | null>(null);
  let amount = $state<bigint | null>(null);
  let amountKey = $state(0);
  let destination = $state('');
  let check = $state<{ kind: 'plain' | 'valid' | 'invalid'; slot?: number } | null>(null);
  let sending = $state(false);
  let done = $state<{ txid: string; amount: bigint; title: string } | null>(null);

  const sidechains = $derived(dc?.sidechains ?? []);
  const chosen = $derived(sidechains.find((s) => s.slot === slot) ?? null);
  const available = $derived(wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null);
  const spacing = $derived(app.info?.blockSpacing ?? 60);

  /** A colour of its own for each sidechain's mark, from its slot. */
  const hue = (s: number) => (s * 67 + 200) % 360;
  const initial = (title: string) => (title.trim()[0] ?? '?').toUpperCase();

  // The deposit address says which sidechain it is for.
  $effect(() => {
    const text = destination.trim();
    if (!text) {
      check = null;
      return;
    }
    const timer = setTimeout(async () => {
      try {
        check = await host.depositAddress(text);
      } catch {
        check = null;
      }
    }, 200);
    return () => clearTimeout(timer);
  });
  const destOk = $derived(check?.kind === 'valid' && check.slot === slot);
  const destMessage = $derived.by(() => {
    if (!check || !chosen) return '';
    if (check.kind === 'invalid') return t('This address has a typing error in it. Copy it again from the app of %1.', chosen.title);
    if (check.kind === 'plain') return t('This is not a deposit address. In the app of %1, open Drivechains, then Deposit, and copy the address shown there.', chosen.title);
    if (check.slot !== slot) return t('This deposit address is for another sidechain (slot %1), not %2.', check.slot, chosen.title);
    return t('A deposit address of %1.', chosen.title);
  });
  const amountOk = $derived(amount !== null && amount > 0n && (available === null || amount <= available));

  function choose(s: number) {
    slot = s;
    step = 1;
  }
  async function paste() {
    try {
      const text = await host.paste();
      if (text) destination = text.trim();
    } catch {
      // Nothing to paste: the field stays as it is.
    }
  }
  function restart() {
    step = 0;
    slot = null;
    amount = null;
    amountKey++;
    destination = '';
    done = null;
  }

  async function move() {
    const wallet = wallets.current;
    if (!chosen || amount === null || sending) return;
    if (wallet === null) {
      await inform(t('Move coins'), noWalletText(t('the coins are paid from it.')));
      return;
    }
    // Checked again now: the same checks as the deposit of Advanced, without its "deposit anyway".
    let again;
    try {
      again = await host.depositAddress(destination.trim());
    } catch (e) {
      await inform(t('Move coins'), errorText(e), 'error');
      return;
    }
    if (again.kind !== 'valid' || again.slot !== chosen.slot) {
      step = 2;
      return;
    }
    sending = true;
    try {
      const result = await walletRpc<{ txid: string }>(wallet, 'createsidechaindeposit', [chosen.slot, destination.trim(), satsToCoins(amount)]);
      if (!result) return;
      done = { txid: result.txid, amount, title: chosen.title };
      refresh();
    } catch (e) {
      await inform(t('Move coins'), errorText(e), 'error');
    } finally {
      sending = false;
    }
  }
</script>

<div class="move">
  <section class="panel wizard" aria-labelledby="move-title">
    <div class="panel-head"><h2 id="move-title">Move CHN to a sidechain</h2></div>
    <div class="panel-body">
      {#if done}
        <div class="done" role="status">
          <span class="big-check" aria-hidden="true"><Icon name="check" size={28} /></span>
          <div>
            <h3>{formatAmount(done.amount, prefs.unit, { withUnit: true })} are on their way to <SafeText text={done.title} /></h3>
            <p class="muted">They arrive in your wallet there after the next block of the sidechain, usually a minute or two. Its app shows them on its Drivechains page.</p>
            <p class="muted small">Transaction <span class="mono">{done.txid.slice(0, 16)}…</span> <CopyButton text={done.txid} label="Copy the transaction id" /></p>
            <button type="button" class="btn primary" onclick={restart}>Move more coins</button>
          </div>
        </div>
      {:else}
        <ol class="steps" aria-label="Steps">
          {#each STEPS as s, i (s)}
            <li class:current={i === step} class:past={i < step} aria-current={i === step ? 'step' : undefined}><span class="dot">{i < step ? '✓' : i + 1}</span> {t(s)}</li>
          {/each}
        </ol>

        {#if step === 0}
          <p class="lead">Which <Term id="sidechain">sidechain</Term> should the coins go to?</p>
          {#if !dc}
            <p class="muted">Loading the sidechains…</p>
          {:else if sidechains.length === 0}
            <p class="muted">No sidechain is active on this chain yet.</p>
          {:else}
            <div class="choices">
              {#each sidechains as s (s.slot)}
                <button type="button" class="choice" class:chosen={s.slot === slot} onclick={() => choose(s.slot)}>
                  <span class="mark" use:css={{ '--hue': hue(s.slot) }} aria-hidden="true">{initial(s.title)}</span>
                  <span class="choice-text">
                    <strong translate="no">{s.title}</strong>
                    <span class="muted small desc" translate="no">{s.description}</span>
                  </span>
                  <Icon name="chevron-right" size={16} />
                </button>
              {/each}
            </div>
          {/if}
        {:else if step === 1 && chosen}
          <p class="lead">How much CHN should go to <SafeText text={chosen.title} />?</p>
          {#key amountKey}<AmountInput id="move-amount" bind:value={amount} label="Amount" max={available ?? undefined} />{/key}
          {#if available !== null}<p class="muted small">You have <Amount sats={available} /> available.</p>{/if}
          <div class="nav">
            <button type="button" class="btn" onclick={() => (step = 0)}>Back</button>
            <button type="button" class="btn primary" disabled={!amountOk} onclick={() => (step = 2)}>Next</button>
          </div>
        {:else if step === 2 && chosen}
          <p class="lead">Where should it arrive on <SafeText text={chosen.title} />?</p>
          <ol class="how muted">
            <li>Open the app of <SafeText text={chosen.title} /> (start it from the Sidechain apps tab if it is not running).</li>
            <li>Go to its Drivechains page, then Deposit, and copy the <Term id="deposit">deposit</Term> address shown there.</li>
            <li>Paste it here.</li>
          </ol>
          <div class="field">
            <label for="move-destination">Deposit address</label>
            <div class="paste-row">
              <input id="move-destination" type="text" class="mono" maxlength="100" spellcheck="false" autocomplete="off" placeholder="s{chosen.slot}_…" bind:value={destination} aria-invalid={check && !destOk ? 'true' : undefined} aria-describedby="move-destination-check" />
              <button type="button" class="btn" onclick={paste}><Icon name="paste" size={15} /> Paste</button>
            </div>
            <span id="move-destination-check" class="hint" class:error={!!check && !destOk} class:ok={destOk}>{destMessage}</span>
          </div>
          <div class="nav">
            <button type="button" class="btn" onclick={() => (step = 1)}>Back</button>
            <button type="button" class="btn primary" disabled={!destOk} onclick={() => (step = 3)}>Next</button>
          </div>
        {:else if step === 3 && chosen && amount !== null}
          <p class="lead">Check, then move the coins.</p>
          <dl class="facts summary">
            <dt>To</dt><dd><SafeText text={chosen.title} /></dd>
            <dt>Amount</dt><dd><Amount sats={amount} /></dd>
            <dt>Arrives at</dt><dd class="mono break">{destination.trim()}</dd>
            <dt>When</dt><dd>After the next block of the sidechain, usually a minute or two.</dd>
          </dl>
          <p class="muted small">A small <Term id="fee">fee</Term> is added. Coming back takes longer: see “Bringing coins back”.</p>
          <div class="nav">
            <button type="button" class="btn" onclick={() => (step = 2)}>Back</button>
            <button type="button" class="btn primary" disabled={sending || wallets.current === null} onclick={move}>{sending ? 'Moving…' : t('Move %1', formatAmount(amount, prefs.unit, { withUnit: true }))}</button>
          </div>
          {#if wallets.current === null}<p class="muted">Open a wallet to pay from it.</p>{/if}
        {/if}
      {/if}
    </div>
  </section>

  <section class="panel back" aria-labelledby="back-title">
    <div class="panel-head"><h2 id="back-title">Bringing coins back</h2></div>
    <div class="panel-body">
      <ol class="how">
        <li>Open the app of the sidechain, and its <strong>Drivechains</strong> page.</li>
        <li>Choose <strong>Withdraw</strong>, and enter an address of this wallet (Receive gives you one) and the amount.</li>
        <li>The miners of Chains approve <Term id="withdrawal">withdrawals</Term> together, in a <Term id="bundle">bundle</Term>.
          {#if dc}That takes at least {niceTimeOffset(dc.info.withdrawalminscore * spacing)}; then the coins arrive here.{:else}That takes a while; then the coins arrive here.{/if}</li>
      </ol>
      <p class="muted small">Want to follow the votes on withdrawals, or the slots of sidechains? They are in <button type="button" class="link" onclick={() => setUiMode('advanced')}>Advanced mode</button>.</p>
      <button type="button" class="btn" onclick={() => go('sidechains', 'nodes')}><Icon name="nodes" size={15} /> Sidechain apps</button>
    </div>
  </section>
</div>

<style>
  .move {
    display: grid;
    grid-template-columns: minmax(0, 1.6fr) minmax(16rem, 1fr);
    gap: 1rem;
    align-items: start;
  }
  @media (max-width: 900px) {
    .move {
      grid-template-columns: minmax(0, 1fr);
    }
  }
  .wizard .panel-body,
  .back .panel-body {
    display: flex;
    flex-direction: column;
    gap: 0.8rem;
  }
  .lead {
    margin: 0;
    font-size: 1.05rem;
    font-weight: 600;
  }
  .steps {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem 1rem;
    margin: 0;
    padding: 0 0 0.6rem;
    border-bottom: 1px solid var(--line);
    list-style: none;
    color: var(--text-faint);
    font-size: 0.88rem;
  }
  .steps li {
    display: flex;
    align-items: center;
    gap: 0.35rem;
  }
  .steps .dot {
    display: inline-grid;
    place-items: center;
    width: 1.45rem;
    height: 1.45rem;
    border: 1px solid var(--line-strong, var(--line));
    border-radius: 50%;
    font-size: 0.78rem;
    font-weight: 700;
  }
  .steps .past {
    color: var(--text-muted);
  }
  .steps .current {
    color: var(--text);
    font-weight: 650;
  }
  .steps .current .dot {
    border-color: var(--accent);
    background: var(--accent);
    color: var(--on-accent);
  }
  .choices {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(15rem, 1fr));
    gap: 0.6rem;
  }
  .choice {
    display: flex;
    align-items: center;
    gap: 0.7rem;
    padding: 0.7rem 0.8rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .choice:hover,
  .choice.chosen {
    border-color: var(--accent);
    background: var(--raised);
  }
  .mark {
    display: inline-grid;
    place-items: center;
    flex: none;
    width: 2.4rem;
    height: 2.4rem;
    border-radius: 50%;
    background: hsl(var(--hue) 60% 45%);
    color: #fff;
    font-weight: 750;
    font-size: 1.1rem;
  }
  .choice-text {
    display: flex;
    flex-direction: column;
    flex: 1;
    min-width: 0;
  }
  .desc {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .nav {
    display: flex;
    gap: 0.5rem;
    justify-content: flex-end;
  }
  .how {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    margin: 0;
    padding-left: 1.3rem;
    line-height: 1.45;
  }
  .paste-row {
    display: flex;
    gap: 0.4rem;
  }
  .paste-row input {
    flex: 1;
    min-width: 0;
  }
  .field .hint.error {
    color: var(--negative);
  }
  .field .hint.ok {
    color: var(--positive);
  }
  .summary {
    margin: 0;
  }
  .done {
    display: flex;
    gap: 1rem;
    align-items: flex-start;
  }
  .done h3 {
    margin: 0 0 0.3rem;
  }
  .done p {
    margin: 0 0 0.6rem;
  }
  .big-check {
    display: inline-grid;
    place-items: center;
    flex: none;
    width: 3rem;
    height: 3rem;
    border-radius: 50%;
    background: var(--accent);
    color: var(--on-accent);
  }
</style>
