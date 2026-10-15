<script lang="ts">
  // Pages of a table: Previous / Next, where we are, and (when `sizes` is given) how many rows a page
  // shows. The page is 0-based; the parent keeps it and the size (bind:page, bind:size).
  import { t } from '../lib/i18n.svelte';
  import { formatCount } from '../lib/format';
  import Icon from './Icon.svelte';

  let {
    total,
    page = $bindable(0),
    size = $bindable(10),
    sizes = [],
    label = 'rows',
    onpage,
    onsize,
  }: {
    total: number;
    page?: number;
    size?: number;
    /** The page sizes offered (none: no selector). */
    sizes?: readonly number[];
    /** What the rows are, for screen readers ("deposits"). */
    label?: string;
    /** Told of a new page (for a parent that keeps pages itself rather than binding). */
    onpage?: (page: number) => void;
    /** Told of a new page size (for a parent that keeps it itself, in its prefs say). */
    onsize?: (size: number) => void;
  } = $props();
  function go(n: number) {
    page = n;
    onpage?.(n);
  }

  const pages = $derived(Math.max(1, Math.ceil(total / size)));
  // A page past the end (fewer rows now, or a bigger page): the last one.
  $effect(() => {
    if (page > pages - 1) go(pages - 1);
  });
  const from = $derived(total === 0 ? 0 : page * size + 1);
  const to = $derived(Math.min(total, (page + 1) * size));
</script>

{#if total > size || sizes.length > 0}
  <nav class="pager" aria-label={t('Pages of the %1', label)}>
    <span class="faint where">{formatCount(from)}–{formatCount(to)} {t('of')} {formatCount(total)}</span>
    {#if sizes.length > 0}
      <label class="size">
        <span class="faint">{t('Show')}</span>
        <select value={size} aria-label={t('Rows per page')} onchange={(e) => { size = Number((e.currentTarget as HTMLSelectElement).value); onsize?.(size); go(0); }}>
          {#each sizes as n (n)}<option value={n}>{n}</option>{/each}
        </select>
      </label>
    {/if}
    {#if pages > 1}
      <button type="button" class="btn small ghost icon" aria-label={t('Previous page')} disabled={page === 0} onclick={() => go(Math.max(0, page - 1))}><Icon name="chevron-right" size={15} /></button>
      <span class="num">{page + 1} / {pages}</span>
      <button type="button" class="btn small ghost icon" aria-label={t('Next page')} disabled={page >= pages - 1} onclick={() => go(Math.min(pages - 1, page + 1))}><Icon name="chevron-right" size={15} /></button>
    {/if}
  </nav>
{/if}

<style>
  .pager {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: flex-end;
    gap: 0.5rem;
    padding: 0.5rem 0.25rem 0;
    font-size: 0.86rem;
  }
  .where {
    margin-right: auto;
  }
  .size {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
  }
  .size select {
    width: auto;
    min-height: 1.9rem;
  }
  /* Previous: the chevron turned to point left. */
  .pager button:first-of-type :global(svg) {
    transform: rotate(180deg);
  }
</style>
