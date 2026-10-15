<script lang="ts">
  // The command palette (Ctrl+K): every page and every item of the old menu bar, found by name.
  // Up/Down choose, Enter runs, Escape closes.
  import { t } from '../lib/i18n.svelte';
  import Icon from './Icon.svelte';
  import { buildCommands, palette, searchCommands, type Command } from '../lib/commands.svelte';

  let dialog: HTMLDialogElement | undefined = $state();
  let input: HTMLInputElement | undefined = $state();
  let list: HTMLDivElement | undefined = $state();
  let query = $state('');
  let active = $state(0);

  const all = $derived(buildCommands());
  const found = $derived(searchCommands(all, query));
  /** With nothing typed, by group as the menus were; otherwise best match first, in one list. */
  const groups = $derived.by(() => {
    if (query.trim()) return [{ title: `${found.length} result${found.length === 1 ? '' : 's'}`, items: found }];
    const out: { title: string; items: Command[] }[] = [];
    for (const c of found) {
      const last = out[out.length - 1];
      if (last && last.title === c.group) last.items.push(c);
      else out.push({ title: c.group, items: [c] });
    }
    return out;
  });
  const flat = $derived(groups.flatMap((g) => g.items));

  $effect(() => {
    if (!dialog) return;
    if (palette.open && !dialog.open) {
      query = '';
      active = 0;
      dialog.showModal();
      input?.focus();
    } else if (!palette.open && dialog.open) dialog.close();
  });

  // The first command that can run, when the list changes.
  $effect(() => {
    void query;
    const first = flat.findIndex((c) => !c.disabled);
    active = first < 0 ? 0 : first;
  });

  $effect(() => {
    void active;
    list?.querySelector(`[data-index="${active}"]`)?.scrollIntoView({ block: 'nearest' });
  });

  function close() {
    palette.open = false;
  }

  function run(c: Command | undefined) {
    if (!c || c.disabled) return;
    close();
    c.run();
  }

  function move(step: number) {
    if (flat.length === 0) return;
    let i = active;
    for (let n = 0; n < flat.length; n++) {
      i = (i + step + flat.length) % flat.length;
      if (!flat[i].disabled) break;
    }
    active = i;
  }

  function onkeydown(e: KeyboardEvent) {
    if (e.key === 'ArrowDown') move(1);
    else if (e.key === 'ArrowUp') move(-1);
    else if (e.key === 'PageDown') move(8);
    else if (e.key === 'PageUp') move(-8);
    else if (e.key === 'Enter') run(flat[active]);
    else return;
    e.preventDefault();
  }
</script>

<dialog
  bind:this={dialog}
  class="palette"
  aria-label="Command palette"
  oncancel={(e) => {
    e.preventDefault();
    close();
  }}
  onclick={(e) => {
    if (e.target === dialog) close();
  }}
>
  <div class="box">
    <div class="search">
      <Icon name="search" size={18} />
      <input
        bind:this={input}
        bind:value={query}
        type="text"
        role="combobox"
        aria-expanded="true"
        aria-controls="palette-list"
        aria-activedescendant={flat.length ? `palette-${active}` : undefined}
        aria-autocomplete="list"
        aria-label="Search pages and actions"
        placeholder="Search pages and actions…"
        autocomplete="off"
        spellcheck="false"
        {onkeydown}
      />
      <kbd>Esc</kbd>
    </div>
    <div class="list" id="palette-list" role="listbox" aria-label="Pages and actions" bind:this={list}>
      {#if flat.length === 0}
        <p class="none">Nothing is called “{query}”.</p>
      {/if}
      {#each groups as group (group.title)}
        <div class="group" role="presentation">{t(group.title)}</div>
        {#each group.items as c (c.id)}
          {@const index = flat.indexOf(c)}
          <!-- The input keeps the keyboard (aria-activedescendant); a click runs the command. -->
          <!-- svelte-ignore a11y_click_events_have_key_events -->
          <div
            id="palette-{index}"
            data-index={index}
            class="option"
            class:active={index === active}
            role="option"
            aria-selected={index === active}
            aria-disabled={c.disabled ? 'true' : undefined}
            tabindex="-1"
            onclick={() => run(c)}
            onmousemove={() => {
              if (!c.disabled) active = index;
            }}
          >
            <span class="icon">{#if c.icon}<Icon name={c.icon} size={16} />{:else if c.checked}<Icon name="check" size={16} />{/if}</span>
            <span class="grow label">{c.arg === undefined ? t(c.label) : t(c.label, c.arg)}</span>
            {#if query.trim()}<span class="where">{t(c.group)}</span>{/if}
            {#if c.checked && c.icon}<Icon name="check" size={14} />{/if}
            {#if c.shortcut}<kbd>{c.shortcut}</kbd>{/if}
          </div>
        {/each}
      {/each}
    </div>
    <div class="foot" aria-hidden="true"><kbd>↑</kbd><kbd>↓</kbd><span>choose</span><kbd>Enter</kbd><span>run</span><kbd>Ctrl K</kbd><span>open from anywhere</span></div>
  </div>
</dialog>

<style>
  .palette {
    width: min(40rem, calc(100vw - 1.5rem));
    max-height: min(34rem, calc(100vh - 6rem));
    margin: 4.5rem auto auto;
    padding: 0;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-xl);
    background: linear-gradient(180deg, var(--card-top), transparent 10rem), var(--surface);
    color: var(--text);
    box-shadow: var(--shadow);
    overflow: hidden;
  }
  .palette[open] {
    display: flex;
  }
  .palette::backdrop {
    background: var(--overlay);
  }
  .box {
    display: flex;
    flex-direction: column;
    width: 100%;
    min-height: 0;
  }
  .search {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    padding: 0.7rem 0.9rem;
    border-bottom: 1px solid var(--line);
    color: var(--text-muted);
  }
  .search input {
    flex: 1;
    min-height: 2.2rem;
    border: 0;
    background: none;
    font-size: 1.05rem;
    padding: 0;
  }
  .search input:focus-visible {
    outline: none;
  }
  .list {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    padding: 0.3rem 0.4rem 0.5rem;
  }
  .group {
    padding: 0.55rem 0.6rem 0.2rem;
    color: var(--text-faint);
    font-size: 0.78rem;
    font-weight: 650;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  .option {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    padding: 0.42rem 0.6rem;
    border-radius: var(--radius-s);
    cursor: pointer;
  }
  /* The option under the keys: its background only, no bar at its left (user decision). */
  .option.active {
    background: var(--accent-soft);
  }
  .option[aria-disabled='true'] {
    color: var(--text-faint);
    cursor: default;
  }
  .icon {
    display: inline-flex;
    width: 1rem;
    color: var(--accent);
  }
  .label {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .where {
    color: var(--text-faint);
    font-size: 0.82rem;
    white-space: nowrap;
  }
  .none {
    padding: 1.2rem 0.6rem;
    color: var(--text-muted);
  }
  .foot {
    display: flex;
    align-items: center;
    gap: 0.35rem;
    padding: 0.45rem 0.9rem;
    border-top: 1px solid var(--line);
    background: var(--surface-2);
    color: var(--text-faint);
    font-size: 0.8rem;
  }
  .foot span {
    margin-right: 0.7rem;
  }
  @media (max-width: 560px) {
    .palette {
      margin-top: 0.75rem;
      max-height: calc(100vh - 1.5rem);
    }
    .foot {
      display: none;
    }
  }
</style>
