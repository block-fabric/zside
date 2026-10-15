<script lang="ts">
  // Settings → Appearance: the layout of the window, shown at once (also in the palette). On the
  // Settings page (staged) it waits for Apply as the other settings do, and Cancel takes it back.
  import { t } from '../lib/i18n.svelte';
  import Icon from './Icon.svelte';
  import { setLayout } from '../lib/commands.svelte';
  import { closeDetail } from '../lib/detail.svelte';
  import { LAYOUTS, type LayoutId } from '../lib/layout';
  import { prefs } from '../lib/state.svelte';
  import { stagePrefs } from '../lib/settingsdraft.svelte';

  let { staged = false }: { staged?: boolean } = $props();

  function choose(id: LayoutId) {
    if (!staged) return setLayout(id);
    closeDetail();
    stagePrefs({ layout: id });
  }
</script>

<fieldset class="field">
  <legend class="label">Layout</legend>
  <div class="layouts">
    {#each LAYOUTS as l (l.id)}
      <label class="layout" class:chosen={prefs.layout === l.id}>
        <input type="radio" name="layout" class="sr-only" value={l.id} checked={prefs.layout === l.id} onchange={() => choose(l.id)} />
        <span class="sketch {l.id}" aria-hidden="true"><span></span><span></span><span></span><span></span></span>
        <span class="name"><Icon name={l.icon} size={16} /> {t(l.name)}</span>
        <span class="summary">{t(l.summary)}.</span>
      </label>
    {/each}
  </div>
  <span class="hint">The same pages and actions, arranged another way. Ctrl+K finds any of them, whatever the layout.</span>
</fieldset>

<style>
  fieldset {
    margin: 0;
    padding: 0;
    border: 0;
  }
  .layouts {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(13rem, 1fr));
    gap: 0.6rem;
  }
  .layout {
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
    padding: 0.6rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    cursor: pointer;
  }
  .layout:has(input:focus-visible) {
    outline: 2px solid var(--focus);
    outline-offset: 2px;
  }
  .layout.chosen {
    border-color: var(--accent);
    box-shadow: inset 0 0 0 1px var(--accent);
  }
  .name {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    font-weight: 600;
  }
  .summary {
    color: var(--text-muted);
    font-size: 0.84rem;
  }
  /* A sketch of each layout, in the colours of the theme. */
  .sketch {
    position: relative;
    display: block;
    height: 4.2rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-s);
    background: var(--bg);
    overflow: hidden;
  }
  .sketch span {
    position: absolute;
    background: var(--surface);
    border: 1px solid var(--line);
  }
  .sketch span:nth-child(1) {
    background: var(--accent-soft);
  }
  .workspace span:nth-child(1) { left: 0; right: 0; top: 0; height: 18%; }
  .workspace span:nth-child(2) { left: 0; right: 0; top: 18%; height: 12%; background: var(--surface-2); }
  .workspace span:nth-child(3) { left: 4%; right: 34%; top: 38%; bottom: 8%; }
  .workspace span:nth-child(4) { right: 0; width: 28%; top: 30%; bottom: 0; background: var(--surface-2); }
  .dashboard span:nth-child(1) { left: 0; top: 0; bottom: 0; width: 22%; }
  .dashboard span:nth-child(2) { left: 27%; top: 10%; width: 44%; height: 38%; }
  .dashboard span:nth-child(3) { left: 75%; top: 10%; right: 4%; height: 38%; }
  .dashboard span:nth-child(4) { left: 27%; top: 56%; right: 4%; bottom: 8%; }
</style>
