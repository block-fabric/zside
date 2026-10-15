<script lang="ts">
  // A card of the dashboard: a title that opens the full page, what it shows, its actions. A `stat`
  // card is one of the summary row: a figure, a few lines, its actions side by side.
  import type { Snippet } from 'svelte';
  import Icon, { type IconName } from '../../components/Icon.svelte';

  let {
    title,
    icon,
    open,
    openLabel,
    wide = false,
    stat = false,
    children,
    actions,
  }: {
    title: string;
    icon: IconName;
    /** Opens the full page of the card. */
    open: () => void;
    openLabel: string;
    wide?: boolean;
    stat?: boolean;
    children: Snippet;
    actions?: Snippet;
  } = $props();

  const id = `card-${Math.random().toString(36).slice(2)}`;
</script>

<section class="card panel" class:wide class:stat aria-labelledby={id}>
  <header>
    <span class="icon"><Icon name={icon} size={17} /></span>
    <h2 {id}>{title}</h2>
    <button type="button" class="btn ghost small open" onclick={open} aria-label={openLabel} title={openLabel}>
      <span class="word">Open</span><Icon name="arrow" size={15} />
    </button>
  </header>
  <div class="body">{@render children()}</div>
  {#if actions}<footer>{@render actions()}</footer>{/if}
</section>

<style>
  .card {
    display: flex;
    flex-direction: column;
    min-width: 0;
    transition: border-color 160ms;
  }
  .card:hover {
    border-color: var(--line-strong);
  }
  .wide {
    grid-column: span 2;
  }
  header {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    padding: 0.95rem 0.8rem 0.45rem 1.1rem;
  }
  .icon {
    display: inline-grid;
    place-items: center;
    width: 2rem;
    height: 2rem;
    flex: none;
    border-radius: 50%;
    background: var(--accent-soft);
    color: var(--accent-fg);
    box-shadow: 0 0 18px -6px var(--accent);
  }
  h2 {
    flex: 1;
    min-width: 0;
    font-size: 0.98rem;
    font-weight: 620;
  }
  .stat h2 {
    color: var(--text-muted);
  }
  .open {
    color: var(--text-muted);
    border-radius: 999px;
  }
  .word {
    font-size: 0.85rem;
  }
  .stat .word {
    display: none;
  }
  .body {
    flex: 1;
    padding: 0.35rem 1.1rem 0.9rem;
    min-width: 0;
  }
  footer {
    display: flex;
    flex-wrap: wrap;
    gap: 0.45rem;
    padding: 0 1.1rem 1.05rem;
  }
  footer :global(.btn) {
    min-height: 2.15rem;
    padding: 0 0.85rem;
    font-size: 0.9rem;
  }
  footer :global(.btn.grow) {
    flex: 1 1 auto;
  }
  /* The summary row: the actions on one line, at the bottom of each card. */
  .stat footer {
    flex-wrap: nowrap;
    gap: 0.35rem;
  }
  .stat footer :global(.btn) {
    min-width: 0;
    padding: 0 0.7rem;
  }
  .stat footer :global(.btn.ghost) {
    padding: 0 0.5rem;
  }
  .stat footer :global(.btn.icon) {
    flex: none;
    width: 2.15rem;
    padding: 0;
  }
  @media (max-width: 1100px) {
    .wide {
      grid-column: auto;
    }
  }
</style>
