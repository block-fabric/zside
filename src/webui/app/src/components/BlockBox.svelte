<script lang="ts">
  // One block of the ribbon, drawn as a box: a lid and a side (two skewed faces, no 3D), and its face —
  // the height, the reward (subsidy and fees), the miner's tag of its coinbase, the sidechains it merged
  // mined (a sidechain: the block of the mainchain that merged mined it), the time since the block before it (coloured against the recent pace of the chain) and how long
  // ago. A fill rises from the floor of the face: its weight against the most a block of Chains may have,
  // tinted by the fee rate it paid. After the ribbon of the user's pool site (epool.cash).
  import { t } from '../lib/i18n.svelte';
  import Amount from './Amount.svelte';
  import SafeText from './SafeText.svelte';
  import { css } from '../lib/actions';
  import { feeTint, fillPercent, intervalTone, shortInterval, type RibbonBlock } from '../lib/blocks';
  import { formatCount, niceTimeOffset } from '../lib/format';
  import { CHAIN } from '../chain/config';

  let {
    block,
    secs,
    median,
    now,
    selected = false,
    fresh = false,
    onselect,
  }: {
    block: RibbonBlock;
    /** Seconds since the block before it, or null. */
    secs: number | null;
    /** The recent pace of the chain (median interval), or null. */
    median: number | null;
    /** Now, in seconds (for "how long ago"). */
    now: number;
    selected?: boolean;
    fresh?: boolean;
    onselect: (hash: string) => void;
  } = $props();

  const mainName = CHAIN.mainchain?.name ?? '';

  const fill = $derived(fillPercent(block.weight, block.nTx));
  const tint = $derived(feeTint(block.feeRate));
  const tone = $derived(intervalTone(secs, median));
  const reward = $derived(block.subsidy === null ? null : block.subsidy + (block.fees ?? 0));
  const ago = $derived(niceTimeOffset(Math.max(0, now - block.time)));
  const label = $derived(
    `${t('Block')} ${formatCount(block.height)}: ${formatCount(block.nTx)} ${t('transactions')}, ${Math.round(fill)}% ${t('full')}, ${ago} ${t('ago')}` +
      (block.bmm.length ? `, ${t('merged mined by')} ${block.bmm.length} ${t('sidechains')}` : '') +
      (block.main ? `, ${t('committed to by block')} ${formatCount(block.main.height)} ${t('of')} ${mainName}` : ''),
  );
</script>

