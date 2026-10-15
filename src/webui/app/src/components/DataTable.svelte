<script lang="ts" generics="T">
  // A table for long lists (transactions, peers, blocks): sorting by column, filtering by
  // text, and only the rows in view drawn, so 100 000 rows scroll as fast as 10.
  // Keyboard: arrows, Page Up/Down, Home/End move the selection; Enter opens the row.
  import SafeText from './SafeText.svelte';
  import { t } from '../lib/i18n.svelte';
  import type { Snippet } from 'svelte';
  import { css } from '../lib/actions';
  import { filterAndSort, visibleRange, type Column, type SortDir } from '../lib/table';

  let {
    rows,
    columns,
    key,
    filter = '',
    sortKey = $bindable(null),
    sortDir = $bindable('desc'),
    selected = $bindable(null),
    rowHeight = 34,
    label,
    empty = 'Nothing to show',
    onactivate,
    oncontext,
    cell,
    fit = false,
  }: {
    rows: readonly T[];
    columns: readonly Column<T>[];
    key: (row: T) => string;
    filter?: string;
    sortKey?: string | null;
    sortDir?: SortDir;
    selected?: string | null;
    rowHeight?: number;
    label: string;
    empty?: string;
    onactivate?: (row: T) => void;
    oncontext?: (row: T, event: MouseEvent) => void;
    cell?: Snippet<[T, Column<T>]>;
    /** The columns share the width of the table (a narrow window): no scrolling sideways, long text cut. */
    fit?: boolean;
  } = $props();

  let scroller: HTMLDivElement | undefined = $state();
  const gridId = `grid-${Math.random().toString(36).slice(2)}`;
  let scrollTop = $state(0);
  let viewport = $state(400);

  const shown = $derived(filterAndSort(rows, columns, filter, sortKey, sortDir));
  const range = $derived(visibleRange(scrollTop, viewport, rowHeight, shown.length));
  const slice = $derived(shown.slice(range.start, range.end));
  const template = $derived(columns.map((c) => c.width ?? 'minmax(6rem, 1fr)').join(' '));
  const selectedIndex = $derived(selected === null ? -1 : shown.findIndex((r) => key(r) === selected));

  $effect(() => {
    if (!scroller || typeof ResizeObserver === 'undefined') return;
    const observer = new ResizeObserver(() => {
      if (scroller) viewport = scroller.clientHeight;
    });
    observer.observe(scroller);
    viewport = scroller.clientHeight;
    return () => observer.disconnect();
  });

  function sortBy(column: Column<T>) {
    if (column.sortable === false) return;
    if (sortKey === column.key) sortDir = sortDir === 'asc' ? 'desc' : 'asc';
    else {
      sortKey = column.key;
      sortDir = 'asc';
    }
  }

  function select(index: number) {
    if (shown.length === 0) return;
    const i = Math.max(0, Math.min(shown.length - 1, index));
    selected = key(shown[i]);
    // Keep the row in view, under the header.
    if (!scroller) return;
    const top = i * rowHeight;
    const header = rowHeight;
    if (top < scroller.scrollTop) scroller.scrollTop = top;
    else if (top + rowHeight > scroller.scrollTop + scroller.clientHeight - header) scroller.scrollTop = top + rowHeight - scroller.clientHeight + header;
  }

  function onkeydown(e: KeyboardEvent) {
    const page = Math.max(1, Math.floor(viewport / rowHeight) - 1);
    const handled: Record<string, () => void> = {
      ArrowDown: () => select(selectedIndex + 1),
      ArrowUp: () => select(selectedIndex < 0 ? 0 : selectedIndex - 1),
      PageDown: () => select(selectedIndex + page),
      PageUp: () => select(selectedIndex - page),
      Home: () => select(0),
      End: () => select(shown.length - 1),
      Enter: () => {
        if (selectedIndex >= 0) onactivate?.(shown[selectedIndex]);
      },
    };
    const fn = handled[e.key];
    if (fn) {
      e.preventDefault();
      fn();
    }
  }

  function ariaSort(column: Column<T>): 'ascending' | 'descending' | 'none' {
    if (sortKey !== column.key) return 'none';
    return sortDir === 'asc' ? 'ascending' : 'descending';
  }
</script>

<div
  class="table"
  class:fit
  role="grid"
  aria-label={label}
  aria-rowcount={shown.length + 1}
  aria-activedescendant={selectedIndex >= 0 ? `${gridId}-row-${selectedIndex}` : undefined}
  tabindex="0"
  bind:this={scroller}
  onscroll={() => scrollTop = scroller?.scrollTop ?? 0}
  {onkeydown}
