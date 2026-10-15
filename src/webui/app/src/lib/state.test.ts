import { describe, expect, it } from 'vitest';
import { dispatch, setTransport } from './bridge';
import { app, applyPrefs, node, prefs, subscribe, wallets } from './state.svelte';

setTransport(async () => true);
subscribe();

describe('state from events', () => {
  it('follows phase, tip, peers and the chain of recent blocks', () => {
    dispatch([{ type: 'phase', data: { phase: 'ready', data: { height: 1 } } }]);
    expect(app.phase).toBe('ready');
    for (let h = 1; h <= 10; h++) {
      dispatch([{ type: 'tip', data: { height: h, time: h, hash: `h${h}`, progress: 1, state: 'synced', ibd: false, loadingBlocks: false } }]);
    }
    expect(node.tip?.height).toBe(10);
    expect(node.recent.map((b) => b.height)).toEqual([4, 5, 6, 7, 8, 9, 10]);
    // A reorganisation to another tip of the same height starts the chain again.
    dispatch([{ type: 'tip', data: { height: 10, time: 11, hash: 'other10', progress: 1, state: 'synced', ibd: false, loadingBlocks: false } }]);
    expect(node.recent.map((b) => b.hash)).toEqual(['other10']);
    dispatch([{ type: 'connections', data: { count: 4 } }, { type: 'networkActive', data: { active: false } }]);
    expect(node.connections).toBe(4);
    expect(node.networkActive).toBe(false);
  });

  it('follows wallets and their balances', () => {
    dispatch([{ type: 'wallet.loaded', data: { name: 'b', encrypted: false, locked: false, privateKeysDisabled: false, hd: true } }]);
    dispatch([{ type: 'wallet.loaded', data: { name: 'a', encrypted: true, locked: true, privateKeysDisabled: false, hd: true } }]);
    expect(wallets.list.map((w) => w.name)).toEqual(['a', 'b']);
    expect(wallets.current).toBe('b');
    dispatch([{ type: 'wallet.balance', data: { name: 'b', trusted: 150000000, untrusted_pending: 0, immature: 5000000000 } }]);
    expect(wallets.balances.b.trusted).toBe(150000000n);
    dispatch([{ type: 'wallet.status', data: { name: 'a', encrypted: true, locked: false } }]);
    expect(wallets.list[0].locked).toBe(false);
    dispatch([{ type: 'wallet.unloaded', data: { name: 'b' } }]);
    expect(wallets.current).toBe('a');
    expect(wallets.balances.b).toBeUndefined();
  });

  it('keeps node dialogs in order', () => {
    dispatch([{ type: 'dialog', data: { id: 1, kind: 'message', title: 'Error', message: 'one' } }, { type: 'dialog', data: { id: 2, kind: 'question', title: 'Q', message: 'two' } }]);
    expect(app.dialogs.map((d) => d.id)).toEqual([1, 2]);
  });

  it('takes only valid preferences', () => {
    applyPrefs({ theme: 'ocean', fontAdjust: 99, unit: 'bits', mask: true });
    expect(prefs).toMatchObject({ theme: 'ocean', fontAdjust: 12, unit: 'bits', mask: true });
    applyPrefs({ theme: 'neon', unit: 'BTC', fontAdjust: 'x' });
    expect(prefs).toMatchObject({ theme: 'ocean', unit: 'bits', fontAdjust: 12 });
  });
});
