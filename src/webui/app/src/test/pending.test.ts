// The progress of a registration in two steps (lib/pending.svelte.ts, PendingCard, the indicator of the top bar) and the
// helpers of the one question for both steps (lib/dialogs.svelte.ts: walletRpcTwoSteps, walletRpcIfAuthorized).
import { render, screen } from '@testing-library/svelte';
import { afterEach, describe, expect, it } from 'vitest';
import PendingCard from '../components/PendingCard.svelte';
import PendingIndicator from '../components/PendingIndicator.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { walletRpcIfAuthorized, walletRpcTwoSteps } from '../lib/dialogs.svelte';
import { activeOps, blockSeconds, blocksLeft, etaSeconds, etaText, pending, progressOf, publishPending, type PendingOp } from '../lib/pending.svelte';

const op = (o: Partial<PendingOp> = {}): PendingOp => ({ key: 'k', what: 'Asset', name: 'GOLD', wallet: 'w', stage: 'waiting', depth: 1, needed: 3, ...o });

afterEach(() => {
  setTransport(null);
  pending.bySource = {};
});

describe('the time until a registration', () => {
  it('counts the blocks the reservation still needs and the one that registers it', () => {
    expect(blocksLeft(op({ stage: 'reserved', depth: null }))).toBe(4);
    expect(blocksLeft(op({ depth: 1 }))).toBe(3);
    expect(blocksLeft(op({ depth: 3 }))).toBe(1);
    expect(blocksLeft(op({ stage: 'registering' }))).toBe(1);
    expect(blocksLeft(op({ stage: 'confirm' }))).toBe(0);
    expect(etaSeconds(op({ depth: 0 }), 15)).toBe(60);
    expect(etaSeconds(op({ stage: 'done' }), 15)).toBeNull();
    expect(etaText(45)).toBe('~45 s');
    expect(etaText(400)).toBe('~7 min');
  });

  it('takes the pace of the last blocks, else the target of the chain', () => {
    expect(blockSeconds([], 15)).toBe(15);
    expect(blockSeconds([{ time: 0 }, { time: 20 }, { time: 40 }, { time: 100 }, { time: 120 }], 60)).toBe(20);
  });

  it('moves forward with each step', () => {
    const stages: PendingOp[] = [op({ stage: 'reserved', depth: null }), op({ depth: 0 }), op({ depth: 2 }), op({ stage: 'registering', depth: 3 }), op({ stage: 'done' })];
    const p = stages.map(progressOf);
    for (let i = 1; i < p.length; i++) expect(p[i]).toBeGreaterThan(p[i - 1]);
    expect(p.at(-1)).toBe(1);
  });
});

describe('the card of a registration', () => {
  it('says the steps: reserved, registering by itself in so long, registered', () => {
    render(PendingCard, { op: op({ depth: 1, needed: 3 }) });
    expect(screen.getByRole('status').textContent).toMatch(/Registering by itself in ~\d+ (s|min) \(block 2 of 3\)/);
    expect(document.querySelector('.steps')!.textContent).toContain('Reserved ✓');
    expect(screen.getByRole('progressbar')).toBeTruthy();
  });

  it('offers what the user can do when the app needs it', () => {
    let ran = 0;
    render(PendingCard, { op: op({ stage: 'confirm', action: { label: 'Confirm registration', run: () => ran++ } }) });
    screen.getByRole('button', { name: 'Confirm registration' }).click();
    expect(ran).toBe(1);
  });

  it('shows in the top bar while one is under way, and a moment after it is done', () => {
    publishPending('test', [op()]);
    const { unmount } = render(PendingIndicator);
    expect(screen.getByRole('button', { name: /1 registration under way/ })).toBeTruthy();
    unmount();
    const now = Date.now();
    expect(activeOps(now, [op({ stage: 'done', ended: now - 1000 })])).toHaveLength(1);
    expect(activeOps(now, [op({ stage: 'done', ended: now - 120_000 })])).toHaveLength(0);
  });
});

describe('one question for both steps', () => {
  it('asks the host for the first step with its second (then), and says when it was declined', async () => {
    const calls: BridgeRequest[] = [];
    setTransport(async (r) => {
      calls.push(r);
      return { txid: 'a'.repeat(64), authorized: 3600 };
    });
    const done = await walletRpcTwoSteps('w', 'reserveasset', ['GOLD'], { method: 'registerasset', params: ['GOLD', '1000', 2, {}, true] });
    expect(done?.authorized).toBe(3600);
    expect(calls[0]).toMatchObject({ kind: 'host', method: 'rpc.confirmed', params: { wallet: 'w', method: 'reserveasset', params: ['GOLD'], then: { method: 'registerasset', params: ['GOLD', '1000', 2, {}, true] } } });
    setTransport(async () => {
      throw { code: -32800, message: 'declined' };
    });
    expect(await walletRpcTwoSteps('w', 'reserveasset', ['GOLD'], { method: 'registerasset', params: ['GOLD', '1000', 2, {}, true] })).toBeNull();
  });

  it('runs the second only when it needs no question, never asking', async () => {
    const calls: BridgeRequest[] = [];
    setTransport(async (r) => {
      calls.push(r);
      throw { code: -32801, message: 'registerasset needs your confirmation' };
    });
    expect(await walletRpcIfAuthorized('w', 'registerasset', ['GOLD'])).toEqual({ needed: 'registerasset needs your confirmation' });
    expect(calls[0].params).toMatchObject({ authorizedOnly: true });
    setTransport(async () => ({ txid: 'b'.repeat(64) }));
    expect(await walletRpcIfAuthorized('w', 'registerasset', ['GOLD'])).toEqual({ result: { txid: 'b'.repeat(64) } });
  });
});