>
  <div class="head" role="row" aria-rowindex="1" use:css={{ 'grid-template-columns': template, height: `${rowHeight}px` }}>
    {#each columns as column (column.key)}
      <div role="columnheader" aria-sort={ariaSort(column)} class="cell {column.align ?? 'start'}">
        {#if column.sortable === false}
          <span>{t(column.label)}</span>
        {:else}
          <button type="button" title="Sort by {column.label || 'status'}" onclick={() => sortBy(column)}>
            {t(column.label)}
            <span class="arrow" aria-hidden="true">{sortKey === column.key ? (sortDir === 'asc' ? '▲' : '▼') : ''}</span>
          </button>
        {/if}
      </div>
    {/each}
  </div>
  {#if shown.length === 0}
    <div class="empty">{rows.length === 0 ? empty : 'Nothing matches the filter'}</div>
  {:else}
    <div class="body" use:css={{ height: `${shown.length * rowHeight}px` }}>
      <div class="rows" use:css={{ transform: `translateY(${range.start * rowHeight}px)` }}>
        {#each slice as row, i (key(row))}
          {@const index = range.start + i}
          <!-- The grid takes the keyboard (aria-activedescendant); a click selects as the arrows do. -->
          <!-- svelte-ignore a11y_click_events_have_key_events, a11y_interactive_supports_focus -->
          <div
            id="{gridId}-row-{index}"
            class="dt-row"
            class:odd={index % 2 === 1}
            class:selected={selected === key(row)}
            role="row"
            aria-rowindex={index + 2}
            aria-selected={selected === key(row)}
            use:css={{ 'grid-template-columns': template, height: `${rowHeight}px` }}
            onclick={() => (selected = key(row))}
            ondblclick={() => onactivate?.(row)}
            oncontextmenu={(e) => {
              if (oncontext) {
                e.preventDefault();
                selected = key(row);
                oncontext(row, e);
              }
            }}
          >
            {#each columns as column (column.key)}
              <div role="gridcell" class="cell {column.align ?? 'start'}" class:mono={column.mono}>
                {#if cell}{@render cell(row, column)}{:else if column.words}{String(column.value(row) ?? '')}{:else}<SafeText text={String(column.value(row) ?? '')} />{/if}
              </div>
            {/each}
          </div>
        {/each}
      </div>
    </div>
  {/if}
</div>

<style>
  .table {
    position: relative;
    height: 100%;
    min-height: 8rem;
    overflow: auto;
    border: 1px solid var(--line);
    border-radius: var(--radius-xl);
    background: linear-gradient(180deg, var(--card-top), transparent 8rem), var(--surface);
    box-shadow: var(--shadow-card);
    font-variant-numeric: tabular-nums;
  }
  .table:focus-visible {
    outline-offset: -2px;
  }
  .head {
    position: sticky;
    top: 0;
    z-index: 1;
    display: grid;
    min-width: max-content;
    width: 100%;
    background: var(--surface);
    border-bottom: 1px solid var(--line);
    font-size: 0.8rem;
    font-weight: 600;
    letter-spacing: 0.02em;
    color: var(--text-muted);
  }
  .head button {
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
    padding: 0;
    border: 0;
    background: none;
    color: inherit;
    font: inherit;
    cursor: pointer;
  }
  .arrow {
    font-size: 0.65rem;
    min-width: 0.7rem;
  }
  /* An amount column: the arrow goes before the label, so that the label ends where the values end. */
  .cell.end button {
    flex-direction: row-reverse;
  }
  .body {
    position: relative;
    min-width: max-content;
  }
  .rows {
    position: absolute;
    inset: 0 0 auto 0;
    will-change: transform;
  }
  /* Not .row: that is the flex row of base.css, whose gap would widen every column of the rows (not of the head). */
  .dt-row {
    display: grid;
    min-width: max-content;
    width: 100%;
    cursor: default;
  }
  .dt-row {
    border-radius: var(--radius-m);
  }
  .dt-row.odd {
    background: color-mix(in srgb, var(--surface-2) 60%, transparent);
  }
  .dt-row:hover {
    background: var(--raised);
  }
  /* The row chosen: its background, and a thin outline; no bar at its left (user decision). */
  .dt-row.selected {
    background: var(--accent-soft);
    box-shadow: inset 0 0 0 1px color-mix(in srgb, var(--accent) 35%, transparent);
  }
  .cell {
    display: flex;
    align-items: center;
    padding: 0 0.75rem;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
    min-width: 0;
  }
  .cell.end {
    justify-content: flex-end;
    /* Content wider than the cell (an amount in a narrow column) is cut at its end, never at its start: with
       plain flex-end it would overflow to the left, and 0.00100000 read 00100000 (audit port-2 #2). */
    justify-content: safe flex-end;
    text-align: right;
  }
  .cell.end > :global(*) {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .cell.center {
    justify-content: center;
  }
  .cell.mono {
    font-family: var(--font-mono);
    font-size: 0.88rem;
  }
  .fit .head,
  .fit .body,
  .fit .dt-row {
    min-width: 0;
  }
  .fit {
    overflow-x: hidden;
  }
  .empty {
    padding: 2rem 1rem;
    text-align: center;
    color: var(--text-muted);
  }
</style>
