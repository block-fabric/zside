<script lang="ts">
  // A proof under way (a private payment, a shielding, an unshielding, a fee bump): the node proves it on a
  // lane of its own, off the UI thread, and this says how far along it seems from what the earlier proofs
  // of the same kind took on this computer. Halo 2 proofs take seconds; the page stays usable meanwhile.
  import Icon from '../../components/Icon.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { css } from '../../lib/actions';
  import { proofProgress } from './model';
  import { proving } from './store.svelte';

  let now = $state(Date.now());
  $effect(() => {
    if (!proving.now) return;
    now = Date.now();
    const timer = setInterval(() => (now = Date.now()), 200);
    return () => clearInterval(timer);
  });

  const p = $derived(proving.now);
  const elapsed = $derived(p ? Math.max(0, now - p.started) : 0);
  const share = $derived(p ? proofProgress(elapsed, p.estimate) : 0);
  // The steps of a payment out of the pool (src/wallet/shielded.cpp SendShielded): the notes picked and
  // the bundle built, proved, then signed and handed to the mempool. Which one it is in, as far as the time
  // tells: the node does not say.
  const STEPS = ['Choosing the notes', 'Proving (Halo 2)', 'Signing and sending'];
  const step = $derived(share < 0.08 ? 0 : share < 0.9 ? 1 : 2);
  const seconds = $derived(Math.floor(elapsed / 1000));
  const left = $derived(p ? Math.max(0, Math.round((p.estimate - elapsed) / 1000)) : 0);
</script>

{#if p}
  <div class="proving panel" role="status" aria-live="polite">
    <div class="top">
      <span class="spin" aria-hidden="true"><Icon name="shield" size={20} /></span>
      <div class="what">
        <strong>{t(p.label)}</strong>
        <span class="muted small">
          {#if left > 0}{t('About %1 s left', String(left))}{:else}{t('Almost done')}{/if} · {t('%1 s', String(seconds))}
        </span>
      </div>
    </div>
    <div class="bar" role="progressbar" aria-label={t('Proving')} aria-valuemin="0" aria-valuemax="100" aria-valuenow={Math.round(share * 100)}>
      <span use:css={{ width: `${share * 100}%` }}></span>
    </div>
    <ol class="steps">
      {#each STEPS as s, i (s)}
        <li class:done={i < step} class:now={i === step}>{t(s)}</li>
      {/each}
    </ol>
    <p class="faint small">The proof runs in the node, not in this window: you can keep using the app. The time left is an estimate from the earlier proofs on this computer.</p>
  </div>
{/if}

<style>
  .proving {
    display: grid;
    gap: 0.7rem;
    padding: 1rem 1.1rem;
    border-color: color-mix(in srgb, var(--brand, var(--accent)) 45%, var(--line));
  }
  .top {
    display: flex;
    align-items: center;
    gap: 0.75rem;
  }
  .what {
    display: flex;
    flex-direction: column;
    gap: 0.1rem;
  }
  .spin {
    display: inline-grid;
    place-items: center;
    width: 2.2rem;
    height: 2.2rem;
    border-radius: 50%;
    background: color-mix(in srgb, var(--brand, var(--accent)) 16%, transparent);
    color: var(--brand, var(--accent));
    animation: pulse 1.4s ease-in-out infinite;
  }
  @keyframes pulse {
    50% {
      transform: scale(0.9);
      opacity: 0.7;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .spin {
      animation: none;
    }
  }
  .bar {
    position: relative;
    height: 0.45rem;
    border-radius: 999px;
    background: var(--raised);
    overflow: hidden;
  }
  .bar span {
    position: absolute;
    inset: 0 auto 0 0;
    border-radius: 999px;
    background: var(--brand, var(--accent));
    transition: width 200ms linear;
  }
  .steps {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem 1.1rem;
    margin: 0;
    padding: 0;
    list-style: none;
    color: var(--text-faint);
    font-size: 0.84rem;
  }
  .steps li::before {
    content: '○ ';
  }
  .steps li.done {
    color: var(--text-muted);
  }
  .steps li.done::before {
    content: '● ';
  }
  .steps li.now {
    color: var(--text);
    font-weight: 600;
  }
  .steps li.now::before {
    content: '◐ ';
  }
  p {
    margin: 0;
  }
</style>
