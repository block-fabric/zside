<script lang="ts">
  // The sync overlay (ModalOverlay of chains-qt): while the node catches up, the blocks left, the
  // time of the last block, the progress, its speed per hour and the time left; Hide (or Escape).
  import { every } from '../lib/poll';
  import { formatTime } from '../lib/format';
  import { nav } from '../lib/nav.svelte';
  import { app, node, walletsEnabled } from '../lib/state.svelte';
  import { MAX_BLOCK_TIME_GAP } from '../lib/sync';
  import { blocksLeft, estimate, overlay } from '../lib/syncoverlay';

  // Only while it is shown: the clock of its estimate, and the watch on the other dialogs (a watch on the
  // whole page would run at each change of it, as each row of a long table scrolled in).
  const shown = $derived(nav.dialog === 'sync');
  let now = $state(Date.now());
  $effect(() => {
    if (!shown) return;
    now = Date.now();
    return every(1000, () => (now = Date.now()));
  });

  const tip = $derived(node.tip);
  const est = $derived(estimate(node.progressSamples, now));
  const left = $derived(
    tip ? blocksLeft(tip.height, node.header?.height ?? tip.height, node.header?.time ?? 0, app.info?.blockSpacing ?? 600, Math.floor(now / 1000), node.presync ?? undefined) : 'Unknown…',
  );
  const pct = $derived(tip ? (tip.progress * 100).toFixed(2) : '0.00');

  // Another dialog is open (a page asks something): the layer steps aside until it closes.
  let othersOpen = $state(false);
  $effect(() => {
    if (!shown || typeof MutationObserver === 'undefined') return;
    const look = () => (othersOpen = document.querySelectorAll('dialog[open]:not(.layer)').length > 0);
    const o = new MutationObserver(look);
    o.observe(document.body, { subtree: true, childList: true, attributes: true, attributeFilter: ['open'] });
    look();
    return () => o.disconnect();
  });

  // Shown: the keyboard goes to it, unless the user is typing somewhere (ModalOverlay focuses its button).
  let card: HTMLDivElement | undefined = $state();
  $effect(() => {
    if (card && (document.activeElement === document.body || document.activeElement === null)) card.focus();
  });

  function close() {
    // Closed by the user: it no longer opens by itself (ModalOverlay userClosed).
    if (nav.dialog === 'sync') overlay.userClosed = true;
    overlay.shown = false;
    nav.dialog = null;
  }

  // As BitcoinGUI::setNumBlocks with a wallet: the overlay shows by itself while the node is behind,
  // unless the user closed it, and goes once the node has caught up (overlay.shown: it is open because it
  // opened by itself; one the user opened stays until the user closes it).
  const behind = $derived(!!tip && Math.floor(now / 1000) - tip.time >= MAX_BLOCK_TIME_GAP);
  $effect(() => {
    const open = nav.dialog === 'sync';
    if (!open) overlay.shown = false;
    if (!tip || !walletsEnabled()) return;
    if (behind) {
      if (nav.dialog === null && !overlay.userClosed) {
        overlay.shown = true;
        nav.dialog = 'sync';
      }
    } else if (open && overlay.shown) {
      overlay.shown = false;
      nav.dialog = null;
    }
  });
</script>

<svelte:window
  onkeydown={(e) => {
    // Escape hides it, when no dialog of a page is open above it.
    if (e.key === 'Escape' && nav.dialog === 'sync' && document.querySelectorAll('dialog[open]').length === 1) {
      e.preventDefault();
      close();
    }
  }}
/>

{#if nav.dialog === 'sync'}
  <!-- A layer over the pages, as ModalOverlay covers the wallet view of chains-qt: the menus, the status bar
       and the dialogs of the app stay above it. -->
  <!-- Not modal: a layer over the pages only; it steps aside while a dialog is open above it. -->
  <dialog open={!othersOpen} class="layer" aria-labelledby="sync-title">
    <div class="card" bind:this={card} role="document" tabindex="-1">
      <header>
        <h2 id="sync-title">Synchronizing with the network</h2>
        <button type="button" class="btn ghost icon small" aria-label="Close" onclick={close}>✕</button>
      </header>
      <div class="body">
        <p>Recent transactions may not yet be visible, and therefore your wallet's balance might be incorrect. This information will be correct once your wallet has finished synchronizing with the network, as detailed below.</p>
        <p class="warn">Attempting to spend coins that are affected by not-yet-displayed transactions will not be accepted by the network.</p>
        <progress max="100" value={Number(pct)} aria-label="Progress"></progress>
        <dl class="facts">
          <dt>Number of blocks left</dt><dd id="sync-left">{left}</dd>
          <dt>Last block time</dt><dd>{tip ? formatTime(tip.time) : 'Unknown…'}</dd>
          <dt>Progress</dt><dd class="num">{pct}%</dd>
          <dt>Progress increase per hour</dt><dd class="num">{est.perHour}</dd>
          <dt>Estimated time left until synced</dt><dd>{est.timeLeft}</dd>
        </dl>
      </div>
      <footer><button type="button" class="btn primary" onclick={close}>Hide</button></footer>
    </div>
  </dialog>
{/if}

<style>
  .layer {
    position: absolute;
    inset: 0;
    z-index: 20;
    display: grid;
    place-items: center;
    width: 100%;
    height: 100%;
    max-width: none;
    max-height: none;
    margin: 0;
    padding: 1rem;
    border: 0;
    background: var(--overlay);
    color: var(--text);
    overflow: auto;
  }
  .card {
    width: min(34rem, 100%);
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-l);
    background: var(--surface);
    color: var(--text);
    box-shadow: var(--shadow);
    overflow: hidden;
  }
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 1rem;
    padding: 1rem 1.25rem 0.5rem;
  }
  h2 {
    margin: 0;
    font-size: 1.1rem;
  }
  .body {
    padding: 0.5rem 1.25rem 1rem;
  }
  footer {
    display: flex;
    justify-content: flex-end;
    padding: 0.85rem 1.25rem;
    border-top: 1px solid var(--line);
    background: var(--surface-2);
  }
  .warn {
    color: var(--warning);
  }
  progress {
    width: 100%;
    margin: 0.4rem 0 1rem;
  }
</style>
