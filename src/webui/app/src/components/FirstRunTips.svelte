<script lang="ts">
  // The first-run tips of Home, in Simple mode: three steps of this chain (lib/firststeps.ts), each with
  // the page it opens, until the user dismisses them (the preference tipsDone; the palette's "Show the
  // first steps again" brings them back).
  import { t } from '../lib/i18n.svelte';
  import Icon from './Icon.svelte';
  import { FIRST_STEPS, type FirstStep } from '../lib/firststeps';
  import { go } from '../lib/nav.svelte';
  import { openQuick } from '../lib/quick.svelte';
  import { app, prefs, savePrefs } from '../lib/state.svelte';
  import { pageDisabled } from '../lib/commands.svelte';
  import { simpleMode } from '../lib/uimode.svelte';

  function run(a: NonNullable<FirstStep['action']>) {
    if (a.panel) openQuick(a.panel);
    else go(a.page, a.tab ?? '');
  }
</script>

{#if simpleMode() && prefs.loaded && !prefs.tipsDone}
  <section class="tips panel" aria-labelledby="tips-title">
    <header>
      <h2 id="tips-title">{t('Welcome to %1', app.info?.name ?? 'Chains')}</h2>
      <p class="muted">Three first steps. Underlined words explain themselves: point at them, or tap them.</p>
      <button type="button" class="btn ghost small icon close" aria-label="Hide the first steps" title="Hide the first steps" onclick={() => savePrefs({ tipsDone: true })}><Icon name="x" size={16} /></button>
    </header>
    <ol>
      {#each FIRST_STEPS as step, i (step.title)}
        <li>
          <span class="n" aria-hidden="true">{i + 1}</span>
          <div class="body">
            <h3><Icon name={step.icon} size={16} /> {t(step.title)}</h3>
            <p>{t(step.text)}</p>
            {#if step.action}
              <button type="button" class="btn small" disabled={pageDisabled(step.action.page)} onclick={() => run(step.action!)}>{t(step.action.label)} <Icon name="arrow" size={14} /></button>
            {/if}
          </div>
        </li>
      {/each}
    </ol>
    <footer>
      <span class="muted small">This is Simple mode. Every option is in Advanced mode: switch at the foot of the sidebar.</span>
      <button type="button" class="btn primary small" onclick={() => savePrefs({ tipsDone: true })}>Got it</button>
    </footer>
  </section>
{/if}

<style>
  .tips {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    padding: 1rem 1.1rem;
    border-color: color-mix(in srgb, var(--accent) 40%, var(--line));
    background: linear-gradient(135deg, var(--accent-soft), transparent 60%), var(--surface);
  }
  header {
    position: relative;
    padding-right: 2.5rem;
  }
  h2 {
    margin: 0;
    font-size: 1.1rem;
  }
  header p {
    margin: 0.2rem 0 0;
  }
  .close {
    position: absolute;
    top: -0.2rem;
    right: -0.3rem;
  }
  ol {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(14rem, 1fr));
    gap: 0.7rem;
    margin: 0;
    padding: 0;
    list-style: none;
  }
  li {
    display: flex;
    gap: 0.65rem;
    padding: 0.75rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
  }
  .n {
    display: inline-grid;
    place-items: center;
    flex: none;
    width: 1.7rem;
    height: 1.7rem;
    border-radius: 50%;
    background: var(--accent);
    color: var(--on-accent);
    font-weight: 700;
  }
  .body {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 0.35rem;
    min-width: 0;
  }
  h3 {
    display: flex;
    align-items: center;
    gap: 0.35rem;
    margin: 0;
    font-size: 0.95rem;
  }
  h3 :global(svg) {
    color: var(--accent-fg);
  }
  .body p {
    margin: 0;
    color: var(--text-muted);
    font-size: 0.88rem;
    line-height: 1.4;
  }
  footer {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
  }
</style>
