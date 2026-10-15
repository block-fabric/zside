import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import BlockBox from '../components/BlockBox.svelte';
import BlockRibbon from '../components/BlockRibbon.svelte';
import { setTransport } from '../lib/bridge';
import { CHAIN } from '../chain/config';
import { formatCount } from '../lib/format';
import {
  BLOCK_WEIGHT,
  CHAINS_MAX_BLOCK_WEIGHT,
  coinbaseTag,
  expectedSeconds,
  feeTint,
  fillPercent,
  intervalOf,
  intervalTone,
  medianInterval,
  shortInterval,
  type RibbonBlock,
} from '../lib/blocks';

setTransport(async () => true);

// The block ribbon of the explorer (after the user's pool site): the fill, its tint, the interval colour.

const block = (over: Partial<RibbonBlock> = {}): RibbonBlock => ({
  height: 120,
  hash: 'ab'.repeat(32),
  time: 1_700_000_000,
  nTx: 41,
  weight: 400_000,
  subsidy: 5_000_000_000,
  fees: 123_456,
  feeRate: 3.2,
  tag: 'my pool',
  bmm: [],
  ...over,
});

describe('what a box shows', () => {
  it('the fill: the weight against the 800,000 of a block of Chains, none for the coinbase alone', () => {
    expect(CHAINS_MAX_BLOCK_WEIGHT).toBe(800_000);
    expect(fillPercent(400_000, 41)).toBe(50);
    expect(fillPercent(800_000, 900)).toBe(100);
    expect(fillPercent(900_000, 900)).toBe(100);
    // A little is still seen.
    expect(fillPercent(2_000, 2)).toBe(6);
    // The coinbase alone: an empty block.
    expect(fillPercent(900, 1)).toBe(0);
  });

  it('the tint: what the transactions paid', () => {
    expect(feeTint(null)).toBe('none');
    expect(feeTint(0)).toBe('none');
    expect(feeTint(1)).toBe('low');
    expect(feeTint(2)).toBe('mid');
    expect(feeTint(25)).toBe('high');
  });

  it('the interval: against the recent pace of the chain, not a fixed threshold', () => {
    const list = [{ time: 1000 }, { time: 940 }, { time: 880 }, { time: 700 }, { time: 690 }];
    expect(intervalOf(list, 0)).toBe(60);
    expect(intervalOf(list, 4)).toBeNull();
    expect(medianInterval(list)).toBe(60);
    expect(intervalTone(60, 60)).toBe('normal');
    expect(intervalTone(10, 60)).toBe('fast');
    expect(intervalTone(180, 60)).toBe('slow');
    expect(intervalTone(null, 60)).toBe('unknown');
    expect(intervalTone(30, null)).toBe('unknown');
    expect(shortInterval(42)).toBe('42s');
    expect(shortInterval(185)).toBe('3m 5s');
    expect(shortInterval(5400)).toBe('1.5h');
  });

  it("the miner's tag: the text of the coinbase after the height", () => {
    // Height 120 (push of 1 byte), then "/my pool/" and an extranonce.
    const hex = '0178' + Buffer.from('/my pool/').toString('hex') + '00ff01';
    expect(coinbaseTag(hex)).toBe('/my pool/');
    expect(coinbaseTag('0178' + '0001')).toBe('');
    expect(coinbaseTag('zz')).toBe('');
    expect(coinbaseTag('0178' + Buffer.from('x'.repeat(80)).toString('hex')).length).toBe(40);
  });

  it('the expected time to the next block', () => {
    expect(expectedSeconds(1, 2 ** 32 / 60)).toBeCloseTo(60);
    expect(expectedSeconds(0, 10)).toBeNull();
    expect(expectedSeconds(1, 0)).toBeNull();
    // A guess far off the pace (blocks made at once on a test chain): nothing.
    expect(expectedSeconds(4.6e-10, 1e-6)).toBeNull();
  });
});

