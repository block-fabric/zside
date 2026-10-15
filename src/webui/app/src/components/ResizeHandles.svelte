<script lang="ts">
  // The edges of a window without the frame of the system: thin strips along them and squares at
  // the corners, invisible, that resize the window when dragged (not when it is maximized).
  import { frame, startResize, WINDOW_EDGES, type WindowEdge } from '../lib/windowframe.svelte';

  function press(e: MouseEvent, edge: WindowEdge) {
    if (e.button !== 0) return;
    e.preventDefault();
    startResize(edge);
  }
</script>

{#if frame.integrated && !frame.maximized}
  {#each WINDOW_EDGES as edge (edge)}
    <div class="resize {edge}" data-edge={edge} aria-hidden="true" onmousedown={(e) => press(e, edge)}></div>
  {/each}
{/if}

<style>
  .resize {
    position: fixed;
    z-index: 200;
    --edge: 4px;
    --corner: 10px;
  }
  .top,
  .bottom {
    left: var(--corner);
    right: var(--corner);
    height: var(--edge);
    cursor: ns-resize;
  }
  .left,
  .right {
    top: var(--corner);
    bottom: var(--corner);
    width: var(--edge);
    cursor: ew-resize;
  }
  .top {
    top: 0;
  }
  .bottom {
    bottom: 0;
  }
  .left {
    left: 0;
  }
  .right {
    right: 0;
  }
  .top-left,
  .top-right,
  .bottom-left,
  .bottom-right {
    width: var(--corner);
    height: var(--corner);
  }
  .top-left {
    top: 0;
    left: 0;
    cursor: nwse-resize;
  }
  .bottom-right {
    bottom: 0;
    right: 0;
    cursor: nwse-resize;
  }
  .top-right {
    top: 0;
    right: 0;
    cursor: nesw-resize;
  }
  .bottom-left {
    bottom: 0;
    left: 0;
    cursor: nesw-resize;
  }
</style>
