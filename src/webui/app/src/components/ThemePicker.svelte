<script lang="ts">
  // The theme at the foot of the sidebar: a list drawn by the app, opening above its button (user
  // decision: a native list opened past the window and could not always be seen). A listbox: Enter,
  // Space or Up/Down open it on the theme in use; arrows, Home and End move; Enter picks; Escape and
  // a click elsewhere close it.
  import { t } from '../lib/i18n.svelte';
  import { css } from '../lib/actions';
  import Icon from './Icon.svelte';

  let {
    value,
    groups,
    onpick,
    label = 'Theme',
  }: {
    value: string;
    /** The themes, in groups (the built-in ones, the user's); a group without a title has none shown. */
    groups: { title: string; options: { id: string; name: string; own?: boolean }[] }[];
    onpick: (id: string) => void;
    label?: string;
  } = $props();

  let open = $state(false);
  let button: HTMLButtonElement | undefined = $state();
  let list: HTMLDivElement | undefined = $state();
  let active = $state('');
  let pos = $state({ left: 0, bottom: 0, width: 200, maxHeight: 400 });
  const listId = `themes-${Math.random().toString(36).slice(2)}`;

  const all = $derived(groups.flatMap((g) => g.options));
  const current = $derived(all.find((o) => o.id === value));
  const nameOf = (o: { name: string; own?: boolean }) => (o.own ? o.name : t(o.name));

  function show() {
    if (!button) return;
    const r = button.getBoundingClientRect();
    // Above the button, as wide as it at least, as high as the window allows.
    const width = Math.max(r.width, 13 * 16);
    pos = { left: Math.max(4, Math.min(r.left, window.innerWidth - width - 4)), bottom: window.innerHeight - r.top + 4, width, maxHeight: Math.max(120, r.top - 12) };
    active = value;
    open = true;
    queueMicrotask(() => {
      list?.focus();
      list?.querySelector<HTMLElement>('[aria-selected="true"]')?.scrollIntoView?.({ block: 'nearest' });
    });
  }
  function close(refocus = true) {
    open = false;
    if (refocus) button?.focus();
  }
  function pick(id: string) {
    close();
    if (id !== value) onpick(id);
  }
  function move(step: number | 'first' | 'last') {
    if (!all.length) return;
    const at = all.findIndex((o) => o.id === active);
    const next = step === 'first' ? 0 : step === 'last' ? all.length - 1 : Math.max(0, Math.min(all.length - 1, (at < 0 ? 0 : at) + step));
    active = all[next].id;
    document.getElementById(optionId(active))?.scrollIntoView?.({ block: 'nearest' });
  }
  const optionId = (id: string) => `${listId}-${id.replace(/[^A-Za-z0-9_-]/g, '_')}`;

  function onButtonKey(e: KeyboardEvent) {
    if (['ArrowDown', 'ArrowUp', 'Enter', ' '].includes(e.key)) {
      e.preventDefault();
      show();
    }
  }
  function onListKey(e: KeyboardEvent) {
    const keys: Record<string, () => void> = {
      ArrowDown: () => move(1),
      ArrowUp: () => move(-1),
      Home: () => move('first'),
      End: () => move('last'),
      Enter: () => pick(active),
      ' ': () => pick(active),
      Escape: () => close(),
      Tab: () => close(false),
    };
    const f = keys[e.key];
    if (!f) return;
    if (e.key !== 'Tab') e.preventDefault();
    f();
  }
  // A click outside closes it.
  $effect(() => {
    if (!open) return;
    const away = (e: PointerEvent) => {
      const target = e.target as Node;
      if (!list?.contains(target) && !button?.contains(target)) close(false);
    };
    const resize = () => close(false);
    document.addEventListener('pointerdown', away, true);
    window.addEventListener('resize', resize);
    return () => {
      document.removeEventListener('pointerdown', away, true);
      window.removeEventListener('resize', resize);
    };
  });
</script>

<button
  bind:this={button}
  type="button"
  class="picker"
  aria-label={t(label)}
  title={t(label)}
  aria-haspopup="listbox"
  aria-expanded={open}
  aria-controls={open ? listId : undefined}
  onclick={() => (open ? close() : show())}
  onkeydown={onButtonKey}
>
  <span class="name" translate={current?.own ? 'no' : undefined}>{current ? nameOf(current) : value}</span>
  <Icon name="chevron" size={15} />
</button>

{#if open}
  <div
    bind:this={list}
    id={listId}
    class="list"
    role="listbox"
    tabindex="-1"
    aria-label={t(label)}
    aria-activedescendant={optionId(active)}
    onkeydown={onListKey}
    use:css={{ left: `${pos.left}px`, bottom: `${pos.bottom}px`, width: `${pos.width}px`, 'max-height': `${pos.maxHeight}px` }}
  >
    {#each groups as g (g.title)}
      {#if g.options.length}
        <div role="group" aria-label={g.title ? t(g.title) : undefined}>
          {#if g.title}<div class="group faint" aria-hidden="true">{t(g.title)}</div>{/if}
          {#each g.options as o (o.id)}
            <div
              id={optionId(o.id)}
              role="option"
              tabindex="-1"
              class="option"
              class:active={o.id === active}
              aria-selected={o.id === value}
              translate={o.own ? 'no' : undefined}
              onpointermove={() => (active = o.id)}
              onclick={() => pick(o.id)}
              onkeydown={onListKey}
            >
              <span class="check">{#if o.id === value}<Icon name="check" size={14} />{/if}</span>
              {nameOf(o)}
            </div>
          {/each}
        </div>
      {/if}
    {/each}
  </div>
{/if}

<style>
  .picker {
    display: flex;
    /* As wide as the theme shown ("Matrix Green", not "Matrix Gr": audit port-1 #11), on a line of its own
       when the row has no room for it. */
    flex: 1 0 auto;
    max-width: 100%;
    align-items: center;
    justify-content: space-between;
    gap: 0.4rem;
    min-width: 0;
    min-height: 1.9rem;
    padding: 0.15rem 0.55rem 0.15rem 0.7rem;
    /* As the other dropdowns (select, styles/base.css). */
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    color: var(--text);
    font: inherit;
    font-size: 0.88rem;
    cursor: pointer;
    transition: border-color 120ms;
  }
  .picker:focus-visible {
    outline: none;
    border-color: var(--accent-fg);
    box-shadow: 0 0 0 3px color-mix(in srgb, var(--accent) 28%, transparent);
  }
  .picker :global(svg) {
    color: var(--text-muted);
    transition: transform 0.15s;
  }
  /* The list opens upwards: the chevron points up while it is open. */
  .picker[aria-expanded='true'] :global(svg) {
    transform: rotate(180deg);
  }
  .name {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .list {
    position: fixed;
    z-index: 60;
    overflow-y: auto;
    padding: 0.3rem;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    box-shadow: var(--shadow);
    font-size: 0.88rem;
  }
  .list:focus {
    outline: none;
  }
  .group {
    padding: 0.45rem 0.6rem 0.2rem;
    font-size: 0.76rem;
    letter-spacing: 0.04em;
    text-transform: uppercase;
  }
  .option {
    display: flex;
    align-items: center;
    gap: 0.35rem;
    padding: 0.38rem 0.6rem 0.38rem 0.35rem;
    border-radius: var(--radius-s);
    color: var(--text);
    cursor: pointer;
  }
  .option.active {
    background: var(--accent-soft);
  }
  .option[aria-selected='true'] {
    color: var(--accent-fg);
    font-weight: 600;
  }
  .check {
    display: inline-flex;
    width: 1rem;
    justify-content: center;
  }
</style>
