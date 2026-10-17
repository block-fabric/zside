import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import { dispatch, setTransport, type BridgeRequest } from '../lib/bridge';
import { go } from '../lib/nav.svelte';
import { app, node, prefs, wallets } from '../lib/state.svelte';
import { applyWindowState, frame, isDragTarget, watchWindow } from '../lib/windowframe.svelte';
import ResizeHandles from '../components/ResizeHandles.svelte';
import TitleStrip from '../components/TitleStrip.svelte';
import WindowControls from '../components/WindowControls.svelte';
import Shell from '../views/Shell.svelte';

const calls: BridgeRequest[] = [];
const methods = () => calls.map((c) => c.method);

async function fakeHost(r: BridgeRequest): Promise<unknown> {
  calls.push(r);
  if (r.method === 'window.state') return { titleBar: 'integrated', supported: true, frameless: true, restartNeeded: false, maximized: false };
  if (r.method === 'window.setTitleBar') return { titleBar: (r.params as { mode: string }).mode, supported: true, frameless: true, restartNeeded: true, maximized: false };
  if (r.method === 'getbalances') return { mine: { trusted: 1, untrusted_pending: 0, immature: 0 } };
  if (r.method?.startsWith('window.')) return true;
  return null;
}

beforeAll(() => {
  window.matchMedia ??= ((query: string) => ({ matches: false, media: query, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia;
  const proto = HTMLDialogElement.prototype as unknown as Record<string, unknown>;
  proto.showModal ??= function (this: HTMLDialogElement) {
    this.setAttribute('open', '');
  };
  proto.close ??= function (this: HTMLDialogElement) {
    this.removeAttribute('open');
  };
  Element.prototype.scrollIntoView ??= function () {};
});

beforeEach(() => {
  calls.length = 0;
  setTransport(fakeHost);
  applyWindowState({ frameless: true, maximized: false, titleBar: 'integrated', supported: true, restartNeeded: false });
});

afterEach(() => {
  setTransport(null);
});

describe('the buttons of the window', () => {
  it('minimize, maximize and close through the bridge', async () => {
    render(WindowControls);
    await fireEvent.click(screen.getByRole('button', { name: 'Minimize' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Maximize' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Close' }));
    expect(methods()).toEqual(['window.minimize', 'window.toggleMaximize', 'window.close']);
  });

  it('show the state of the window: maximize or restore', async () => {
    render(WindowControls);
    const button = screen.getByRole('button', { name: 'Maximize' });
    expect(button.dataset.state).toBe('normal');
    const off = watchWindow();
    await new Promise((r) => setTimeout(r, 0));
    calls.length = 0;
    // The desktop maximized the window (a shortcut, a double click elsewhere): the event says so.
    dispatch([{ type: 'window', data: { maximized: true } }]);
    expect(frame.maximized).toBe(true);
    await Promise.resolve();
    const restore = screen.getByRole('button', { name: 'Restore' });
    expect(restore.dataset.state).toBe('maximized');
    expect(restore.querySelector('path')?.getAttribute('d')).not.toBe('M1 1h8v8H1z');
    await fireEvent.click(restore);
    expect(methods()).toEqual(['window.toggleMaximize']);
    expect(frame.maximized).toBe(false);
    expect(screen.getByRole('button', { name: 'Maximize' })).toBeTruthy();
    off();
  });

  it('are named and in a group, for the keyboard and screen readers', () => {
    render(WindowControls);
    const group = screen.getByRole('group', { name: 'Window' });
    const buttons = within(group).getAllByRole('button');
    expect(buttons.map((b) => b.getAttribute('aria-label'))).toEqual(['Minimize', 'Maximize', 'Close']);
    for (const b of buttons) expect(b.tagName).toBe('BUTTON');
  });
});

describe('the title strip', () => {
  it('moves the window from its empty part, maximizes on a double click', async () => {
    const { container } = render(TitleStrip, { title: 'Send', detail: 'alice' });
    const bar = container.querySelector('header.titlestrip') as HTMLElement;
    expect(bar.textContent).toContain('Send');
    expect(bar.textContent).toContain('alice');
    await fireEvent.mouseDown(bar, { button: 0, detail: 1 });
    expect(methods()).toEqual(['window.startMove']);
    calls.length = 0;
    await fireEvent.mouseDown(bar, { button: 0, detail: 2 });
    await fireEvent.dblClick(bar);
    // One toggle for the double click, whether the press or the dblclick saw it.
    expect(methods()).toEqual(['window.toggleMaximize']);
    expect(frame.maximized).toBe(true);
  });

  it('maximizes on a double click the presses did not see', async () => {
    const { container } = render(TitleStrip, { title: 'Send' });
    const bar = container.querySelector('header.titlestrip') as HTMLElement;
    // Some window managers keep the second press: only the dblclick reaches the page.
    await new Promise((r) => setTimeout(r, 650));
    await fireEvent.dblClick(bar);
    expect(methods()).toEqual(['window.toggleMaximize']);
  });

  it('does not move the window from its buttons, or with another button', async () => {
    const { container } = render(TitleStrip, { title: 'Send' });
    await fireEvent.mouseDown(screen.getByRole('button', { name: 'Close' }), { button: 0, detail: 1 });
    await fireEvent.mouseDown(container.querySelector('header') as HTMLElement, { button: 2, detail: 1 });
    expect(methods()).toEqual([]);
  });

  it('is not shown with the title bar of the system', () => {
    applyWindowState({ frameless: false });
    const { container } = render(TitleStrip, { title: 'Send' });
    expect(container.querySelector('header')).toBeNull();
    expect(screen.queryByRole('button', { name: 'Close' })).toBeNull();
  });

  it('tells what can be dragged', () => {
    const bar = document.createElement('header');
    bar.innerHTML = '<span class="t">Title</span><button>b</button><a href="#">l</a><div role="menu"><span class="m">x</span></div>';
    expect(isDragTarget(bar, bar)).toBe(true);
    expect(isDragTarget(bar.querySelector('.t'), bar)).toBe(true);
    expect(isDragTarget(bar.querySelector('button'), bar)).toBe(false);
    expect(isDragTarget(bar.querySelector('a'), bar)).toBe(false);
    expect(isDragTarget(bar.querySelector('.m'), bar)).toBe(false);
    expect(isDragTarget(document.body, bar)).toBe(false);
  });
});

describe('the resize handles', () => {
  it('are at the 8 edges and corners, and resize from there', async () => {
    const { container } = render(ResizeHandles);
    const handles = [...container.querySelectorAll<HTMLElement>('.resize')];
    expect(handles.map((h) => h.dataset.edge)).toEqual(['top', 'bottom', 'left', 'right', 'top-left', 'top-right', 'bottom-left', 'bottom-right']);
    await fireEvent.mouseDown(handles[5], { button: 0 });
    expect(calls).toEqual([expect.objectContaining({ method: 'window.startResize', params: { edge: 'top-right' } })]);
  });

  it('are gone when the window is maximized, or has the frame of the system', async () => {
    applyWindowState({ maximized: true });
    const first = render(ResizeHandles);
    expect(first.container.querySelectorAll('.resize').length).toBe(0);
    first.unmount();
    applyWindowState({ maximized: false, frameless: false });
    const second = render(ResizeHandles);
    expect(second.container.querySelectorAll('.resize').length).toBe(0);
  });
});

describe('the state of the window', () => {
  it('is asked of the host and follows its events', async () => {
    applyWindowState({ frameless: false, supported: false });
    const off = watchWindow();
    await new Promise((r) => setTimeout(r, 0));
    expect(methods()).toContain('window.state');
    expect(frame.integrated).toBe(true);
    expect(frame.supported).toBe(true);
    dispatch([{ type: 'window', data: { maximized: true } }]);
    expect(frame.maximized).toBe(true);
    off();
    dispatch([{ type: 'window', data: { maximized: false } }]);
    expect(frame.maximized).toBe(true);
  });
});

describe('every layout', () => {
  beforeEach(() => {
    app.info = { name: 'Chains', version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp', walletEnabled: true, devtools: false, test: false, platform: 'linux', blockSpacing: 600 };
    prefs.loaded = true;
    node.tip = { height: 10, time: 1_700_000_000, hash: 'ab'.repeat(32), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
    wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
    wallets.current = 'w';
    go('send');
  });
  afterEach(() => {
    wallets.list = [];
    wallets.current = null;
    prefs.layout = 'dashboard';
  });

  for (const layout of ['workspace', 'dashboard'] as const) {
    it(`${layout}: has the title bar at the top, with the buttons of the window`, async () => {
      prefs.layout = layout;
      const { container, unmount } = render(Shell);
      const group = screen.getByRole('group', { name: 'Window' });
      const bar = group.closest('header') as HTMLElement;
      expect(bar).toBeTruthy();
      // The first thing of the frame, above the layout.
      if (layout === 'workspace') expect(bar.classList.contains('top')).toBe(true);
      else {
        expect(bar.classList.contains('titlestrip')).toBe(true);
        expect(bar.textContent).toContain('Send');
        expect(bar.textContent).toContain('w');
        expect(container.querySelector('.frame')?.firstElementChild).toBe(bar);
      }
      await fireEvent.mouseDown(bar, { button: 0, detail: 1 });
      expect(methods()).toContain('window.startMove');
      calls.length = 0;
      await fireEvent.mouseDown(bar, { button: 0, detail: 2 });
      // What the window was asked (the pages of a chain may read the node meanwhile).
      expect(methods().filter((m) => m?.startsWith('window.'))).toEqual(['window.toggleMaximize']);
      unmount();
      // With the title bar of the system: none of it.
      applyWindowState({ frameless: false });
      render(Shell);
      expect(screen.queryByRole('group', { name: 'Window' })).toBeNull();
      expect(document.querySelector('.titlestrip')).toBeNull();
    });
  }
});

describe('the title bar option of the settings', () => {
  it('waits for Apply, as the other settings, and goes on Cancel', async () => {
    const { default: Settings } = await import('../pages/Settings.svelte');
    const { pendingNow } = await import('../lib/settingsdraft.svelte');
    go('settings', 'display');
    render(Settings);
    const box = (await screen.findByText('Use the title bar of the desktop')).closest('label')!.querySelector('input') as HTMLInputElement;
    const apply = screen.getByRole('button', { name: 'Apply' });
    expect(box.checked).toBe(false);
    await fireEvent.click(box);
    // Ticked, waiting: the window keeps its title bar until Apply.
    expect(box.checked).toBe(true);
    expect(methods()).not.toContain('window.setTitleBar');
    expect(frame.titleBar).toBe('integrated');
    expect(pendingNow()).toBe(1);
    expect((apply as HTMLButtonElement).disabled).toBe(false);
    await fireEvent.click(apply);
    await new Promise((r) => setTimeout(r, 0));
    expect(calls.filter((c) => c.method === 'window.setTitleBar').map((c) => (c.params as { mode: string }).mode)).toEqual(['system']);
    expect(frame.titleBar).toBe('system');
    expect(pendingNow()).toBe(0);
    // Back, then Cancel: nothing is asked of the window.
    await fireEvent.click(box);
    expect(box.checked).toBe(false);
    expect(pendingNow()).toBe(1);
    await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    await new Promise((r) => setTimeout(r, 0));
    expect(pendingNow()).toBe(0);
    expect(box.checked).toBe(true);
    expect(calls.filter((c) => c.method === 'window.setTitleBar').length).toBe(1);
  });
});
