<script lang="ts">
  import SafeText from './SafeText.svelte';
  // Short notices in a corner: copied, sent, a warning of the node.
  import { t } from '../lib/i18n.svelte';
  import Icon from './Icon.svelte';
  import { dismissToast, toasts } from '../lib/state.svelte';

  /** The newest two show; the others wait behind them (a column of notices covered the amounts of a table). */
  const MAX_SHOWN = 2;
  const shown = $derived(toasts.list.slice(-MAX_SHOWN));
  const hidden = $derived(toasts.list.length - shown.length);

  // Notices sit in the bottom corner, where the buttons of a form often are. They go to the top corner instead
  // when the user works in a form they would cover (the focus in it), or when they would cover more of what
  // can be used (fields, buttons) at the bottom than at the top (audit port-1 #11).
  let stack: HTMLDivElement | undefined = $state();
  let top = $state(false);
  type Box = { left: number; right: number; top: number; bottom: number };
  /** Where the stack is in a corner, as its styles place it (whichever corner it is in now). */
  function cornerBox(atTop: boolean): Box | null {
    if (!stack || typeof window === 'undefined') return null;
    const em = parseFloat(getComputedStyle(document.documentElement).fontSize) || 16;
    const right = window.innerWidth - em;
    const height = stack.offsetHeight;
    const bottom = atTop ? 4 * em + height : window.innerHeight - 3 * em;
    return { left: right - stack.offsetWidth, right, top: bottom - height, bottom };
  }
  const overlaps = (a: Box, b: Box) => a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
  /** The part of the page the user works in: the form (or card, panel) around the focus. */
  function workArea(): DOMRect | null {
    const el = typeof document === 'undefined' ? null : document.activeElement;
    if (!el || el === document.body || !(el instanceof HTMLElement) || stack?.contains(el)) return null;
    if (!el.matches('input, select, textarea, button, [contenteditable="true"]')) return null;
    return (el.closest('form, [data-form], .card, .panel') ?? el).getBoundingClientRect();
  }
  /** How many of points spread over a box land on something that can be used (not the notices). */
  function usableUnder(box: Box): number {
    if (typeof document.elementsFromPoint !== 'function') return 0;
    let hits = 0;
    for (const fx of [0.15, 0.5, 0.85]) {
      for (const fy of [0.15, 0.5, 0.85]) {
        const x = box.left + (box.right - box.left) * fx;
        const y = box.top + (box.bottom - box.top) * fy;
        const under = document.elementsFromPoint(x, y).find((e) => !stack?.contains(e));
        if (under?.closest('input, select, textarea, button, a[href], [role="button"], [contenteditable="true"]')) hits++;
      }
    }
    return hits;
  }
  function placeToasts(): void {
    const bottom = cornerBox(false);
    if (!bottom || shown.length === 0) {
      top = false;
      return;
    }
    const area = workArea();
    if (area && overlaps(area, bottom)) {
      top = true;
      return;
    }
    const upper = cornerBox(true)!;
    top = usableUnder(bottom) > usableUnder(upper);
  }
  $effect(() => {
    void shown.length;
    placeToasts();
  });
  $effect(() => {
    const again = () => placeToasts();
    // Once the focus has moved (on focusout it is on nothing for a moment).
    const later = () => setTimeout(placeToasts, 0);
    document.addEventListener('focusin', again);
    document.addEventListener('focusout', later);
    window.addEventListener('resize', again);
    // What is under the corners changes with a scroll of the page, and with a page shown.
    document.addEventListener('scroll', later, { capture: true, passive: true });
    const nav = setInterval(placeToasts, 1500);
    return () => {
      document.removeEventListener('focusin', again);
      document.removeEventListener('focusout', later);
      window.removeEventListener('resize', again);
      document.removeEventListener('scroll', later, { capture: true });
      clearInterval(nav);
    };
  });
</script>

<div class="toasts" class:top bind:this={stack} role="status" aria-live="polite">
  {#if hidden > 0}
    <button type="button" class="more" onclick={() => dismissToast(toasts.list[0].id)} title="Dismiss the oldest">{hidden} more</button>
  {/if}
  {#each shown as toast (toast.id)}
    <div class="toast {toast.kind}">
      <Icon name={toast.kind === 'success' ? 'check' : toast.kind === 'info' ? 'info' : 'alert'} size={18} />
      <div class="grow">
        <strong><SafeText text={toast.title} /></strong>
        {#if toast.message}<p class="break"><SafeText text={toast.message} /></p>{/if}
        {#if toast.actions?.length}
          <div class="acts">
            {#each toast.actions as a (a.label)}
              <button type="button" class="btn small" onclick={() => { dismissToast(toast.id); a.run(); }}>{t(a.label)}</button>
            {/each}
          </div>
        {/if}
      </div>
      <button type="button" class="btn ghost icon small" aria-label="Dismiss" onclick={() => dismissToast(toast.id)}>✕</button>
    </div>
  {/each}
</div>

<style>
  .toasts {
    position: fixed;
    right: 1rem;
    bottom: 3rem;
    z-index: 50;
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    align-items: flex-end;
    width: min(21rem, calc(100vw - 2rem));
    pointer-events: none;
  }
  .toasts.top {
    top: 4rem;
    bottom: auto;
  }
  .toast {
    display: flex;
    align-items: flex-start;
    gap: 0.65rem;
    padding: 0.7rem 0.6rem 0.7rem 0.85rem;
    border: 1px solid var(--line-strong);
    border-left: 4px solid var(--text-muted);
    border-radius: var(--radius-m);
    background: var(--surface);
    box-shadow: var(--shadow);
    pointer-events: auto;
  }
  .toast.success {
    border-left-color: var(--positive);
  }
  .toast.warning {
    border-left-color: var(--warning);
  }
  .toast.error {
    border-left-color: var(--negative);
  }
  .toast {
    width: 100%;
  }
  .toast p {
    margin: 0.15rem 0 0;
    color: var(--text-muted);
    font-size: 0.9rem;
    display: -webkit-box;
    -webkit-line-clamp: 3;
    line-clamp: 3;
    -webkit-box-orient: vertical;
    overflow: hidden;
  }
  .acts {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem;
    margin-top: 0.45rem;
  }
  .more {
    padding: 0.15rem 0.6rem;
    border: 1px solid var(--line-strong);
    border-radius: 999px;
    background: var(--surface);
    color: var(--text-muted);
    font: inherit;
    font-size: 0.8rem;
    pointer-events: auto;
    cursor: pointer;
  }
</style>
