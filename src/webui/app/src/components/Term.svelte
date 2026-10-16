<script lang="ts">
  // A word of the trade with its meaning in plain words (lib/glossary.ts): underlined with dots, it
  // shows a short explanation on hover, on focus (Tab) and on a tap; Escape or a tap elsewhere closes it.
  //   <Term id="bmm">merged mined</Term>   (no children: the word of the glossary)
  // The explanation floats over the page (position: fixed), so a panel that clips its content never cuts it;
  // it is in the page only while shown (the text around the word reads as it is).
  import type { Snippet } from 'svelte';
  import { t } from '../lib/i18n.svelte';
  import { glossaryEntry, type TermId } from '../lib/glossary';
  import { css } from '../lib/actions';

  let { id, children }: { id: TermId; children?: Snippet } = $props();

  const entry = $derived(glossaryEntry(id));
  const tipId = `term-${Math.random().toString(36).slice(2)}`;
  let open = $state(false);
  let pinned = $state(false);
  let anchor: HTMLElement | undefined = $state();
  let pos = $state({ left: 0, top: 0, above: false });

  function place() {
    if (!anchor) return;
    const r = anchor.getBoundingClientRect();
    const width = Math.min(288, window.innerWidth - 16);
    const left = Math.max(8, Math.min(r.left + r.width / 2 - width / 2, window.innerWidth - width - 8));
    // Below the word, or above it near the bottom of the window.
    const above = r.bottom + 120 > window.innerHeight && r.top > 140;
    pos = { left, top: above ? r.top - 6 : r.bottom + 6, above };
  }
  function show() {
    place();
    open = true;
  }
  function hide() {
    if (!pinned) open = false;
  }
  function toggle(e: Event) {
    e.stopPropagation();
    pinned = !pinned;
    if (pinned) show();
    else open = false;
  }
  function onkeydown(e: KeyboardEvent) {
    if (e.key === 'Escape' && open) {
      e.stopPropagation();
      pinned = false;
      open = false;
    } else if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      toggle(e);
    }
  }
</script>

<svelte:window
  onpointerdown={(e) => {
    if (pinned && anchor && !anchor.contains(e.target as Node)) {
      pinned = false;
      open = false;
    }
  }}
  onscroll={() => open && place()}
/>

{#if entry}
  <span
    class="term"
    role="button"
    tabindex="0"
    aria-describedby={open ? tipId : undefined}
    aria-expanded={open}
    bind:this={anchor}
    onpointerenter={show}
    onpointerleave={hide}
    onfocus={show}
    onblur={() => {
      pinned = false;
      open = false;
    }}
    onclick={toggle}
    {onkeydown}
    >{#if children}{@render children()}{:else}{t(entry.term)}{/if}</span
  >{#if open}<span id={tipId} role="tooltip" class="tip" class:above={pos.above} use:css={{ left: `${pos.left}px`, top: `${pos.top}px` }}
      ><strong>{t(entry.term)}</strong> {t(entry.text)}</span
    >{/if}
{:else if children}{@render children()}{/if}

<style>
  .term {
    position: relative;
    text-decoration: underline dotted;
    text-decoration-color: var(--text-faint);
    text-underline-offset: 0.2em;
    cursor: help;
    border-radius: 3px;
  }
  .term:hover,
  .term[aria-expanded='true'] {
    text-decoration-color: var(--accent);
  }
  .term:focus-visible {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }
  .tip {
    position: fixed;
    z-index: 80;
    display: block;
    width: min(18rem, calc(100vw - 16px));
    padding: 0.55rem 0.7rem;
    border: 1px solid var(--line-strong, var(--line));
    border-radius: var(--radius-m, 8px);
    background: var(--surface);
    color: var(--text);
    box-shadow: var(--shadow);
    font-size: 0.84rem;
    font-weight: 450;
    line-height: 1.4;
    text-align: left;
    text-transform: none;
    letter-spacing: normal;
    white-space: normal;
    pointer-events: none;
    cursor: default;
  }
  .tip.above {
    transform: translateY(-100%);
  }
  .tip strong {
    color: var(--accent-fg, var(--accent));
    font-weight: 650;
  }
</style>
