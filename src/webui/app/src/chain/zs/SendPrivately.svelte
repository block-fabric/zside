<script lang="ts">
  // Simple mode's private payment, on the Send page (chain/LocalSend.svelte): one recipient, an amount and a
  // memo for a private address, out of the private (shielded) balance. The same call and confirmation as
  // Private → Pay: sendshielded [{address: amount}, memo], proved in the node.
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import Icon from '../../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { shortHash } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { prefs, toast, wallets } from '../../lib/state.svelte';
  import { coinsToSats, formatAmount, satsToCoins } from '../../lib/units';
  import { MEMO_BYTES, memoBytes } from './model';
  import Privacy from './Privacy.svelte';
  import ProvingPanel from './ProvingPanel.svelte';
  import { prove, proving, zs } from './store.svelte';
  import ZAddressInput from './ZAddressInput.svelte';

  const available = $derived(coinsToSats(zs.balance?.available ?? 0));
  const busy = $derived(proving.now !== null);
  let address = $state('');
  let valid = $state(false);
  let kind = $state<'shielded' | 'transparent' | ''>('');
  let amount = $state<bigint | null>(null);
  let memo = $state('');
  let formKey = $state(0);
  const problem = $derived.by(() => {
    if (amount !== null && amount > available) return t('More than your private balance (%1).', formatAmount(available, prefs.unit));
    if (memoBytes(memo) > MEMO_BYTES) return t('The memo is longer than %1 bytes.', String(MEMO_BYTES));
    return '';
  });
  const ready = $derived(valid && amount !== null && amount > 0n && !problem && !busy);

  async function send() {
    const wallet = wallets.current;
    if (wallet === null || !ready || amount === null) return;
    const toPublic = kind === 'transparent';
    const r = await ask({
      title: t('Send privately'),
      message: toPublic
        ? t('Pay out of the shielded balance? The amounts and the transparent addresses will be public; where the coins came from will not.')
        : t('Pay out of the shielded balance, privately? Only the recipients see the amounts and the memo.'),
      code: `${formatAmount(amount, prefs.unit)} ${t('to')} ${address.trim()}`,
      level: toPublic ? 'warning' : 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Pay', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    const params: unknown[] = [{ [address.trim()]: satsToCoins(amount) }, toPublic ? '' : memo];
    try {
      const done = await prove('pay', 'Proving the payment', () => walletRpc<{ txid: string }>(wallet, 'sendshielded', params));
      if (!done) return;
      toast('success', t('Paid'), t('With the next block. Transaction %1.', shortHash(done.txid, 10)));
      address = '';
      amount = null;
      memo = '';
      formKey++;
    } catch (e) {
      await inform(t('Send privately'), errorText(e), 'error');
    }
  }
</script>

<section class="panel zs send-privately" aria-labelledby="zs-sp-title">
  <div class="panel-head"><h2 id="zs-sp-title">Send privately</h2><Privacy kind={kind === 'transparent' ? 'mixed' : 'private'} /></div>
  <div class="panel-body zs-form">
    <ProvingPanel />
    <p class="muted small">Private balance: <strong><Amount sats={available} /></strong></p>
    {#if available <= 0n}
      <div class="zs-note private">
        <Icon name="shield" size={18} />
        <div>
          <p>Your private balance is empty. Make some coins private first: it takes one block.</p>
          <button type="button" class="btn" onclick={() => go('private', 'move')}><Icon name="shield" size={16} /> Make private</button>
        </div>
      </div>
    {/if}
    {#key formKey}
      <ZAddressInput id="zs-sp-to" label="Pay to" bind:value={address} bind:valid bind:kind disabled={busy} />
      <AmountInput id="zs-sp-amount" label="Amount" bind:value={amount} max={available} disabled={busy} />
    {/key}
    {#if kind !== 'transparent'}
      <div class="field">
        <label for="zs-sp-memo">Message (optional)</label>
        <textarea id="zs-sp-memo" bind:value={memo} disabled={busy} placeholder="Only the recipient can read it"></textarea>
      </div>
    {:else}
      <div class="zs-note public"><Icon name="eye" size={18} /><span>This is a public address: the amount and the address will be visible on the chain. Where the coins came from stays private.</span></div>
    {/if}
    {#if problem}<p class="error" role="alert">{problem}</p>{/if}
    <div class="zs-actions">
      <button type="button" id="zs-sp-send" class="btn primary" disabled={!ready} onclick={send}><Icon name="private" size={16} /> {busy ? t('Proving…') : t('Send privately')}</button>
    </div>
  </div>
</section>
