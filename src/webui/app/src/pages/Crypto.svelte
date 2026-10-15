<script lang="ts">
  // Crypto tools (CryptoToolsDialog of chains-qt): a hash calculator, a merkle tree of transaction
  // ids (or of a block), and an address decoder.
  import { t } from '../lib/i18n.svelte';
  import CopyButton from '../components/CopyButton.svelte';
  import { errorText } from '../lib/dialogs.svelte';
  import { ownTab, setOwnTab } from '../lib/nav.svelte';
  import { host, rpc, type Decoded, type Hashes } from '../lib/rpc';

  // A tab of a page that holds it (Tools, Advanced transactions): without a title of its own.
  let { embedded = false }: { embedded?: boolean } = $props();

  const TABS = [
    { id: 'hash', label: 'Hash Calculator' },
    { id: 'merkle', label: 'Merkle Tree' },
    { id: 'address', label: 'Address Decoder' },
  ];
  const tab = $derived(TABS.some((t) => t.id === ownTab('crypto')) ? ownTab('crypto') : 'hash');

  // --- Hashes ---
  let hashInput = $state('');
  let hexInput = $state(false);
  let hashes = $state<Hashes | null>(null);
  $effect(() => {
    const text = hashInput;
    const hex = hexInput;
    const t = setTimeout(() => {
      host
        .hashes(text, hex)
        .then((h) => (hashes = h))
        .catch(() => (hashes = null));
    }, 80);
    return () => clearTimeout(t);
  });
  const HASH_ROWS: { key: keyof Hashes; label: string }[] = [
    { key: 'sha256', label: 'SHA-256' },
    { key: 'sha256d', label: 'SHA-256d' },
    { key: 'sha256dId', label: 'SHA-256d (as id)' },
    { key: 'ripemd160', label: 'RIPEMD-160' },
    { key: 'hash160', label: 'Hash160' },
    { key: 'sha512', label: 'SHA-512' },
    { key: 'sha1', label: 'SHA-1' },
  ];

  // --- Merkle tree ---
  let merkleBlock = $state('');
  let merkleInput = $state('');
  let levels = $state<string[][]>([]);
  let merkleError = $state('');
  let headerRoot = $state('');

  $effect(() => {
    const ids = merkleInput.split('\n').filter((l) => l.trim());
    const t = setTimeout(() => {
      if (ids.length === 0) {
        levels = [];
        merkleError = '';
        return;
      }
      host
        .merkle(ids)
        .then((r) => {
          levels = r.levels ?? [];
          merkleError = r.error ?? '';
        })
        .catch((e) => (merkleError = errorText(e)));
    }, 120);
    return () => clearTimeout(t);
  });

  async function loadBlock() {
    headerRoot = '';
    merkleError = '';
    try {
      let hash = merkleBlock.trim();
      if (/^\d+$/.test(hash)) hash = await rpc.call('getblockhash', [Number(hash)]);
      const block = await rpc.raw<{ tx: string[]; merkleroot: string }>('getblock', [hash, 1]);
      merkleInput = block.tx.join('\n');
      headerRoot = block.merkleroot;
    } catch (e) {
      merkleError = errorText(e);
    }
  }
  const root = $derived(levels.length ? levels[levels.length - 1][0] : '');

  // --- Address decoder ---
  let addressInput = $state('');
  let validation = $state<Record<string, unknown> | null>(null);
  let decoded = $state<Decoded | null>(null);
  let decodeError = $state('');

  async function decode() {
    const text = addressInput.trim();
    if (!text) return;
    decodeError = '';
    validation = null;
    decoded = null;
    try {
      [validation, decoded] = await Promise.all([rpc.raw<Record<string, unknown>>('validateaddress', [text]), host.decode(text)]);
    } catch (e) {
      decodeError = errorText(e);
    }
  }
  const validationRows = $derived(validation ? Object.entries(validation).filter(([k]) => k !== 'isvalid' && k !== 'address') : []);
</script>

