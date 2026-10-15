<script lang="ts">
  import SafeText from '../components/SafeText.svelte';
  // An error the app cannot go on after: what happened, and quit.
  import { app } from '../lib/state.svelte';
  import { host } from '../lib/rpc';

  const data = $derived(app.phaseData as { title?: string; message?: string; details?: string[] });
</script>

<main class="fatal">
  <div class="card" role="alertdialog" aria-labelledby="fatal-title" aria-describedby="fatal-message">
    <h1 id="fatal-title">{#if data.title}<SafeText text={data.title} />{:else}Error{/if}</h1>
    <p id="fatal-message" class="break"><SafeText text={data.message ?? ''} /></p>
    {#if data.details?.length}
      <ul class="details">
        {#each data.details as d, i (i)}<li class="mono break">{d}</li>{/each}
      </ul>
    {/if}
    {#if app.info?.bugreport}<p class="muted">If this is a bug, report it at {app.info.bugreport}</p>{/if}
    <div class="actions">
      <button type="button" class="btn primary" onclick={() => host.quit()}>Quit</button>
    </div>
  </div>
</main>

<style>
  .fatal {
    display: grid;
    place-items: center;
    height: 100%;
    padding: 1.5rem;
  }
  .card {
    width: min(38rem, 100%);
    padding: 2rem;
    border: 1px solid var(--negative);
    border-radius: var(--radius-l);
    background: var(--surface);
  }
  h1 {
    color: var(--negative);
    margin-bottom: 0.8rem;
  }
  .details {
    padding-left: 1.2rem;
    color: var(--text-muted);
  }
  .actions {
    display: flex;
    justify-content: flex-end;
  }
</style>
