<script lang="ts">
  // A modal dialog on the native <dialog> element: it keeps the focus inside, Escape closes it.
  import type { Snippet } from 'svelte';

  let {
    open = $bindable(false),
    title,
    onclose,
    wide = false,
    dismissable = true,
    children,
    actions,
  }: {
    open?: boolean;
    title: string;
    onclose?: () => void;
    wide?: boolean;
    dismissable?: boolean;
    children: Snippet;
    actions?: Snippet;
  } = $props();

  let dialog: HTMLDialogElement | undefined = $state();
  const titleId = `dialog-${Math.random().toString(36).slice(2)}`;

  $effect(() => {
    if (!dialog) return;
    if (open && !dialog.open) dialog.showModal();
    else if (!open && dialog.open) dialog.close();
  });

  function close() {
    open = false;
    onclose?.();
  }
</script>

<dialog
  bind:this={dialog}
  class:wide
  aria-labelledby={titleId}
  oncancel={(e) => {
    e.preventDefault();
    if (dismissable) close();
  }}
>
  <header>
    <h2 id={titleId}>{title}</h2>
    {#if dismissable}
      <button type="button" class="btn ghost icon small" aria-label="Close" onclick={close}>✕</button>
    {/if}
  </header>
  <div class="content">{@render children()}</div>
  {#if actions}<footer>{@render actions()}</footer>{/if}
</dialog>

<style>
  dialog {
    width: min(34rem, calc(100vw - 2rem));
    max-height: calc(100vh - 3rem);
    padding: 0;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-xl);
    background: linear-gradient(180deg, var(--card-top), transparent 10rem), var(--surface);
    color: var(--text);
    box-shadow: var(--shadow);
    overflow: hidden;
  }
  dialog[open] {
    display: flex;
    flex-direction: column;
  }
  dialog.wide {
    width: min(52rem, calc(100vw - 2rem));
  }
  dialog::backdrop {
    background: var(--overlay);
  }
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 1rem;
    padding: 1rem 1.25rem 0.5rem;
  }
  .content {
    padding: 0.5rem 1.25rem 1rem;
    overflow: auto;
  }
  footer {
    display: flex;
    justify-content: flex-end;
    flex-wrap: wrap;
    gap: 0.5rem;
    padding: 0.85rem 1.25rem;
    border-top: 1px solid var(--line);
    background: var(--surface-2);
  }
</style>
