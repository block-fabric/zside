<script lang="ts">
  // Opens the command palette: a search field to look at.
  import Icon from './Icon.svelte';
  import { openPalette } from '../lib/commands.svelte';
</script>

<button
  type="button"
  class="palette-button"
  aria-label="Search pages and actions (Ctrl+K)"
  title="Search pages and actions (Ctrl+K)"
  aria-haspopup="dialog"
  onclick={openPalette}
>
  <Icon name="search" size={16} />
  <span class="grow">Search…</span><kbd>Ctrl K</kbd>
</button>

<style>
  .palette-button {
    display: inline-flex;
    align-items: center;
    gap: 0.5rem;
    min-width: 13rem;
    min-height: 2rem;
    padding: 0 0.5rem 0 0.65rem;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-s);
    background: var(--surface-2);
    color: var(--text-muted);
    font: inherit;
    font-size: 0.92rem;
    text-align: left;
    cursor: pointer;
  }
  .palette-button:hover {
    background: var(--raised);
    color: var(--text);
  }
</style>
