<script lang="ts">
  // What zSide sends beside the kit's form, at the top of the Send page (pages/Send.svelte): in Simple mode
  // a "Send privately" switch; on, the private form (zs/SendPrivately.svelte, the same sendshielded call as
  // Private → Pay) takes the place of the kit's form. Advanced keeps the kit's form; Private → Pay pays privately.
  import Term from '../components/Term.svelte';
  import { simpleMode } from '../lib/uimode.svelte';
  import SendPrivately from './zs/SendPrivately.svelte';
  import { startZs, zs } from './zs/store.svelte';
  import './zs/zs.css';

  let { active = $bindable(false) }: { active?: boolean } = $props();
  $effect(() => {
    if (simpleMode()) startZs();
  });
  let privately = $state(false);
  const shown = $derived(simpleMode() && !zs.unlockNeeded);
  $effect(() => {
    active = shown && privately;
  });
</script>

{#if shown}
  <div class="zs zs-send-switch">
    <label class="check switch-row">
      <input type="checkbox" role="switch" aria-checked={privately} bind:checked={privately} />
      <span><strong>Send privately</strong> <span class="muted">from your <Term id="shielded">private</Term> balance: nobody but the recipient sees the amount or that you paid.</span></span>
    </label>
  </div>
  {#if privately}<SendPrivately />{/if}
{/if}

<style>
  .zs-send-switch {
    margin: 0 0 0.9rem;
    padding: 0.7rem 0.9rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface);
  }
  .switch-row {
    display: flex;
    align-items: flex-start;
    gap: 0.6rem;
  }
  .switch-row input {
    margin-top: 0.2rem;
  }
</style>
