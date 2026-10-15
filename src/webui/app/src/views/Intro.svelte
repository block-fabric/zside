<script lang="ts">
  import SafeText from '../components/SafeText.svelte';
  import { CHAIN } from '../chain/config';
  // The first start: where the node keeps its data, and whether it keeps every block (intro.cpp).
  import { host, type IntroData, type Space } from '../lib/rpc';
  import { app } from '../lib/state.svelte';
  import { backupDays, pathMessage } from '../lib/intro';

  const data = $derived(app.phaseData as unknown as IntroData);
  const GB = 1_000_000_000;

  let useDefault = $state(true);
  let custom = $state('');
  let prune = $state(false);
  let pruneGB = $state(2);
  let space = $state<Space | null>(null);
  let error = $state('');
  let busy = $state(false);
  let pruneTouched = false;
  let initialised = false;

  $effect(() => {
    if (initialised || !data?.defaultDir) return;
    initialised = true;
    useDefault = data.dir === data.defaultDir;
    custom = data.dir;
    pruneGB = data.pruneGB;
    if (data.pruneForced !== undefined) prune = data.pruneForced;
  });

  const dir = $derived(useDefault ? data?.defaultDir ?? '' : custom.trim());
  const requiredGB = $derived(data ? (prune && pruneGB <= data.blockchainGB ? pruneGB + data.chainstateGB : data.blockchainGB + data.chainstateGB) : 0);
  const freeGB = $derived(space?.free !== undefined ? space.free / GB : null);

  // FreespaceChecker: what the path chosen is; Intro::UpdatePruneLabels: the backups the blocks kept restore.
  const pathState = $derived(pathMessage(space));
  const days = $derived(backupDays(Math.max(data?.minPruneGB ?? 1, pruneGB || 0)));

  $effect(() => {
    const d = dir;
    if (!d) {
      space = null;
      return;
    }
    const timer = setTimeout(async () => {
      try {
        space = await host.introSpace(d);
        // As the Qt intro: prune by default when the disk is short of room for the whole chain.
        if (!pruneTouched && data && data.pruneForced === undefined && space.free !== undefined) {
          prune = space.free < (data.blockchainGB + data.chainstateGB + 10) * GB;
        }
      } catch {
        space = null;
      }
    }, 250);
    return () => clearTimeout(timer);
  });

  async function start() {
    error = '';
    if (!dir) {
      error = 'Choose a data directory';
      return;
    }
    busy = true;
    try {
      await host.introChoose(dir, prune ? Math.max(data.minPruneGB, pruneGB) : 0);
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
      busy = false;
    }
  }
</script>

<main class="intro">
  <div class="card">
    <h1>Welcome to {app.info?.name ?? CHAIN.name}</h1>
    <p class="lead">
      This is the first start of the app{app.info?.networkLabel ? ` on ${app.info.networkLabel}` : ''}. It will download and store a copy of
      the {CHAIN.name} block chain; the wallet is stored with it. Choose where.
    </p>

    <fieldset>
      <legend class="sr-only">Data directory</legend>
      <label class="check">
        <input type="radio" name="dir" checked={useDefault} onchange={() => (useDefault = true)} />
        <span>Use the default data directory</span>
      </label>
      <div class="path mono break">{data?.defaultDir}</div>
      <label class="check">
        <input type="radio" name="dir" checked={!useDefault} onchange={() => (useDefault = false)} />
        <span>Use a custom data directory</span>
      </label>
      <input type="text" class="mono" aria-label="Custom data directory" disabled={useDefault} bind:value={custom} spellcheck="false" />
    </fieldset>

    <div class="space">
      {#if pathState}<p class:error={pathState.error} class:muted={!pathState.error} id="intro-path"><SafeText text={pathState.text} /></p>{/if}
      {#if space?.error}
        <!-- No room to tell. -->
      {:else if freeGB !== null}
        <p class:low={freeGB < requiredGB}>
          {freeGB.toFixed(0)} GB of free space available{space?.exists ? '' : ' (the folder will be made)'}.
          {#if prune && data && pruneGB <= data.blockchainGB}
            Approximately {requiredGB} GB of data will be stored in this directory.
          {:else}
            At least {requiredGB} GB of data will be stored in this directory, and it will grow over time.
          {/if}
        </p>
      {/if}
    </div>

    <label class="check">
      <input type="checkbox" bind:checked={prune} disabled={data?.pruneForced !== undefined} onchange={() => (pruneTouched = true)} />
      <span>Limit block chain storage to</span>
      <input type="number" class="gb" min={data?.minPruneGB ?? 1} bind:value={pruneGB} disabled={!prune || data?.pruneForced !== undefined} aria-label="Storage limit in GB" />
      <span>GB</span>
      <span class="muted" id="intro-backup-days">(sufficient to restore backups {days} day{days === 1 ? '' : 's'} old)</span>
    </label>
    {#if prune}
      <p class="hint">Old blocks are deleted once they are verified. Reverting this setting requires downloading the whole chain again.</p>
    {/if}

    {#if error}<p class="error" role="alert">{error}</p>{/if}

    <div class="actions">
      <button type="button" class="btn" onclick={() => host.introCancel()}>Cancel</button>
      <button type="button" class="btn primary" disabled={busy || !dir || !!pathState?.error} onclick={start}>Start</button>
    </div>
  </div>
</main>

<style>
  .intro {
    display: grid;
    place-items: center;
    height: 100%;
    padding: 1.5rem;
    overflow: auto;
  }
  .card {
    width: min(40rem, 100%);
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
    padding: 2rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-l);
    background: var(--surface);
    box-shadow: var(--shadow);
  }
  .lead {
    color: var(--text-muted);
  }
  fieldset {
    display: flex;
    flex-direction: column;
    gap: 0.45rem;
    margin: 0;
    padding: 0;
    border: 0;
  }
  .path {
    margin-left: 1.5rem;
    color: var(--text-muted);
  }
  .low {
    color: var(--warning);
  }
  .gb {
    width: 5.5rem;
  }
  .hint {
    color: var(--text-muted);
    font-size: 0.9rem;
  }
  .error {
    color: var(--negative);
  }
  .actions {
    display: flex;
    justify-content: flex-end;
    gap: 0.5rem;
    margin-top: 0.5rem;
  }
</style>
