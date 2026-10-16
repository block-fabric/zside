<script lang="ts">
  // Simple or Advanced (lib/uimode.svelte.ts): a segmented control of two radio buttons, in the foot of
  // the sidebar beside the size of the text. The arrow keys move between them, as in any radio group.
  import { t } from '../lib/i18n.svelte';
  import { prefs } from '../lib/state.svelte';
  import { setUiMode, UI_MODES, type UiMode } from '../lib/uimode.svelte';

  let buttons: HTMLButtonElement[] = $state([]);

  function onkeydown(e: KeyboardEvent, index: number) {
    const step = e.key === 'ArrowRight' || e.key === 'ArrowDown' ? 1 : e.key === 'ArrowLeft' || e.key === 'ArrowUp' ? -1 : 0;
    if (!step) return;
    e.preventDefault();
    const next = (index + step + UI_MODES.length) % UI_MODES.length;
    setUiMode(UI_MODES[next].id);
    buttons[next]?.focus();
  }
  function pick(mode: UiMode) {
    setUiMode(mode);
  }
</script>

<div class="mode-switch" role="radiogroup" aria-label="Interface mode" title="Simple: what a beginner needs, in plain words. Advanced: every section of every page.">
  {#each UI_MODES as m, i (m.id)}
    <button
      type="button"
      role="radio"
      aria-checked={prefs.uiMode === m.id}
      tabindex={prefs.uiMode === m.id ? 0 : -1}
      title={t(m.summary)}
      bind:this={buttons[i]}
      onclick={() => pick(m.id)}
      onkeydown={(e) => onkeydown(e, i)}
    >{t(m.label)}</button>
  {/each}
</div>

<style>
  .mode-switch {
    display: inline-flex;
    flex: 1 1 7.5rem;
    min-width: 0;
    padding: 2px;
    border: 1px solid var(--line);
    border-radius: 999px;
    background: var(--surface-2);
  }
  button {
    flex: 1 1 auto;
    min-width: 0;
    padding: 0.22rem 0.4rem;
    border: 0;
    border-radius: 999px;
    background: none;
    color: var(--text-muted);
    font: inherit;
    font-size: 0.78rem;
    font-weight: 600;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    cursor: pointer;
  }
  button:hover:not([aria-checked='true']) {
    color: var(--text);
    background: var(--raised);
  }
  button[aria-checked='true'] {
    background: var(--accent);
    color: var(--on-accent);
    box-shadow: 0 4px 14px -8px var(--accent);
  }
  button:focus-visible {
    outline: 2px solid var(--focus, var(--accent));
    outline-offset: 1px;
  }
</style>
