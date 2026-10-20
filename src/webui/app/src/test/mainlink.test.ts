// The alert of a sidechain whose node cannot reach its mainchain node (user request 2026-10-20: the hub's
// Chains node died and a sidechain wallet froze for hours without a word). The decisions (stepLink): a link
// down for DOWN_DELAY_MS shows the banner, a syncing mainchain node for SYNC_DELAY_MS the softer one, a
// hiccup shows nothing, one notice per outage, the desktop after 5 minutes, one toast when it is back.
// The banner (MainchainAlert): the reason in plain words, Simple mode's short message, How to fix.
import { fireEvent, render, screen } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import MainchainAlert from '../components/MainchainAlert.svelte';
import NotificationBell from '../components/NotificationBell.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { checkLink, DESKTOP_DELAY_MS, DOWN_DELAY_MS, mainLink, newTracker, stepLink, stopMainLink, SYNC_DELAY_MS, type LinkTracker } from '../lib/mainlink.svelte';
import { nav } from '../lib/nav.svelte';
import { notices } from '../lib/notices.svelte';
import type { MainchainInfo } from '../lib/side';
import { app, prefs, toasts, wallets } from '../lib/state.svelte';

const T0 = 1_800_000_000_000;

function info(state: MainchainInfo['state'], extra: Partial<MainchainInfo> = {}): MainchainInfo {
  return { slot: 5, connected: state === 'ok' || state === 'syncing', node: '127.0.0.1:19554', height: 100, tipheight: 100, state, ...extra };
}

/** Looks every 4 s from `from` to `to` (ms), as the window does. */
function run(tracker: LinkTracker, sample: MainchainInfo | null, from: number, to: number) {
  const effects = [];
  for (let now = from; now <= to; now += 4000) {
    const step = stepLink(tracker, sample, now);
    tracker = step.tracker;
    effects.push(...step.effects);
  }
  return { tracker, effects };
}

describe('the link to the mainchain node (stepLink)', () => {
  it('shows the down banner only once the link is down for DOWN_DELAY_MS, with one notice', () => {
    const down = info('down', { error_kind: 'refused' });
    let r = run(newTracker(), down, T0, T0 + DOWN_DELAY_MS - 4000);
    expect(r.tracker.shown).toBe(null);
    expect(r.effects).toEqual([]);
    r = run(r.tracker, down, T0 + DOWN_DELAY_MS, T0 + 3 * DOWN_DELAY_MS);
    expect(r.tracker.shown).toBe('down');
    expect(r.effects).toEqual([{ type: 'notice', banner: 'down' }]);
  });

  it('takes the time the node gives: an app started during an outage shows it at once', () => {
    const r = stepLink(newTracker(), info('down', { error_kind: 'refused', since: T0 / 1000 - 600 }), T0);
    expect(r.tracker.shown).toBe('down');
  });

  it('ignores a hiccup (down, then ok again before the delay): no banner, no toast', () => {
    let r = run(newTracker(), info('down'), T0, T0 + 12_000);
    r = run(r.tracker, info('ok'), T0 + 16_000, T0 + 40_000);
    r = run(r.tracker, info('down'), T0 + 44_000, T0 + 56_000);
    expect(r.tracker.shown).toBe(null);
    expect(r.effects).toEqual([]);
  });

  it('shows the softer banner when the mainchain node syncs for SYNC_DELAY_MS', () => {
    const syncing = info('syncing', { error_kind: 'not-synced', behind_blocks: 40 });
    let r = run(newTracker(), syncing, T0, T0 + SYNC_DELAY_MS - 4000);
    expect(r.tracker.shown).toBe(null);
    r = run(r.tracker, syncing, T0 + SYNC_DELAY_MS, T0 + SYNC_DELAY_MS + 8000);
    expect(r.tracker.shown).toBe('syncing');
    expect(r.effects).toEqual([{ type: 'notice', banner: 'syncing' }]);
    // Caught up: the banner goes, without the toast of a link back (it was never down).
    r = run(r.tracker, info('ok'), T0 + 60_000, T0 + 60_000);
    expect(r.tracker.shown).toBe(null);
    expect(r.effects).toEqual([{ type: 'restored', wasDown: false }]);
  });

  it('when the link is back: the banner goes, one restored effect, and a new outage is a new notice', () => {
    let r = run(newTracker(), info('down'), T0, T0 + 30_000);
    expect(r.tracker.shown).toBe('down');
    r = run(r.tracker, info('ok'), T0 + 34_000, T0 + 50_000);
    expect(r.tracker.shown).toBe(null);
    expect(r.effects).toEqual([{ type: 'restored', wasDown: true }]);
    r = run(r.tracker, info('down'), T0 + 54_000, T0 + 90_000);
    expect(r.effects).toEqual([{ type: 'notice', banner: 'down' }]);
  });

  it('down, then reachable but catching up: the toast of the link back, and the softer banner at once', () => {
    let r = run(newTracker(), info('down'), T0, T0 + 30_000);
    const step = stepLink(r.tracker, info('syncing', { behind_blocks: 300 }), T0 + 34_000);
    expect(step.tracker.shown).toBe('syncing');
    expect(step.effects).toEqual([{ type: 'restored', wasDown: true }]);
    // The same outage: no second notice; caught up, no second toast.
    r = run(step.tracker, info('ok'), T0 + 38_000, T0 + 38_000);
    expect(r.effects).toEqual([{ type: 'restored', wasDown: false }]);
  });

  it('tells the desktop once, after DESKTOP_DELAY_MS down', () => {
    const r = run(newTracker(), info('down'), T0, T0 + 2 * DESKTOP_DELAY_MS);
    expect(r.effects.filter((e) => e.type === 'desktop')).toHaveLength(1);
    expect(r.effects.filter((e) => e.type === 'notice')).toHaveLength(1);
  });

  it('keeps its state when the node cannot be asked, and reads a node of before the states', () => {
    const r = run(newTracker(), info('down'), T0, T0 + 30_000);
    expect(stepLink(r.tracker, null, T0 + 34_000)).toEqual({ tracker: r.tracker, effects: [] });
    const old = { slot: 5, connected: false, node: 'x:1', height: 1, tipheight: 1 } as MainchainInfo;
    expect(run(newTracker(), old, T0, T0 + 30_000).tracker.shown).toBe('down');
    expect(stepLink(newTracker(), info('starting'), T0).tracker.shown).toBe(null);
  });
});