describe('the box', () => {
  it('draws the fill, its tint, the interval colour, the tag as text of the chain, the BMM badges', async () => {
    const onselect = vi.fn();
    const b = block({ bmm: [2, 5], tag: '<b>Confirmed</b>' });
    const { container } = render(BlockBox, { block: b, secs: 10, median: 60, now: 1_700_000_120, onselect });
    const face = container.querySelector('.blk-face') as HTMLElement;
    // Against the weight a block of this chain may have (chain.json blockWeight): half of a block of Chains.
    expect(face.style.getPropertyValue('--fill')).toBe(BLOCK_WEIGHT === CHAINS_MAX_BLOCK_WEIGHT ? '50%' : `${fillPercent(b.weight ?? 0, b.nTx, BLOCK_WEIGHT)}%`);
    expect(face.dataset.tint).toBe('mid');
    expect(container.querySelector('.interval')!.classList.contains('fast')).toBe(true);
    expect(container.querySelector('.interval')!.textContent).toBe('10s');
    const tag = container.querySelector('.tag [translate="no"]') as HTMLElement;
    expect(tag.textContent).toBe('<b>Confirmed</b>');
    expect(container.querySelector('.tag b')).toBeNull();
    expect([...container.querySelectorAll('.bmm .badge')].map((b) => b.textContent)).toEqual(['BMM 2', 'BMM 5']);
    expect(container.textContent).toContain('50.00123456');
    const button = screen.getByRole('button', { name: /Block 120/ });
    expect(button.getAttribute('aria-pressed')).toBe('false');
    await fireEvent.click(button);
    expect(onselect).toHaveBeenCalledWith('ab'.repeat(32));
  });

  it('a sidechain: the block of the mainchain that merged mined it, in place of the BMM badges', () => {
    const main = CHAIN.mainchain?.name ?? '';
    const { container } = render(BlockBox, { block: block({ main: { height: 1234, hash: 'cd'.repeat(32) } }), secs: 60, median: 60, now: 1_700_000_060, onselect: () => undefined });
    expect([...container.querySelectorAll('.bmm .badge')].map((b) => b.textContent)).toEqual([`${main} ${formatCount(1234)}`]);
    expect(screen.getByRole('button', { name: /Block 120/ }).getAttribute('aria-label')).toContain(`committed to by block ${formatCount(1234)} of ${main}`);
  });

  it('draws the fill against the weight the node says for its network (app.info.blockWeight)', async () => {
    const { app } = await import('../lib/state.svelte');
    const before = app.info;
    app.info = { ...(before ?? {}), blockWeight: 4_000_000 } as never;
    try {
      const { container } = render(BlockBox, { block: block({ weight: 1_000_000 }), secs: 60, median: 60, now: 1_700_000_060, onselect: () => undefined });
      expect((container.querySelector('.blk-face') as HTMLElement).style.getPropertyValue('--fill')).toBe('25%');
    } finally {
      app.info = before;
    }
    // The host says it (app.cpp, consensus.max_block_weight of the network).
    const { readFileSync } = await import('node:fs');
    const { join } = await import('node:path');
    expect(readFileSync(join(__dirname, '../../../app.cpp'), 'utf8')).toMatch(/info\.pushKV\("blockWeight", Params\(\)\.GetConsensus\(\)\.max_block_weight\)/);
  });

  it('an empty block, a slow one, a block without its stats yet', () => {
    const { container } = render(BlockBox, { block: block({ nTx: 1, weight: 900, feeRate: 0, subsidy: null, fees: null, tag: null }), secs: 200, median: 60, now: 1_700_000_000, selected: true, onselect: () => undefined });
    const face = container.querySelector('.blk-face') as HTMLElement;
    expect(face.style.getPropertyValue('--fill')).toBe('0%');
    expect(face.dataset.tint).toBe('none');
    expect(container.querySelector('.interval')!.classList.contains('slow')).toBe(true);
    expect(container.querySelector('.blk')!.getAttribute('aria-pressed')).toBe('true');
  });
});

describe('the ribbon', () => {
  it('the blocks newest first (no box for the block being mined); the arrows move between them', async () => {
    const blocks = [block({ height: 122, hash: 'cc'.repeat(32), time: 1_700_000_120 }), block({ height: 121, hash: 'bb'.repeat(32), time: 1_700_000_060 }), block()];
    const { container } = render(BlockRibbon, {
      blocks,
      selected: 'bb'.repeat(32),
      fresh: 'cc'.repeat(32),
      now: 1_700_000_130,
      onselect: () => undefined,
    });
    const boxes = [...container.querySelectorAll<HTMLElement>('button.blk')];
    expect(boxes.map((b) => b.querySelector('.height')!.textContent)).toEqual(['122', '121', '120']);
    expect(container.querySelector('.blk-next')).toBeNull();
    expect(boxes[0].classList.contains('is-fresh')).toBe(true);
    expect(boxes[1].getAttribute('aria-pressed')).toBe('true');
    expect(container.textContent).toContain('median 1m 0s');
    boxes[0].focus();
    await fireEvent.keyDown(container.querySelector('.strip')!, { key: 'ArrowRight' });
    expect(document.activeElement).toBe(boxes[1]);
  });

  it('scrolled near its right end, the strip asks for older blocks; the wheel scrolls it sideways', async () => {
    const blocks = Array.from({ length: 30 }, (_, i) => block({ height: 200 - i, hash: (i + 10).toString(16).padStart(2, '0').repeat(32), time: 1_700_000_000 - i * 60 }));
    const onmore = vi.fn();
    const { container } = render(BlockRibbon, { blocks, selected: null, fresh: null, now: 1_700_000_100, onselect: () => undefined, onmore });
    const strip = container.querySelector('.strip') as HTMLElement;
    // jsdom has no layout: give the strip a size and a scroll range.
    Object.defineProperty(strip, 'clientWidth', { value: 800, configurable: true });
    Object.defineProperty(strip, 'scrollWidth', { value: 5000, configurable: true });
    strip.scrollLeft = 0;
    await fireEvent.wheel(strip, { deltaY: 300 });
    expect(strip.scrollLeft).toBe(300);
    expect(onmore).not.toHaveBeenCalled();
    strip.scrollLeft = 4000;
    await fireEvent.scroll(strip);
    expect(onmore).toHaveBeenCalled();
  });
});