<div class="crypto" class:page={!embedded}>
  {#if !embedded}<header class="page-head"><h1>Crypto tools</h1></header>{/if}
  <!-- Inside another page (Tools, Advanced transactions), its tabs are on that page's row. -->
  {#if !embedded}
  <div class="tabs" role="tablist" aria-label="Crypto tools">
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (setOwnTab('crypto', tb.id))}>{t(tb.label)}</button>
    {/each}
  </div>
  {/if}

  {#if tab === 'hash'}
    <section class="panel">
      <div class="panel-body form">
        <div class="field">
          <label for="hash-input">Data to hash</label>
          <textarea id="hash-input" rows="5" bind:value={hashInput}></textarea>
        </div>
        <label class="check"><input id="hash-hex" type="checkbox" bind:checked={hexInput} /> The data is hexadecimal (hash the bytes it stands for)</label>
        {#if hashes?.error}
          <p class="error" role="alert">{hashes.error}</p>
        {:else if hashes}
          <dl class="hashes" id="hash-output">
            {#each HASH_ROWS as row (row.key)}
              <dt>{t(row.label)}</dt>
              <dd><code class="break">{hashes[row.key]}</code><CopyButton text={String(hashes[row.key])} label="Copy {row.label}" /></dd>
            {/each}
          </dl>
          <p class="muted small">Bytes hashed: {hashes.bytes}. Block hashes and transaction ids are SHA-256d shown with the bytes reversed ("as id").</p>
        {/if}
      </div>
    </section>
  {:else if tab === 'merkle'}
    <section class="panel">
      <div class="panel-body form">
        <form class="row" onsubmit={(e) => { e.preventDefault(); loadBlock(); }}>
          <input id="merkle-block" type="text" class="mono grow" placeholder="Block height or hash" bind:value={merkleBlock} aria-label="Block height or hash" />
          <button type="submit" class="btn">Load transactions of block</button>
        </form>
        <div class="field">
          <label for="merkle-input">Transaction ids, one per line</label>
          <textarea id="merkle-input" class="mono" rows="6" bind:value={merkleInput}></textarea>
        </div>
        {#if merkleError}<p class="error" role="alert">{merkleError}</p>{/if}
        {#if root}
          <div class="root">
            <span class="muted">Merkle root</span>
            <code id="merkle-root" class="break">{root}</code>
            {#if headerRoot}
              <span class="match" class:ok={headerRoot === root}>{headerRoot === root ? 'matches the block header' : `the block header has ${headerRoot}`}</span>
            {/if}
          </div>
          <div class="tree" aria-label="Levels of the merkle tree">
            {#each [...levels].reverse() as level, i (i)}
              <div class="level">
                <span class="faint">{i === 0 ? 'Root' : levels.length - 1 - i === 0 ? `Level 0, the transaction ids (${level.length})` : `Level ${levels.length - 1 - i} (${level.length} hashes)`}</span>
                <div class="nodes">
                  {#each level as h, k (k)}<code title={h}>{h.slice(0, 10)}…</code>{/each}
                </div>
              </div>
            {/each}
          </div>
        {/if}
      </div>
    </section>
  {:else}
    <section class="panel">
      <div class="panel-body form">
        <form class="row" onsubmit={(e) => { e.preventDefault(); decode(); }}>
          <input id="decode-address" type="text" class="mono grow" placeholder="Address, or any Base58Check or Bech32 string" bind:value={addressInput} aria-label="Address" />
          <button type="submit" class="btn primary">Decode</button>
        </form>
        {#if decodeError}<p class="error" role="alert">{decodeError}</p>{/if}
        <div id="decode-output" class="decoded">
          {#if validation}
            {#if validation.isvalid}
              <p class="ok">A valid address of this network.</p>
              <dl class="facts">
                {#each validationRows as [k, v] (k)}<dt>{k}</dt><dd class="mono">{typeof v === 'string' ? v : JSON.stringify(v)}</dd>{/each}
              </dl>
            {:else}
              <p class="bad">Not a valid address of this network{validation.error ? `: ${validation.error}` : '.'}</p>
            {/if}
          {/if}
          {#if decoded?.base58}
            <h3>Base58Check encoding</h3>
            <dl class="facts">
              <dt>version byte</dt><dd class="mono">{decoded.base58.version} (0x{decoded.base58.version.toString(16).padStart(2, '0')})</dd>
              <dt>payload</dt><dd class="mono break">{decoded.base58.payload}</dd>
            </dl>
          {/if}
          {#if decoded?.bech32}
            <h3>{decoded.bech32.encoding === 'bech32m' ? 'Bech32m' : 'Bech32'} encoding</h3>
            <dl class="facts">
              <dt>prefix</dt><dd class="mono">{decoded.bech32.prefix}</dd>
              {#if decoded.bech32.witnessVersion !== undefined}<dt>witness version</dt><dd class="mono">{decoded.bech32.witnessVersion}</dd>{/if}
              {#if decoded.bech32.witnessProgram}<dt>witness program</dt><dd class="mono break">{decoded.bech32.witnessProgram}</dd>{/if}
            </dl>
          {/if}
        </div>
      </div>
    </section>
  {/if}
</div>

<style>
  .form {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    padding-top: 1.1rem;
  }
  .hashes {
    display: grid;
    grid-template-columns: max-content minmax(0, 1fr);
    gap: 0.4rem 1.2rem;
    margin: 0;
  }
  .hashes dt {
    color: var(--text-muted);
    font-weight: 550;
  }
  .hashes dd {
    margin: 0;
    display: flex;
    gap: 0.4rem;
    align-items: center;
  }
  .small {
    font-size: 0.86rem;
  }
  .error,
  .bad {
    color: var(--negative);
  }
  .ok {
    color: var(--positive);
    font-weight: 600;
  }
  .root {
    display: flex;
    gap: 0.8rem;
    align-items: baseline;
    flex-wrap: wrap;
  }
  .match {
    color: var(--negative);
    font-size: 0.9rem;
  }
  .match.ok {
    color: var(--positive);
  }
  .tree {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    max-height: 26rem;
    overflow: auto;
  }
  .level {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
  }
  .nodes {
    display: flex;
    flex-wrap: wrap;
    gap: 0.3rem;
    justify-content: center;
  }
  .nodes code {
    padding: 0.15rem 0.4rem;
    border: 1px solid var(--line);
    border-radius: 4px;
    background: var(--bg);
    font-size: 0.78rem;
  }
  .decoded h3 {
    margin: 1rem 0 0.4rem;
  }
</style>
