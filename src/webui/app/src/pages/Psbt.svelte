<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  import SafeText from '../components/SafeText.svelte';
  // Partially signed transactions (WalletFrame::gotoLoadPSBT, PSBTOperationsDialog): loaded from a
  // file, the clipboard or pasted; signed with the wallet; broadcast; copied or saved.
  import Amount from '../components/Amount.svelte';
  import Icon from '../components/Icon.svelte';
  import { errorText, withUnlock } from '../lib/dialogs.svelte';
  import { FILTERS, openFile, saveBase64As, safeFileName } from '../lib/files';
  import { ownTab, setOwnTab } from '../lib/nav.svelte';
  import { host } from '../lib/rpc';
  import { prefs, toast, wallets } from '../lib/state.svelte';
  import { formatAmount, UNITS } from '../lib/units';
  import { psbt as psbtApi, walletApi, type PsbtState } from '../lib/wallet';

  // A tab of a page that holds it (Tools, Advanced transactions): without a title of its own.
  let { embedded = false }: { embedded?: boolean } = $props();

  const wallet = $derived(wallets.current);
  let ps = $state<PsbtState | null>(null);
  let status = $state<{ text: string; level: string } | null>(null);
  let pasted = $state('');
  let error = $state('');
  let busy = $state(false);

  async function open(psbt: string) {
    error = '';
    try {
      ps = wallet !== null ? await walletApi(wallet).psbtOpen(psbt) : await psbtApi.open(psbt);
      status = ps.status;
    } catch (e) {
      ps = null;
      error = errorText(e);
    }
  }

  async function fromClipboard() {
    const text = await host.paste().catch(() => null);
    try {
      const { psbt } = await psbtApi.fromClipboardText(text ?? '');
      await open(psbt);
    } catch (e) {
      ps = null;
      error = errorText(e);
    }
  }

  async function fromFile() {
    try {
      const file = await openFile({ title: 'Load Transaction Data', filters: [FILTERS.psbt, FILTERS.all] });
      if (!file) return;
      const { psbt } = await psbtApi.fromFile(file.base64);
      await open(psbt);
    } catch (e) {
      ps = null;
      error = errorText(e);
    }
  }

  async function fromText() {
    try {
      const { psbt } = await psbtApi.fromClipboardText(pasted);
      await open(psbt);
    } catch (e) {
      ps = null;
      error = errorText(e);
    }
  }

  // From the menu: Load PSBT from file… / from clipboard…
  $effect(() => {
    const tab = ownTab('psbt');
    if (tab !== 'file' && tab !== 'clipboard') return;
    setOwnTab('psbt', '');
    if (tab === 'file') fromFile();
    else fromClipboard();
  });

  async function sign() {
    if (!ps || wallet === null) return;
    busy = true;
    try {
      const api = walletApi(wallet);
      const current = ps.psbt;
      const r = await withUnlock(wallet, (passphrase) => api.psbtSign(current, passphrase));
      if (!r) status = { text: t('Cannot sign inputs while wallet is locked.'), level: 'warning' };
      else {
        ps = r;
        status = r.status;
      }
    } catch (e) {
      status = { text: errorText(e), level: 'error' };
    } finally {
      busy = false;
    }
  }

  async function broadcast() {
    if (!ps) return;
    busy = true;
    try {
      const r = await psbtApi.broadcast(ps.psbt);
      status = { text: t('Transaction broadcast successfully! Transaction ID: %1', r.txid), level: 'success' };
      ps = { ...ps, canBroadcast: false, canSign: false };
    } catch (e) {
      status = { text: errorText(e), level: 'error' };
    } finally {
      busy = false;
    }
  }

  async function copy() {
    if (!ps) return;
    await host.copy(ps.psbt).catch(() => undefined);
    status = { text: t('PSBT copied to clipboard.'), level: 'info' };
  }

  async function save() {
    if (!ps) return;
    const name = ps.outputs.map((o) => `${o.address}-${formatAmount(BigInt(o.sats), prefs.unit, { separators: false })}`).join('-') + '.psbt';
    try {
      const path = await saveBase64As({ title: 'Save Transaction Data', name: safeFileName(name), filters: [FILTERS.psbt] }, ps.psbt);
      if (path) status = { text: 'PSBT saved to disk.', level: 'info' };
    } catch (e) {
      status = { text: errorText(e), level: 'error' };
    }
  }

  function alternatives(total: bigint): string {
    return UNITS.filter((u) => u.id !== prefs.unit)
      .map((u) => formatAmount(total, u.id, { withUnit: true }))
      .join(' or ');
  }
