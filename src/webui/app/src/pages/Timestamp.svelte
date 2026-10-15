<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  // Timestamp a file (TimestampDialog of chains-qt): its fingerprint published in a transaction, and
  // found again in the last blocks. The file stays on this computer.
  import CopyButton from '../components/CopyButton.svelte';
  import Icon from '../components/Icon.svelte';
  import { confirm, errorText, inform, walletRpc } from '../lib/dialogs.svelte';
  import { chooseFile, FILTERS } from '../lib/files';
  import { formatBytes, formatCount, formatTime } from '../lib/format';
  import { host, rpc, type Stamp } from '../lib/rpc';
  import { noWalletText, wallets } from '../lib/state.svelte';
  import { stampPayload } from '../lib/tools';

  // A tab of a page that holds it (Tools, Advanced transactions): without a title of its own.
  let { embedded = false }: { embedded?: boolean } = $props();

  /** Blocks looked at per call: the progress shows, and Stop is quick. */
  const STEP = 64;

  let path = $state('');
  let hash = $state('');
  let size = $state(0);
  let blocksToSearch = $state(100000);
  let result = $state('');
  let resultKind = $state<'info' | 'found' | 'none' | 'sent'>('info');
  let searching = $state<{ searched: number; total: number } | null>(null);
  let controller: AbortController | null = null;

  async function choose() {
    const chosen = await chooseFile({ title: 'Choose a file', filters: [FILTERS.all] });
    if (!chosen) return;
    path = chosen;
    hash = '';
    result = '';
    try {
      const r = await host.hashFile(chosen);
      hash = r.sha256;
      size = r.size;
    } catch (e) {
      result = errorText(e) || 'The file cannot be read.';
      resultKind = 'none';
    }
  }

  async function stamp() {
    if (!hash) {
      await inform(t('Timestamp File'), t('Choose a file first.'));
      return;
    }
    const wallet = wallets.current;
    if (wallet === null) {
      await inform(t('Timestamp File'), noWalletText(t('stamping costs a transaction fee.')));
      return;
    }
    if (!(await confirm(t('Timestamp File'), t('Publish the fingerprint of this file on the chain?')))) return;
    try {
      const r = await walletRpc<{ txid?: string; complete: boolean }>(wallet, 'send', [[{ data: stampPayload(hash) }]]);
      if (!r) return;
      if (!r.txid) {
        await inform(t('Timestamp File'), t('The wallet could not sign the transaction.'), 'warning');
        return;
      }
      result = `The fingerprint was published in transaction\n${r.txid}\n\nThe stamp counts from the block that includes this transaction. Keep the file exactly as it is: any change gives it another fingerprint.`;
      resultKind = 'sent';
    } catch (e) {
      await inform(t('Timestamp File'), errorText(e), 'warning');
    }
  }

  async function find() {
    if (!hash) {
      await inform(t('Timestamp File'), t('Choose a file first.'));
      return;
    }
    controller = new AbortController();
    const total = Math.max(1, Math.floor(blocksToSearch));
    searching = { searched: 0, total };
    // The oldest stamp is the one that counts, so the search goes on after a find.
    let oldest: Stamp | null = null;
    try {
      let next: string | null = await rpc.call('getbestblockhash', []);
      while (next && searching.searched < total) {
        const r = await host.searchStamp(stampPayload(hash), next, Math.min(STEP, total - searching.searched), { signal: controller.signal });
        searching.searched += r.searched;
        if (r.found.length) oldest = r.found[r.found.length - 1];
        next = r.next;
        if (r.searched === 0) break;
      }
    } catch (e) {
      if (!controller.signal.aborted) {
        result = errorText(e);
        resultKind = 'none';
      }
    }
    const searched = searching.searched;
    searching = null;
    if (oldest) {
      result = `This file was stamped in block ${oldest.height}, mined on ${formatTime(oldest.time)}.\n\nTransaction: ${oldest.txid}\nBlock: ${oldest.block}`;
      resultKind = 'found';
    } else {
      result = `No stamp of this file was found in the last ${searched} blocks. Either it was never stamped, it was changed since, or its stamp is not mined yet.`;
      resultKind = 'none';
    }
  }
</script>

<div class="timestamp" class:page={!embedded}>
  {#if !embedded}<header class="page-head"><h1>Timestamp a file</h1></header>{/if}
  <p class="muted">
    Stamping a file publishes its fingerprint (SHA-256 hash) in a transaction, which proves to anyone, at any later time, that the file existed when the block was mined. The file itself stays on this computer.
    Stamping costs a transaction fee.
  </p>
  <section class="panel">
    <div class="panel-body form">
      <div class="field">
        <span class="label">File</span>
        <div class="row">
          <input id="timestamp-path" type="text" class="mono grow" readonly value={path} placeholder="No file chosen" aria-label="File" />
          <button type="button" class="btn" onclick={choose}><Icon name="folder" size={16} /> Choose file…</button>
        </div>
      </div>
      <div class="field">
        <span class="label">Fingerprint</span>
        <div class="row">
          <code id="timestamp-hash" class="hash break">{hash || '–'}</code>
          {#if hash}<CopyButton text={hash} label="Copy the fingerprint" /><span class="faint">{formatBytes(size)}</span>{/if}
        </div>
      </div>
      <div class="field narrow">
        <label for="timestamp-blocks">Blocks to search</label>
        <input id="timestamp-blocks" type="number" min="1" max="100000000" bind:value={blocksToSearch} />
        <span class="hint">How far back to look for the stamp of the file.</span>
      </div>
      <div class="row">
        <button type="button" class="btn primary" disabled={!hash || !!searching} onclick={stamp}>Stamp this file</button>
        <button type="button" class="btn" disabled={!hash || !!searching} onclick={find}>Find the stamp of this file</button>
        {#if searching}
          <progress max={searching.total} value={searching.searched} aria-label="Searching the chain"></progress>
          <span class="muted num">{formatCount(searching.searched)} / {formatCount(searching.total)}</span>
          <button type="button" class="btn small" onclick={() => controller?.abort()}>Stop</button>
        {/if}
      </div>
    </div>
  </section>
  {#if result}
    <section class="panel result {resultKind}" aria-live="polite">
      <div class="panel-body">
        <pre id="timestamp-result">{result}</pre>
      </div>
    </section>
  {/if}
</div>

<style>
  .form {
    display: flex;
    flex-direction: column;
    gap: 1.1rem;
    padding-top: 1.1rem;
  }
  .narrow {
    max-width: 18rem;
  }
  .hash {
    padding: 0.4rem 0.6rem;
    border-radius: var(--radius-s);
    background: var(--bg);
  }
  progress {
    width: 12rem;
  }
  .result {
    margin-top: 1rem;
    border-left: 4px solid var(--line-strong);
  }
  .result.found,
  .result.sent {
    border-left-color: var(--positive);
  }
  .result.none {
    border-left-color: var(--warning);
  }
  .result .panel-body {
    padding-top: 1rem;
  }
  pre {
    margin: 0;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    font-family: var(--font-ui);
  }
</style>
