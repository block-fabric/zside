<script lang="ts">
  // Messages and questions of the node that wait for the user (errors at start, a reindex to confirm...).
  import SafeText from './SafeText.svelte';
  import Dialog from './Dialog.svelte';
  import { host } from '../lib/rpc';
  import { app } from '../lib/state.svelte';

  // Button flags of CClientUIInterface (node/interface_ui.h).
  const BTN_OK = 0x400;
  const BTN_YES = 0x4000;
  const BTN_CANCEL = 0x400000;

  const current = $derived(app.dialogs[0] ?? null);

  function answer(value: unknown) {
    const d = app.dialogs.shift();
    if (d?.id !== undefined) host.answer(d.id, value).catch(() => undefined);
  }

  const okLabel = $derived(current?.style !== undefined && current.style & BTN_YES ? 'Yes' : 'OK');
  const cancelLabel = $derived(current?.style !== undefined && current.style & BTN_CANCEL && !(current.style & BTN_YES) ? 'Cancel' : 'No');
</script>

{#if current}
  <Dialog open={true} title={current.title} dismissable={current.kind === 'message'} onclose={() => answer(current.kind === 'question' ? false : null)}>
    <div class="body" class:error={current.level === 'error'}>
      <p class="break"><SafeText text={current.message} /></p>
      {#if current.question}<p>{current.question}</p>{/if}
      {#if current.details?.length}
        <ul>
          {#each current.details as d, i (i)}<li class="mono break">{d}</li>{/each}
        </ul>
      {/if}
    </div>
    {#snippet actions()}
      {#if current.kind === 'question'}
        <button type="button" class="btn" onclick={() => answer(false)}>{cancelLabel}</button>
        <button type="button" class="btn primary" onclick={() => answer(true)}>{okLabel}</button>
      {:else if current.kind === 'settingsError'}
        <button type="button" class="btn" onclick={() => answer('abort')}>Abort</button>
        <button type="button" class="btn primary" onclick={() => answer('reset')}>Reset settings</button>
      {:else}
        <button type="button" class="btn primary" onclick={() => answer(null)}>OK</button>
      {/if}
    {/snippet}
  </Dialog>
{/if}

<style>
  .body.error p:first-child {
    color: var(--negative);
    font-weight: 550;
  }
  ul {
    padding-left: 1.2rem;
    color: var(--text-muted);
  }
</style>