describe('the banner and its notices (MainchainAlert, the bell)', () => {
  let answer: MainchainInfo | null;
  let calls: BridgeRequest[];
  beforeEach(() => {
    app.info = { name: 'Sidechain', version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp/x', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 60 };
    prefs.uiMode = 'advanced';
    wallets.newTxs = [];
    toasts.list = [];
    notices.system = [];
    calls = [];
    answer = null;
    setTransport(async (r) => {
      calls.push(r);
      if (r.method === 'getmainchaininfo') return answer;
      return true;
    });
  });
  afterEach(() => stopMainLink());

  it('down: the reason, when it was last reached, How to fix with the settings; then back: gone, one toast', async () => {
    answer = info('down', { error_kind: 'refused', since: T0 / 1000 - 60, last_ok: T0 / 1000 - 60, error: 'Cannot reach the mainchain node at 127.0.0.1:19554: connection refused' });
    render(MainchainAlert);
    await checkLink(T0);
    const alert = await screen.findByRole('alert');
    expect(alert.textContent).toContain('Cannot reach your Chains node (127.0.0.1:19554): connection refused');
    expect(alert.textContent).toMatch(/Last reached .* ago/);
    expect(notices.system.map((n) => n.kind)).toEqual(['mainchain-down']);
    await fireEvent.click(screen.getByRole('button', { name: /How to fix/ }));
    expect(alert.textContent).toContain('server=1');
    expect(alert.textContent).toContain('port 29554');
    await fireEvent.click(screen.getByRole('button', { name: /Open the mainchain settings/ }));
    expect([nav.page, nav.tab]).toEqual(['settings', 'mainchain']);
    // Still down: no second notice.
    await checkLink(T0 + 4000);
    expect(notices.system).toHaveLength(1);
    answer = info('ok', { since: T0 / 1000 });
    await checkLink(T0 + 8000);
    expect(screen.queryByRole('alert')).toBeNull();
    expect(toasts.list.map((x) => x.title)).toEqual(['Connected to Chains again']);
    expect(notices.system.map((n) => n.kind)).toEqual(['mainchain-down', 'mainchain-restored']);
    await checkLink(T0 + 12_000);
    expect(toasts.list).toHaveLength(1);
  });

  it('down for 5 minutes: the desktop is told once', async () => {
    answer = info('down', { error_kind: 'timeout', since: T0 / 1000 - 400 });
    await checkLink(T0);
    await checkLink(T0 + 4000);
    expect(calls.filter((c) => c.method === 'app.notify')).toHaveLength(1);
  });

  it('syncing: the softer banner with the blocks behind; Simple mode: a short plain message', async () => {
    answer = info('syncing', { error_kind: 'not-synced', since: T0 / 1000 - 120, behind_blocks: 42 });
    const view = render(MainchainAlert);
    await checkLink(T0);
    const status = await screen.findByRole('status');
    expect(status.textContent).toContain('Chains is still syncing: 42 blocks behind');
    expect(screen.queryByRole('alert')).toBeNull();
    view.unmount();
    prefs.uiMode = 'simple';
    answer = info('down', { error_kind: 'auth', since: T0 / 1000 - 120 });
    render(MainchainAlert);
    await checkLink(T0 + 4000);
    const alert = await screen.findByRole('alert');
    expect(alert.textContent).toContain('Cannot reach Chains');
    expect(alert.textContent).toContain('This wallet is paused until Chains is back.');
    expect(alert.textContent).not.toContain('127.0.0.1');
    await fireEvent.click(screen.getByRole('button', { name: /How to fix/ }));
    expect(alert.textContent).toContain('rpcpassword');
  });

  it('a hiccup shorter than the delay shows nothing', async () => {
    render(MainchainAlert);
    answer = info('down', { error_kind: 'refused', since: T0 / 1000 });
    await checkLink(T0);
    await checkLink(T0 + 8000);
    answer = info('ok', { since: T0 / 1000 + 10 });
    await checkLink(T0 + 12_000);
    expect(screen.queryByRole('alert')).toBeNull();
    expect(notices.system).toHaveLength(0);
    expect(toasts.list).toHaveLength(0);
    expect(mainLink.banner).toBe(null);
  });

  it('the bell lists the notice of the outage, counted as new until seen', async () => {
    answer = info('down', { error_kind: 'refused', since: T0 / 1000 - 60 });
    await checkLink(T0);
    render(NotificationBell);
    const bell = screen.getByRole('button', { name: 'Notifications: 1 new' });
    await fireEvent.click(bell);
    expect(screen.getByRole('dialog', { name: 'Notifications' }).textContent).toContain('Cannot reach your Chains node');
    await fireEvent.click(screen.getByRole('button', { name: 'Mark all as read' }));
    expect(notices.system.every((n) => !n.unread)).toBe(true);
  });
});
