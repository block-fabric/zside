<script lang="ts">
  // The integrated title bar of the dashboard and of the screens before the
  // node runs: a slim strip at the very top with the name of the app and its network, the page (and
  // the wallet), and the buttons of the window. Dragging its empty part moves the window, a double
  // click maximizes it. The workspace layout has these in its own top bar. Not shown with the title
  // bar of the system.
  import Brand from './Brand.svelte';
  import WindowControls from './WindowControls.svelte';
  import { frame, titleBarDoubleClick, titleBarMouseDown } from '../lib/windowframe.svelte';

  let { title = '', detail = '' }: { title?: string; detail?: string } = $props();
  let bar: HTMLElement | undefined = $state();
</script>

{#if frame.integrated}
  <!-- svelte-ignore a11y_no_static_element_interactions (the drag of the window; its buttons are the keyboard way) -->
  <header
    class="titlestrip"
    bind:this={bar}
    onmousedown={(e) => bar && titleBarMouseDown(e, bar)}
    ondblclick={(e) => bar && titleBarDoubleClick(e, bar)}
  >
    <Brand />
    {#if title}
      <span class="title">
        <span class="sep" aria-hidden="true"></span>
        <span class="where">{title}</span>
        {#if detail}<span class="detail">{detail}</span>{/if}
      </span>
    {/if}
    <span class="grow"></span>
    <WindowControls />
  </header>
{/if}

<style>
  .titlestrip {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    flex: none;
    min-height: 2.15rem;
    padding-left: 0.6rem;
    background: color-mix(in srgb, var(--bg) 70%, transparent);
    border-bottom: 1px solid var(--line);
    font-size: 0.86rem;
    user-select: none;
    -webkit-user-select: none;
  }
  .titlestrip :global(.brand) {
    gap: 0.4rem;
    padding: 0 0.2rem;
  }
  .titlestrip :global(.brand .mark) {
    width: 1.25rem;
    height: 1.25rem;
  }
  .titlestrip :global(.brand .name) {
    font-size: 0.92rem;
  }
  .titlestrip :global(.brand .net) {
    font-size: 0.72rem;
  }
  .title {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    min-width: 0;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
  }
  .sep {
    width: 1px;
    height: 0.95rem;
    background: var(--line-strong);
  }
  .where {
    color: var(--text-muted);
    font-weight: 600;
  }
  .detail {
    padding: 0.05rem 0.45rem;
    border-radius: var(--radius-s);
    background: var(--raised);
    color: var(--text-muted);
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .grow {
    flex: 1;
  }
</style>
