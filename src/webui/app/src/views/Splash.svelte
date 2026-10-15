<script lang="ts">
  // While the node starts (or stops): what it does now.
  import { app } from '../lib/state.svelte';
  import { CHAIN } from '../chain/config';
  import { host } from '../lib/rpc';

  let { stopping = false }: { stopping?: boolean } = $props();

  function onkeydown(e: KeyboardEvent) {
    // As the splash screen of chains-qt: q stops the start, to continue later.
    if (!stopping && (e.key === 'q' || e.key === 'Q')) host.quit();
  }
</script>

<svelte:window {onkeydown} />

<main class="splash" aria-busy="true">
  <svg class="mark" viewBox="0 0 64 32" aria-hidden="true">
    <rect class="a" x="4" y="6" width="20" height="20" rx="6" />
    <rect class="b" x="40" y="6" width="20" height="20" rx="6" />
    <path d="M24 16h16" />
  </svg>
  <h1>{app.info?.name ?? CHAIN.name}</h1>
  {#if app.info?.networkLabel}<span class="badge warning">{app.info.networkLabel}</span>{/if}
  {#if stopping}
    <p class="message" role="status">{app.info?.name ?? CHAIN.name} is shutting down…</p>
    <p class="hint">Do not shut down the computer until this window disappears.</p>
  {:else}
    <p class="message" role="status">{app.initMessage || 'Starting…'}</p>
    {#if app.progress}
      <progress max="100" value={app.progress.progress} aria-label={app.progress.title}></progress>
    {/if}
    <p class="hint">Press <kbd>q</kbd> to shut down and continue later.</p>
  {/if}
  {#if app.info}<p class="version faint">{app.info.version}</p>{/if}
</main>

<style>
  .splash {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 0.6rem;
    height: 100%;
    padding: 2rem;
    text-align: center;
  }
  .mark {
    width: 6rem;
    fill: none;
    stroke: var(--accent);
    stroke-width: 3;
  }
  .mark .b {
    animation: link 1.6s ease-in-out infinite alternate;
  }
  @keyframes link {
    from {
      opacity: 0.25;
    }
  }
  h1 {
    font-size: 2rem;
  }
  .message {
    min-height: 1.5em;
    color: var(--text-muted);
    max-width: 40rem;
  }
  .hint {
    color: var(--text-faint);
    font-size: 0.9rem;
  }
  progress {
    width: 16rem;
    accent-color: var(--accent);
  }
  .version {
    font-size: 0.8rem;
  }
</style>
