<script lang="ts">
  // A button that opens a menu of commands in sections (the menus of chains-qt, in one place).
  // Keyboard: Enter or Down opens it on its first item, arrows move, Home/End, Escape closes.
  import { t } from '../lib/i18n.svelte';
  import Icon, { type IconName } from './Icon.svelte';
  import { css } from '../lib/actions';
  import type { Command } from '../lib/commands.svelte';
  import { statusTip } from '../lib/state.svelte';

  let {
    label,
    icon = 'more',
    text = '',
    sections,
    align = 'end',
    placement = 'below',
  }: {
    label: string;
    icon?: IconName;
    /** Shown next to the icon; the label (its accessible name) should start with it. */
    text?: string;
    sections: { title: string; items: Command[] }[];
    align?: 'start' | 'end';
    /** Where the menu opens: under the button, above it (the foot of a sidebar), at its right (a rail). */
    placement?: 'below' | 'above' | 'right';
  } = $props();

  let open = $state(false);
  let button: HTMLButtonElement | undefined = $state();
  let menu: HTMLDivElement | undefined = $state();
  let pos = $state({ left: 0, top: 0, maxHeight: 400 });
  const menuId = `menu-${Math.random().toString(36).slice(2)}`;

  function items(): HTMLElement[] {
    return Array.from(menu?.querySelectorAll<HTMLElement>('[role^="menuitem"]:not([disabled])') ?? []);
  }

  function place() {
    if (!button) return;
    const r = button.getBoundingClientRect();
    const width = Math.min(18 * 16, window.innerWidth - 8);
    const left = align === 'end' ? Math.max(4, r.right - width) : Math.max(4, Math.min(r.left, window.innerWidth - width - 4));
    if (placement === 'right') pos = { left: Math.min(r.right + 6, window.innerWidth - width - 4), top: 8, maxHeight: window.innerHeight - 16 };
    else if (placement === 'above') pos = { left, top: 8, maxHeight: r.top - 12 };
    else pos = { left, top: r.bottom + 4, maxHeight: window.innerHeight - r.bottom - 12 };
  }

  /** Once drawn: a menu above the button, or at its right, ends at the button's foot. */
  function settle() {
    if (!menu || !button || placement === 'below') return;
    const r = button.getBoundingClientRect();
    const bottom = placement === 'above' ? r.top - 4 : r.bottom;
    pos = { ...pos, top: Math.max(8, bottom - menu.offsetHeight) };
  }

  function show(focusLast = false) {
    place();
    open = true;
    queueMicrotask(() => {
      const list = items();
      settle();
      (focusLast ? list[list.length - 1] : list[0])?.focus();
    });
  }

  function close(refocus = true) {
    statusTip.text = '';
    open = false;
    if (refocus) button?.focus();
  }

  function run(c: Command) {
    if (c.disabled) return;
    close(false);
    c.run();
  }

  function onkeydown(e: KeyboardEvent) {
    const list = items();
    const at = list.indexOf(document.activeElement as HTMLElement);
    if (e.key === 'ArrowDown') list[(at + 1) % list.length]?.focus();
    else if (e.key === 'ArrowUp') list[(at - 1 + list.length) % list.length]?.focus();
    else if (e.key === 'Home') list[0]?.focus();
    else if (e.key === 'End') list[list.length - 1]?.focus();
    else if (e.key === 'Escape') close();
    else if (e.key === 'Tab') close(false);
    else return;
    e.preventDefault();
    e.stopPropagation();
  }

  function onWindowPointer(e: MouseEvent) {
    if (!open) return;
    const t = e.target as Node;
    if (!menu?.contains(t) && !button?.contains(t)) close(false);
  }
</script>

<svelte:window onmousedown={onWindowPointer} onresize={() => open && close(false)} />

<button
  bind:this={button}
  type="button"
  class="btn ghost small trigger"
  class:icon={!text}
  aria-haspopup="menu"
  aria-expanded={open}
  aria-controls={open ? menuId : undefined}
  aria-label={label}
  title={label}
  onclick={() => (open ? close(false) : show())}
  onkeydown={(e) => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      show(e.key === 'ArrowUp');
    }
  }}
>
  <Icon name={icon} size={18} />
  {#if text}<span>{text}</span>{/if}
</button>

{#if open}
  <div class="menu" id={menuId} role="menu" aria-label={label} tabindex="-1" bind:this={menu} {onkeydown} use:css={{ left: `${pos.left}px`, top: `${pos.top}px`, 'max-height': `${pos.maxHeight}px` }}>
    {#each sections as section (section.title)}
      <div class="section" role="group" aria-label={t(section.title)}>
        <div class="title" aria-hidden="true">{t(section.title)}</div>
        {#each section.items as c (c.id)}
          {#if c.separator}<div class="sep" role="separator"></div>{/if}
          <button
            type="button"
            class="item"
            role={c.checked === undefined ? 'menuitem' : 'menuitemcheckbox'}
            aria-checked={c.checked === undefined ? undefined : c.checked}
            disabled={c.disabled}
            tabindex="-1"
            title={c.tip}
            onmouseenter={() => (statusTip.text = c.tip ?? '')}
            onmouseleave={() => (statusTip.text = '')}
            onfocus={() => (statusTip.text = c.tip ?? '')}
            onblur={() => (statusTip.text = '')}
            onclick={() => run(c)}
          >
            <span class="mark" aria-hidden="true">{c.checked ? '✓' : ''}</span>
            <span class="grow">{t(c.label)}</span>
            {#if c.shortcut}<kbd>{c.shortcut}</kbd>{/if}
          </button>
        {/each}
      </div>
    {/each}
  </div>
{/if}

<style>
  .trigger {
    color: var(--text-muted);
  }
  .trigger[aria-expanded='true'] {
    background: var(--raised);
    color: var(--text);
  }
  .menu {
    position: fixed;
    z-index: 45;
    width: min(18rem, calc(100vw - 8px));
    overflow-y: auto;
    padding: 0.35rem;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-m);
    background: var(--surface);
    box-shadow: var(--shadow);
  }
  .section + .section {
    margin-top: 0.3rem;
    padding-top: 0.3rem;
    border-top: 1px solid var(--line);
  }
  .title {
    padding: 0.3rem 0.6rem 0.15rem 1.8rem;
    color: var(--text-faint);
    font-size: 0.78rem;
    font-weight: 650;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  .item {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    width: 100%;
    padding: 0.36rem 0.6rem 0.36rem 0.3rem;
    border: 0;
    border-radius: var(--radius-s);
    background: none;
    color: var(--text);
    font: inherit;
    font-size: 0.93rem;
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
  .mark {
    width: 1.1rem;
    text-align: center;
    color: var(--accent);
    font-size: 0.8rem;
  }
  .sep {
    height: 1px;
    margin: 0.25rem 0.4rem 0.25rem 1.8rem;
    background: var(--line);
  }
</style>
