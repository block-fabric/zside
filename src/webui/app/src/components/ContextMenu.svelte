<script lang="ts" module>
  export interface ContextItem {
    label: string;
    action: () => void;
    disabled?: boolean;
    danger?: boolean;
    /** A line above this item. */
    separator?: boolean;
  }
</script>

<script lang="ts">
  // A menu at the pointer (right click on a row), or under a button. Arrows move, Enter runs, Escape closes.
  import { t } from '../lib/i18n.svelte';
  import { tick } from 'svelte';
  import { css } from '../lib/actions';

  let {
    items,
    x = 0,
    y = 0,
    open = $bindable(false),
    label = 'Actions',
  }: { items: ContextItem[]; x?: number; y?: number; open?: boolean; label?: string } = $props();

  let menu: HTMLDivElement | undefined = $state();
  let pos = $state({ left: 0, top: 0 });
  let returnFocus: HTMLElement | null = null;

  $effect(() => {
    if (!open) return;
    returnFocus = document.activeElement as HTMLElement | null;
    pos = { left: x, top: y };
    tick().then(() => {
      if (!menu) return;
      // Keep the menu in the window.
      const r = menu.getBoundingClientRect();
      pos = {
        left: Math.max(4, Math.min(x, window.innerWidth - r.width - 4)),
        top: Math.max(4, Math.min(y, window.innerHeight - r.height - 4)),
      };
      menu.querySelector<HTMLElement>('button:not([disabled])')?.focus();
    });
  });

  function close() {
    open = false;
    returnFocus?.focus?.();
  }

  function run(item: ContextItem) {
    if (item.disabled) return;
    close();
    item.action();
  }

  function onkeydown(e: KeyboardEvent) {
    const buttons = Array.from(menu?.querySelectorAll<HTMLElement>('button:not([disabled])') ?? []);
    const at = buttons.indexOf(document.activeElement as HTMLElement);
    if (e.key === 'ArrowDown') buttons[(at + 1) % buttons.length]?.focus();
    else if (e.key === 'ArrowUp') buttons[(at - 1 + buttons.length) % buttons.length]?.focus();
    else if (e.key === 'Home') buttons[0]?.focus();
    else if (e.key === 'End') buttons[buttons.length - 1]?.focus();
    else if (e.key === 'Escape' || e.key === 'Tab') close();
    else return;
    e.preventDefault();
    e.stopPropagation();
  }

  function onWindowPointer(e: PointerEvent) {
    if (open && menu && !menu.contains(e.target as Node)) open = false;
  }
</script>

<svelte:window onpointerdown={onWindowPointer} onblur={() => (open = false)} />

{#if open}
  <div class="context-menu" role="menu" aria-label={label} tabindex="-1" bind:this={menu} use:css={{ left: `${pos.left}px`, top: `${pos.top}px` }} {onkeydown}>
    {#each items as item, i (i)}
      {#if item.separator}<div class="sep" role="separator"></div>{/if}
      <button type="button" role="menuitem" class="item" class:danger={item.danger} disabled={item.disabled} onclick={() => run(item)}>{t(item.label)}</button>
    {/each}
  </div>
{/if}

<style>
  .context-menu {
    position: fixed;
    z-index: 60;
    min-width: 15rem;
    padding: 0.3rem;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-m);
    background: var(--surface);
    box-shadow: var(--shadow);
  }
  .item {
    display: block;
    width: 100%;
    padding: 0.42rem 0.7rem;
    border: 0;
    border-radius: var(--radius-s);
    background: none;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .item:hover:not(:disabled),
  .item:focus-visible {
    background: var(--accent-soft);
    outline: none;
  }
  .item:disabled {
    color: var(--text-faint);
    cursor: default;
  }
  .item.danger {
    color: var(--negative);
  }
  .sep {
    height: 1px;
    margin: 0.3rem 0.4rem;
    background: var(--line);
  }
</style>
