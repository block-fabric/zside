<script lang="ts">
  // The indicator of the top bars: what the app is doing in several blocks (lib/pending.svelte.ts), a name or an
  // asset being registered, on whichever page the user is; a moment more once it is done or failed. Its list opens
  // the page of each card.
  import Icon from './Icon.svelte';
  import PendingCard from './PendingCard.svelte';
  import { t } from '../lib/i18n.svelte';
  import { activeOps, blockSeconds, etaSeconds, etaText, openOp, pendingList, type PendingOp } from '../lib/pending.svelte';

  let open = $state(false);
  let root: HTMLElement | undefined = $state();
  let now = $state(Date.now());
  $effect(() => {
    const id = setInterval(() => (now = Date.now()), 5000);
    return () => clearInterval(id);
  });
  const ops = $derived(activeOps(now, pendingList()));
  const going = $derived(ops.filter((o) => o.stage !== 'done' && o.stage !== 'failed'));
  const needs = $derived(ops.some((o) => o.stage === 'confirm' || o.stage === 'failed'));
  const soonest = $derived.by(() => {
    const etas = going.map((o) => etaSeconds(o, blockSeconds())).filter((e): e is number => e !== null);
    return etas.length ? Math.min(...etas) : null;
  });
  const label = $derived(
    going.length
      ? `${going.length === 1 ? '1 registration' : `${going.length} registrations`} under way${soonest !== null ? `, ${etaText(soonest)}` : ''}`
      : ops.some((o) => o.stage === 'failed')
        ? 'A registration failed'
        : 'Registered',
  );
  function visit(op: PendingOp) {
    open = false;
    openOp(op);
  }
</script>

<svelte:window
  onmousedown={(e) => {
    if (open && root && !root.contains(e.target as Node)) open = false;
  }}
  onkeydown={(e) => {
    if (open && e.key === 'Escape') {
      open = false;
      root?.querySelector<HTMLElement>('.pending-pill')?.focus();
    }
  }}
/>

{#if ops.length}
  <div class="pending-ind" bind:this={root}>
    <button type="button" class="btn pending-pill" class:needs aria-label={label} title={label} aria-expanded={open} aria-haspopup="dialog" onclick={() => (open = !open)}>
      <span class="spin" aria-hidden="true"><Icon name={going.length ? 'clock' : needs ? 'alert' : 'check'} size={15} /></span>
      <span class="count">{going.length || ops.length}</span>
      {#if soonest !== null}<span class="eta">{t('%1 left', etaText(soonest))}</span>{/if}
    </button>
    {#if open}
      <div class="pop" role="dialog" aria-label="Registrations">
        <h2>Registrations</h2>
        <ul>
          {#each ops as op (op.key)}
            <li>
              <PendingCard {op} compact />
              {#if op.page}<button type="button" class="btn ghost small" onclick={() => visit(op)}>Open</button>{/if}
            </li>
          {/each}
        </ul>
      </div>
    {/if}
  </div>
{/if}

<style>
  .pending-ind {
    position: relative;
  }
  .pending-pill {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
    height: 2.1rem;
    padding: 0 0.7rem;
    border-radius: 999px;
    font-size: 0.85rem;
  }
  .pending-pill.needs {
    border-color: var(--accent);
  }
  .count {
    font-weight: 600;
  }
  .eta {
    color: var(--text-muted);
  }
  .spin {
    display: inline-flex;
    color: var(--accent);
  }
  .pop {
    position: absolute;
    z-index: 45;
    top: calc(100% + 0.5rem);
    right: 0;
    width: min(24rem, 90vw);
    padding: 0.6rem;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-l);
    background: var(--surface);
    box-shadow: var(--shadow);
  }
  h2 {
    margin: 0 0 0.5rem;
    font-size: 0.95rem;
  }
  ul {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    margin: 0;
    padding: 0;
    list-style: none;
    max-height: 24rem;
    overflow-y: auto;
  }
  li {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    gap: 0.25rem;
  }
  li :global(.pending-card) {
    width: 100%;
  }
</style>