<button type="button" class="blk" class:is-open={selected} class:is-fresh={fresh} aria-pressed={selected} aria-label={label} title={label} onclick={() => onselect(block.hash)}>
  <span class="blk-lid" aria-hidden="true"></span>
  <span class="blk-side" aria-hidden="true"></span>
  <span class="blk-face" use:css={{ '--fill': `${fill}%` }} data-tint={tint}>
    <span class="height mono">{formatCount(block.height)}</span>
    <span class="reward" title={block.fees ? t('Newly issued, and the fees of its transactions') : t('No fees in this block')}>
      {#if reward === null}<span class="faint">–</span>{:else}<Amount sats={reward} masked={false} />{/if}
    </span>
    <span class="who">
      <span class="tag" class:untagged={!block.tag}>{#if block.tag}<SafeText text={block.tag} title={block.tag} />{:else}{block.tag === null ? '…' : t('no tag')}{/if}</span>
      <span class="txs faint">{formatCount(block.nTx)} tx · {formatCount(Math.round(block.weight / 1000))} kWU</span>
    </span>
    {#if block.main}
      <!-- A sidechain: the block of the mainchain that merged mined it (every block of a sidechain is). -->
      <span class="bmm" aria-hidden="true"><span class="badge accent" title="{t('Merged mined: committed to by block')} {formatCount(block.main.height)} {t('of')} {mainName}">{mainName} {formatCount(block.main.height)}</span></span>
    {:else if block.bmm.length}
      <span class="bmm" aria-hidden="true">
        {#each block.bmm.slice(0, 4) as slot (slot)}<span class="badge accent" title="{t('Merged mined: sidechain')} {slot}">BMM {slot}</span>{/each}
        {#if block.bmm.length > 4}<span class="badge">+{block.bmm.length - 4}</span>{/if}
      </span>
    {/if}
    <span class="foot">
      <span class="interval mono {tone}" title={t('Time since the block before it')}>{shortInterval(secs)}</span>
      <span class="ago faint">{ago}</span>
    </span>
  </span>
</button>

<style>
  /* Two skewed faces give the box its depth: a bar skewed about its bottom edge is the lid, a column
     skewed about its left edge the side. Only colours of the look; nothing animated but the arrival. */
  .blk {
    --blk-depth: 12px;
    position: relative;
    flex: none;
    width: 10.5rem;
    margin: var(--blk-depth) var(--blk-depth) 0 0;
    padding: 0;
    border: 0;
    background: none;
    color: inherit;
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .blk:focus-visible {
    outline: none;
  }
  .blk:focus-visible .blk-face {
    outline: 2px solid var(--focus);
    outline-offset: 2px;
  }
  .blk-face {
    position: relative;
    z-index: 2;
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
    min-height: 9.6rem;
    padding: 0.7rem 0.7rem 0.55rem;
    border: 1px solid var(--line-strong);
    border-radius: 3px;
    /* The fill: strongest at the floor, fading out where the block's weight ends. */
    --tint: transparent;
    background:
      linear-gradient(
        to top,
        color-mix(in srgb, var(--tint) 30%, transparent) 0%,
        color-mix(in srgb, var(--tint) 12%, transparent) calc(var(--fill) * 0.6),
        transparent var(--fill)
      ),
      linear-gradient(180deg, var(--surface), var(--surface-2));
  }
  .blk-face[data-tint='low'] {
    --tint: var(--positive);
  }
  .blk-face[data-tint='mid'] {
    --tint: var(--accent);
  }
  .blk-face[data-tint='high'] {
    --tint: var(--warning);
  }
  .blk-face[data-tint='none'] {
    --tint: var(--text-faint);
  }
  .blk-lid {
    position: absolute;
    left: 0;
    top: calc(var(--blk-depth) * -1);
    width: 100%;
    height: var(--blk-depth);
    transform: skewX(-45deg);
    transform-origin: bottom left;
    background: linear-gradient(180deg, var(--accent), color-mix(in srgb, var(--accent) 65%, var(--surface)));
    border: 1px solid color-mix(in srgb, var(--accent) 70%, var(--line-strong));
    border-bottom: 0;
  }
  .blk-side {
    position: absolute;
    right: calc(var(--blk-depth) * -1);
    top: 0;
    width: var(--blk-depth);
    height: 100%;
    transform: skewY(-45deg);
    transform-origin: top left;
    background: linear-gradient(100deg, color-mix(in srgb, var(--line-strong) 70%, var(--bg)), var(--bg));
    border: 1px solid var(--line-strong);
    border-left: 0;
  }
  .blk:hover .blk-face {
    border-color: var(--text-faint);
  }
  .blk.is-open .blk-face {
    border-color: var(--accent-fg);
    box-shadow: 0 0 0 1px var(--accent-fg);
  }
  .blk.is-open .blk-lid,
  .blk.is-open .blk-side {
    border-color: var(--accent-fg);
  }
  .height {
    font-size: 1.05rem;
    font-weight: 650;
    color: var(--text);
  }
  .reward {
    font-size: 0.9rem;
    color: var(--text);
  }
  .who {
    display: flex;
    flex-direction: column;
    gap: 0.1rem;
    min-width: 0;
    font-size: 0.78rem;
  }
  .tag {
    color: var(--accent-fg);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .tag.untagged {
    color: var(--text-faint);
  }
  .txs {
    white-space: nowrap;
  }
  .bmm {
    display: flex;
    flex-wrap: wrap;
    gap: 0.2rem;
  }
  .bmm .badge {
    padding: 0 0.35rem;
    font-size: 0.68rem;
  }
  .foot {
    display: flex;
    justify-content: space-between;
    gap: 0.4rem;
    margin-top: auto;
    padding-top: 0.4rem;
    border-top: 1px solid var(--line);
    font-size: 0.76rem;
  }
  .ago {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .interval.fast {
    color: var(--positive);
  }
  .interval.slow {
    color: var(--warning);
  }
  .interval.normal {
    color: var(--text-muted);
  }
  .interval.unknown {
    color: var(--text-faint);
  }
  /* A block that came while the page is shown: it slides in once (transform and opacity only: cheap). */
  .blk.is-fresh {
    animation: blk-in 600ms ease-out both;
  }
  @keyframes blk-in {
    from {
      opacity: 0;
      transform: translateX(-1.5rem);
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .blk.is-fresh {
      animation: none;
    }
  }
</style>
