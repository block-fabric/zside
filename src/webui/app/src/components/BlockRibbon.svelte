<script lang="ts">
  // The latest blocks as a chain of boxes, newest at the left (no box for the block being mined: user
  // decision). The strip scrolls sideways (the wheel too), and asks for older blocks (onmore) as it nears
  // its right end; the arrows move between the boxes. After the ribbon of the user's pool site
  // (epool.cash): components/BlockBox.svelte, lib/blocks.ts.
  import { t } from '../lib/i18n.svelte';
  import BlockBox from './BlockBox.svelte';
  import { intervalOf, medianInterval, shortInterval, type RibbonBlock } from '../lib/blocks';


  let {
    blocks,
    selected,
    fresh,
    now,
    onselect,
    onmore,
  }: {
    blocks: readonly RibbonBlock[];
    selected: string | null;
    /** The block that came while the page is shown (it slides in once). */
    fresh: string | null;
    now: number;
    onselect: (hash: string) => void;
    /** Older blocks wanted: the strip scrolled near its right end. */
    onmore?: () => void;
  } = $props();

  /** Near the right end (within two boxes' width): ask for older blocks. */
  function nearEnd(strip: HTMLElement) {
    if (onmore && strip.scrollLeft + strip.clientWidth >= strip.scrollWidth - 320) onmore();
  }

  /** The wheel scrolls the strip sideways (a vertical wheel over it would do nothing). */
  function wheel(e: WheelEvent) {
    const strip = e.currentTarget as HTMLElement;
    if (Math.abs(e.deltaY) <= Math.abs(e.deltaX) || strip.scrollWidth <= strip.clientWidth) return;
    e.preventDefault();
    strip.scrollLeft += e.deltaY;
    nearEnd(strip);
  }

  const median = $derived(medianInterval(blocks));

  function keys(e: KeyboardEvent) {
    if (e.key !== 'ArrowRight' && e.key !== 'ArrowLeft') return;
    const boxes = [...(e.currentTarget as HTMLElement).querySelectorAll<HTMLElement>('button.blk')];
    const i = boxes.indexOf(document.activeElement as HTMLElement);
    if (i < 0) return;
    e.preventDefault();
    const to = boxes[Math.max(0, Math.min(boxes.length - 1, i + (e.key === 'ArrowRight' ? 1 : -1)))];
    to.focus();
    to.scrollIntoView?.({ block: 'nearest', inline: 'nearest' });
  }
</script>

<section class="ribbon" aria-label="Latest blocks">
  <div class="ribbon-head">
    <h2 class="caps">{t('Latest blocks')}</h2>
    <span class="faint mono">{median !== null ? `${t('median')} ${shortInterval(median)} ${t('between blocks')} · ${t('target')} 1m` : t('newest first')}</span>
  </div>
  <!-- svelte-ignore a11y_no_noninteractive_element_interactions (the arrows between the boxes; each box is a button) -->
  <div class="strip" role="list" onkeydown={keys} onwheel={wheel} onscroll={(e) => nearEnd(e.currentTarget as HTMLElement)}>
    {#each blocks as b, i (b.hash)}
      <div class="item" role="listitem">
        {#if i > 0}<span class="link" aria-hidden="true"><i></i></span>{/if}
        <BlockBox block={b} secs={intervalOf(blocks, i)} {median} {now} selected={selected === b.hash} fresh={fresh === b.hash} {onselect} />
      </div>
    {/each}
  </div>
</section>

<style>
  .ribbon {
    margin: 0.9rem 0 1rem;
  }
  .ribbon-head {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 0.8rem;
    margin-bottom: 0.3rem;
  }
  .ribbon-head h2 {
    margin: 0;
    font-size: 0.78rem;
  }
  .ribbon-head span {
    font-size: 0.8rem;
  }
  /* Room above for the lids, at the right for the last side; the page never scrolls sideways. */
  .strip {
    display: flex;
    max-width: 100%;
    overflow-x: auto;
    overflow-y: hidden;
    padding: 2px 14px 12px 2px;
  }
  .item {
    display: flex;
    align-items: stretch;
    flex: none;
  }
  .link {
    display: flex;
    align-items: center;
    flex: none;
    width: 1.1rem;
    margin-top: 12px;
  }
  .link i {
    display: block;
    width: 100%;
    height: 2px;
    background: var(--chain-link);
  }
</style>
