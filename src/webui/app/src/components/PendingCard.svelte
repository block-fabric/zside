<script lang="ts">
  // The progress of something the app does in several blocks (lib/pending.svelte.ts): a name or an asset reserved,
  // then registered by the app by itself once the reservation is deep enough. "Reserved ✓ → registering by itself
  // in ~45 s (block 1 of 3) → Registered ✓"; what the user can do (confirm, try again) when the app needs it. Simple
  // mode says the same in a line and a bar.
  import Icon from './Icon.svelte';
  import SafeText from './SafeText.svelte';
  import { t } from '../lib/i18n.svelte';
  import { blockSeconds, etaSeconds, etaText, progressOf, type PendingOp } from '../lib/pending.svelte';
  import { simpleMode } from '../lib/uimode.svelte';

  let { op, compact = false }: { op: PendingOp; compact?: boolean } = $props();

  const simple = $derived(simpleMode());
  // Counted down between blocks, from the time the app saw the last one.
  let now = $state(Date.now());
  $effect(() => {
    const id = setInterval(() => (now = Date.now()), 1000);
    return () => clearInterval(id);
  });
  let since = $state(Date.now());
  $effect(() => {
    // A new depth (a block): the count starts again from it.
    void op.depth;
    void op.stage;
    since = Date.now();
  });
  const eta = $derived.by(() => {
    const full = etaSeconds(op, blockSeconds());
    return full === null ? null : Math.max(1, full - (now - since) / 1000);
  });
  const progress = $derived(progressOf(op));
  const block = $derived(Math.min(op.needed, Math.max(1, (op.depth ?? 0) + 1)));
  const status = $derived.by(() => {
    switch (op.stage) {
      case 'reserved':
        return t('Reserving: waiting for the first block');
      case 'waiting':
        return t('Registering by itself in %1 (block %2 of %3)', etaText(eta), String(block), String(op.needed));
      case 'confirm':
        return t('Ready to register: it needs your confirmation');
      case 'registering':
        return t('Registration sent: in the next block (%1)', etaText(eta));
      case 'done':
        return t('Registered');
      default:
        return t('Not registered');
    }
  });
  const icon = $derived(op.stage === 'done' ? 'check' : op.stage === 'failed' ? 'alert' : op.stage === 'confirm' ? 'key' : 'clock');
</script>

<section class="pending-card" class:compact class:simple class:done={op.stage === 'done'} class:failed={op.stage === 'failed'} data-stage={op.stage} aria-label={t('%1 %2: %3', op.what, op.name, status)}>
  <header>
    <span class="ic" aria-hidden="true"><Icon name={icon} size={compact ? 14 : 16} /></span>
    <span class="title"><span class="what">{op.what}</span> <strong><SafeText text={op.name} /></strong></span>
    <span class="status" role="status">{status}</span>
  </header>
  {#if !simple && !compact}
    <ol class="steps">
      <li class:ok={op.stage !== 'reserved' && op.stage !== 'failed'} class:now={op.stage === 'reserved'}>{op.stage === 'reserved' ? 'Reserving' : 'Reserved'}{#if op.stage !== 'reserved' && op.stage !== 'failed'}{' ✓'}{/if}</li>
      <li class:ok={op.stage === 'done'} class:now={op.stage === 'waiting' || op.stage === 'confirm' || op.stage === 'registering'}>
        {#if op.stage === 'waiting'}Registering by itself · block {block} of {op.needed}{:else if op.stage === 'registering'}Registering · next block{:else if op.stage === 'confirm'}Registering · your confirmation{:else}Registering{/if}
      </li>
      <li class:ok={op.stage === 'done'}>Registered{#if op.stage === 'done'}{' ✓'}{/if}</li>
    </ol>
  {/if}
  {#if op.stage !== 'done' && op.stage !== 'failed'}
    <div class="bar" role="progressbar" aria-valuemin="0" aria-valuemax="100" aria-valuenow={Math.round(progress * 100)} aria-label="Progress"><span style:width="{Math.round(progress * 100)}%"></span></div>
  {/if}
  {#if op.error}<p class="error"><SafeText text={op.error} /></p>{/if}
  {#if op.action}
    <div class="row"><button type="button" class="btn small" class:primary={op.stage === 'confirm'} onclick={op.action.run}>{t(op.action.label)}</button></div>
  {/if}
</section>

<style>
  .pending-card {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    padding: 0.75rem 0.9rem;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-l);
    background: var(--surface-2);
  }
  .pending-card.compact {
    padding: 0.5rem 0.6rem;
    gap: 0.35rem;
  }
  header {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.45rem 0.6rem;
  }
  .ic {
    display: inline-flex;
    color: var(--accent);
  }
  .done .ic {
    color: var(--positive);
  }
  .failed .ic {
    color: var(--negative);
  }
  .title {
    min-width: 0;
    overflow-wrap: anywhere;
  }
  .what {
    color: var(--text-muted);
  }
  .status {
    margin-left: auto;
    color: var(--text-muted);
    font-size: 0.88rem;
  }
  .compact .status {
    margin-left: 0;
    flex-basis: 100%;
    font-size: 0.82rem;
  }
  .steps {
    display: flex;
    flex-wrap: wrap;
    gap: 0.3rem 1.4rem;
    margin: 0;
    padding: 0;
    list-style: none;
    font-size: 0.86rem;
    color: var(--text-faint);
  }
  .steps li {
    position: relative;
  }
  .steps li + li::before {
    content: '→';
    position: absolute;
    left: -1rem;
    color: var(--text-faint);
  }
  .steps li.ok {
    color: var(--text);
  }
  .steps li.now {
    color: var(--accent);
    font-weight: 600;
  }
  .bar {
    height: 0.35rem;
    border-radius: 999px;
    background: var(--raised);
    overflow: hidden;
  }
  .bar span {
    display: block;
    height: 100%;
    border-radius: inherit;
    background: var(--accent);
    transition: width 0.6s ease;
  }
  .error {
    margin: 0;
    color: var(--negative);
    font-size: 0.86rem;
  }
  .row {
    display: flex;
    gap: 0.5rem;
  }
</style>
