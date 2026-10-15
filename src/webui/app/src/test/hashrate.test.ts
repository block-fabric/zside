// The network hash estimator of the Mine page: the units of hash rates, the share of the network, the
// time to a block, the windows and the chart; and the panel against a fake node.
import { fireEvent, render, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import HashEstimator from '../components/HashEstimator.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { blocksPerDay, chartHeights, clearEstimates, estimatesAt, expectedSeconds, formatHashRate, hashParts, hashWindows, networkShare, parseHashRate } from '../lib/hashrate';
import { app, node, prefs } from '../lib/state.svelte';

describe('hash rates', () => {
  it('are shown in the unit they read best in, the decimals apart', () => {
    expect(formatHashRate(0)).toBe('0 H/s');
    expect(formatHashRate(999)).toBe('999 H/s');
    expect(formatHashRate(1500)).toBe('1.50 kH/s');
    expect(formatHashRate(123.456e12)).toBe('123.46 TH/s');
    expect(formatHashRate(5e21)).toBe('5000.00 EH/s');
    expect(formatHashRate(999.999e9)).toBe('1.00 TH/s');
    expect(formatHashRate(NaN)).toBe('–');
    expect(hashParts(12.3456e15)).toEqual({ whole: '12', decimals: '.35', unit: 'PH/s' });
  });

  it('are read as typed, with a unit or in the unit picked', () => {
    expect(parseHashRate('100 TH/s')).toEqual({ rate: 100e12, unit: 'TH/s' });
    expect(parseHashRate('100', 'GH/s')).toEqual({ rate: 100e9, unit: 'GH/s' });
    expect(parseHashRate('2,5 MH/s')).toEqual({ rate: 2.5e6, unit: 'MH/s' });
    expect(parseHashRate('1.5e3 gh')).toEqual({ rate: 1.5e12, unit: 'GH/s' });
    expect(parseHashRate('7 H/s', 'TH/s')).toEqual({ rate: 7, unit: 'H/s' });
    expect(parseHashRate('3 e', 'H/s')).toEqual({ rate: 3e18, unit: 'EH/s' });
    expect(parseHashRate('3 khps')).toEqual({ rate: 3000, unit: 'kH/s' });
    expect(parseHashRate('fast')).toBeNull();
    expect(parseHashRate('-5')).toBeNull();
    expect(parseHashRate('5 ZH/s')).toBeNull();
  });
});

describe('what a hash rate finds', () => {
  it('a share of the network that counts the miner in it', () => {
    expect(networkShare(0, 100)).toBe(0);
    expect(networkShare(100, 300)).toBe(0.25);
    expect(networkShare(5, 0)).toBe(1);
  });

  it('a block in difficulty × 2^32 hashes on average', () => {
    expect(expectedSeconds(1, 2 ** 32)).toBe(1);
    expect(expectedSeconds(1000, 1e12)).toBeCloseTo(4.294967296, 9);
    expect(expectedSeconds(1, 0)).toBe(Infinity);
    expect(blocksPerDay(1, 2 ** 32)).toBe(86400);
    expect(blocksPerDay(1, 0)).toBe(0);
  });

  it('windows of 20 blocks, 2 hours, 12 hours and a day, in the blocks of the chain', () => {
    expect(hashWindows(60).map((w) => w.blocks)).toEqual([20, 120, 720, 1440]);
    expect(hashWindows(600).map((w) => w.blocks)).toEqual([20, 12, 72, 144]);
  });

  it('a chart of points at fixed heights, and the tip', () => {
    expect(chartHeights(105, 60, 20)).toEqual([60, 80, 100, 105]);
    expect(chartHeights(100, 60, 20)).toEqual([60, 80, 100]);
    expect(chartHeights(30, 1440, 20)).toEqual([20, 30]);
    expect(chartHeights(0, 1440, 20)).toEqual([]);
  });

  it('asks the node only for the estimates it does not have (the tip each time)', async () => {
    clearEstimates();
    const asked: string[] = [];
    const ask = async (n: number, h: number) => {
      asked.push(`${n}:${h}`);
      return h;
    };
    expect(await estimatesAt([60, 80, 100, 105], 120, 105, ask)).toEqual([60, 80, 100, 105]);
    asked.length = 0;
    expect(await estimatesAt([60, 80, 100, 106], 120, 106, ask)).toEqual([60, 80, 100, 106]);
    expect(asked).toEqual(['120:106']);
    // Another window is other estimates; a lower tip (a reorganisation) forgets them all.
    asked.length = 0;
    await estimatesAt([60, 80], 20, 106, ask);
    expect(asked).toEqual(['20:60', '20:80']);
    asked.length = 0;
    await estimatesAt([60, 80], 120, 90, ask);
    expect(asked).toEqual(['120:60', '120:80']);
  });
});

describe('the estimator panel', () => {
  let calls: BridgeRequest[];
  beforeEach(() => {
    clearEstimates();
    app.info = { name: 'Chains', version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp/x', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 60 };
    prefs.unit = 'CHN';
    prefs.mask = false;
    node.tip = { height: 200, time: 1_700_000_000, hash: 'ab'.repeat(32), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
    calls = [];
    setTransport(async (r) => {
      calls.push(r);
      const p = (r.params ?? []) as unknown[];
      switch (r.method) {
        case 'getmininginfo':
          return { blocks: 200, difficulty: 1000, next: { height: 201, difficulty: 1000 } };
        case 'getnetworkhashps':
          return Number(p[1]) < 0 ? 3e12 : 2e12 + Number(p[1]) * 1e9;
        case 'getblockstats':
          return { subsidy: 5_000_000_000 };
        default:
          throw { code: -32601, message: `No ${r.method}` };
      }
    });
  });
  afterEach(() => setTransport(null));

  it("shows the network's rate, the difficulty, a chart, and what a rate typed finds", async () => {
    const view = render(HashEstimator);
    await waitFor(() => expect(view.container.querySelector('#estimator-network')!.textContent!.replace(/\s+/g, ' ').trim()).toBe('3.00 TH/s'));
    expect(view.container.querySelector('#estimator-network .dec')!.textContent).toBe('.00');
    expect(view.container.querySelector('#estimator-difficulty')!.textContent).toBe('1000.00');
    // The 2-hour window (120 blocks of 60 s) at the tip, and the chart over the day (a point every 20 blocks).
    await waitFor(() => expect(view.container.querySelector('.chart path.line')).not.toBeNull());
    const asked = calls.filter((c) => c.method === 'getnetworkhashps').map((c) => c.params);
    expect(asked[0]).toEqual([120, -1]);
    expect(asked.slice(1).map((p) => (p as number[])[1])).toEqual([20, 40, 60, 80, 100, 120, 140, 160, 180, 200]);

    const input = view.container.querySelector<HTMLInputElement>('#estimator-rate')!;
    await fireEvent.input(input, { target: { value: '1 TH/s' } });
    await fireEvent.change(input);
    expect((view.container.querySelector('#estimator-unit') as HTMLSelectElement).value).toBe('TH/s');
    expect(input.value).toBe('1');
    // 1 TH/s next to 3 TH/s: a quarter; 1000 × 2^32 / 1e12 s ≈ 4.3 s a block, ≈ 20 116 blocks a day.
    expect(view.container.querySelector('#estimator-share')!.textContent).toBe('25.00%');
    expect(view.container.querySelector('#estimator-time')!.textContent).toMatch(/4 s|seconds/);
    expect(view.container.querySelector('#estimator-blocks')!.textContent).toMatch(/^20\D?117$/);
    expect(view.container.querySelector('#estimator-coins')!.textContent).toMatch(/1\D?005\D?8\d\d\.\d+/);

    // Another window: asked again with it.
    calls.length = 0;
    await fireEvent.change(view.container.querySelector('#estimator-window')!, { target: { value: '20' } });
    await waitFor(() => expect(calls.some((c) => c.method === 'getnetworkhashps' && (c.params as number[])[0] === 20)).toBe(true));
  });

  it("takes this node's rate while it mines, until one is typed", async () => {
    const view = render(HashEstimator, { ownRate: 2500 });
    expect(view.container.querySelector<HTMLInputElement>('#estimator-rate')!.value).toBe('2.5');
    expect((view.container.querySelector('#estimator-unit') as HTMLSelectElement).value).toBe('kH/s');
  });
});
