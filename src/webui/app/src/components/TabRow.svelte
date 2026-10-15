<script lang="ts">
  // The tab row of a page. When the tab shown has tabs of its own (a page shown inside this one), they
  // follow on the same row, after an arrow (user decision, 2026-10-15).
  import { t } from '../lib/i18n.svelte';
  import Icon from './Icon.svelte';

  interface Tab {
    id: string;
    label: string;
  }
  let {
    label,
    tabs,
    current,
    select,
    sub = null,
  }: {
    label: string;
    tabs: Tab[];
    current: string;
    select: (id: string) => void;
    sub?: { label: string; tabs: Tab[]; current: string; select: (id: string) => void } | null;
  } = $props();
</script>

<div class="tabs tab-row">
  <div class="group" role="tablist" aria-label={t(label)}>
    {#each tabs as tb (tb.id)}
      <button type="button" role="tab" aria-selected={current === tb.id} onclick={() => select(tb.id)}>{t(tb.label)}</button>
    {/each}
  </div>
  {#if sub}
    <span class="arrow" aria-hidden="true"><Icon name="chevron-right" size={16} /></span>
    <div class="group sub" role="tablist" aria-label={t(sub.label)}>
      {#each sub.tabs as tb (tb.id)}
        <button type="button" role="tab" aria-selected={sub.current === tb.id} onclick={() => sub.select(tb.id)}>{t(tb.label)}</button>
      {/each}
    </div>
  {/if}
</div>

<style>
  .tab-row {
    align-items: center;
  }
  .group {
    display: flex;
    flex-wrap: wrap;
    gap: 0.2rem;
  }
  .arrow {
    display: inline-grid;
    place-items: center;
    padding: 0 0.15rem;
    color: var(--text-muted);
  }
  /* The sub-tabs: a step smaller than the tabs. */
  .sub :global(button) {
    padding: 0.3rem 0.75rem;
    font-size: 0.92em;
  }
</style>