</script>

<div class="psbt" class:page={!embedded}>
  {#if !embedded}<header class="page-head"><h1>Partially signed transactions</h1></header>{/if}

  <section class="panel load">
    <div class="panel-body row loaders">
      <button type="button" class="btn" onclick={fromFile}><Icon name="upload" size={16} /> Load PSBT from file…</button>
      <button type="button" class="btn" onclick={fromClipboard}><Icon name="paste" size={16} /> Load PSBT from clipboard</button>
    </div>
    <form
      class="panel-body paste"
      onsubmit={(e) => {
        e.preventDefault();
        fromText();
      }}
    >
      <label for="psbt-text">Or paste one (base64)</label>
      <div class="row">
        <textarea id="psbt-text" class="mono grow" rows="2" spellcheck="false" placeholder="cHNidP8B…" bind:value={pasted}></textarea>
        <button type="submit" class="btn" disabled={!pasted.trim()}>Open</button>
      </div>
    </form>
    {#if error}<p class="panel-body error break" role="alert">{error}</p>{/if}
  </section>

  {#if ps}
    <section class="panel ops" aria-labelledby="ops-title">
      <div class="panel-head">
        <h2 id="ops-title">PSBT Operations</h2>
        <span class="badge" class:accent={ps.complete}>{ps.complete ? 'Complete' : `Next: ${ps.next}`}</span>
      </div>
      <div class="panel-body">
        <ul class="outputs">
          {#each ps.outputs as o, i (i)}
            <li>
              Sends <Amount sats={o.sats} /> to <span class="mono break">{o.address || '(no address)'}</span>
              {#if o.mine}<span class="badge accent">own address</span>{/if}
            </li>
          {/each}
          <li>
            {#if ps.fee === undefined}
              Unable to calculate transaction fee or total transaction amount.
            {:else}
              Pays transaction fee: <Amount sats={ps.fee} />
            {/if}
          </li>
        </ul>
        {#if ps.fee !== undefined}
          <p class="total">
            <strong>Total Amount: <Amount sats={ps.total} /></strong>
            <span class="muted">(= {alternatives(BigInt(ps.total))})</span>
          </p>
        {/if}
        {#if ps.unsignedInputs > 0}<p>Transaction has {ps.unsignedInputs} unsigned input{ps.unsignedInputs === 1 ? '' : 's'}.</p>{/if}
        {#if status}<p class="status {status.level}" role="status"><SafeText text={status.text} /></p>{/if}
        <div class="row buttons">
          <button type="button" class="btn primary" disabled={busy || !ps.canSign || wallet === null} onclick={sign}><Icon name="edit" size={16} /> Sign Tx</button>
          <button type="button" class="btn primary" disabled={busy || !ps.canBroadcast} onclick={broadcast}><Icon name="send" size={16} /> Broadcast Tx</button>
          <button type="button" class="btn" onclick={copy}><Icon name="copy" size={16} /> Copy to Clipboard</button>
          <button type="button" class="btn" onclick={save}><Icon name="download" size={16} /> Save…</button>
          <button
            type="button"
            class="btn ghost"
            onclick={() => {
              ps = null;
              status = null;
            }}>Close</button
          >
        </div>
      </div>
    </section>
  {/if}
</div>

<style>
  .psbt {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    max-width: 64rem;
  }
  .loaders {
    padding-top: 1rem;
    flex-wrap: wrap;
  }
  .paste {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
  }
  .paste label {
    font-weight: 550;
    font-size: 0.92rem;
  }
  textarea {
    min-height: 3rem;
  }
  .outputs {
    margin: 0 0 0.6rem;
    padding-left: 1.2rem;
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
  }
  .total {
    padding-top: 0.6rem;
    border-top: 1px solid var(--line);
  }
  .status {
    padding: 0.55rem 0.8rem;
    border-radius: var(--radius-s);
    font-weight: 550;
  }
  .status.info,
  .status.success {
    background: var(--accent-soft);
  }
  .status.success {
    color: var(--positive);
  }
  .status.warning {
    background: var(--warning-soft);
    color: var(--warning);
  }
  .status.error {
    background: var(--warning-soft);
    color: var(--negative);
  }
  .buttons {
    flex-wrap: wrap;
  }
</style>
