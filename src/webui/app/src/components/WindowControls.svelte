<script lang="ts">
  // The buttons of the integrated title bar, at its right: minimize, maximize or restore (its
  // picture follows the state of the window), close (red under the pointer, as on the desktops).
  import { closeWindow, frame, minimizeWindow, toggleMaximize } from '../lib/windowframe.svelte';
</script>

<div class="window-controls" role="group" aria-label="Window">
  <button type="button" class="wc" aria-label="Minimize" title="Minimize" onclick={minimizeWindow}>
    <svg viewBox="0 0 10 10" aria-hidden="true"><path d="M1 5.5h8" /></svg>
  </button>
  <button
    type="button"
    class="wc"
    aria-label={frame.maximized ? 'Restore' : 'Maximize'}
    title={frame.maximized ? 'Restore' : 'Maximize'}
    data-state={frame.maximized ? 'maximized' : 'normal'}
    onclick={toggleMaximize}
  >
    {#if frame.maximized}
      <svg viewBox="0 0 10 10" aria-hidden="true"><path d="M1 3h6v6H1zM3 3V1h6v6H7" /></svg>
    {:else}
      <svg viewBox="0 0 10 10" aria-hidden="true"><path d="M1 1h8v8H1z" /></svg>
    {/if}
  </button>
  <button type="button" class="wc close" aria-label="Close" title="Close" onclick={closeWindow}>
    <svg viewBox="0 0 10 10" aria-hidden="true"><path d="M1 1l8 8M9 1L1 9" /></svg>
  </button>
</div>

<style>
  .window-controls {
    display: flex;
    align-self: stretch;
    flex: none;
  }
  .wc {
    display: grid;
    place-items: center;
    width: 2.9rem;
    min-height: 2rem;
    padding: 0;
    border: 0;
    border-radius: 0;
    background: none;
    color: var(--text-muted);
    cursor: default;
  }
  .wc svg {
    width: 0.72rem;
    height: 0.72rem;
    fill: none;
    stroke: currentColor;
    stroke-width: 1.1;
    shape-rendering: crispEdges;
  }
  .wc.close svg {
    shape-rendering: geometricPrecision;
  }
  .wc:hover {
    background: var(--raised);
    color: var(--text);
  }
  .wc:active {
    background: var(--line);
  }
  .wc.close:hover,
  .wc.close:focus-visible {
    background: #c42b1c;
    color: #fff;
  }
  .wc.close:active {
    background: #a32418;
    color: #fff;
  }
  .wc:focus-visible {
    outline: 2px solid var(--focus);
    outline-offset: -2px;
  }
</style>
