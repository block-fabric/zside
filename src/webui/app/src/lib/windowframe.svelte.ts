// The integrated title bar (doc/webui-layouts.md): the window has no frame of the system, and the
// page draws the title bar — its minimize, maximize/restore and close buttons, the empty part that
// moves the window (a double click maximizes it), the handles at the edges that resize it. With the
// title bar of the system (Settings → Appearance), the page draws none of it.
import { on } from './bridge';
import { host, type WindowState } from './rpc';

export type WindowEdge = 'top' | 'bottom' | 'left' | 'right' | 'top-left' | 'top-right' | 'bottom-left' | 'bottom-right';
export const WINDOW_EDGES: WindowEdge[] = ['top', 'bottom', 'left', 'right', 'top-left', 'top-right', 'bottom-left', 'bottom-right'];

export const frame = $state({
  /** The window is without the frame of the system now: the page draws the title bar. */
  integrated: false,
  maximized: false,
  /** The title bar chosen in the settings. */
  titleBar: 'integrated' as 'integrated' | 'system',
  /** This platform can have the integrated title bar. */
  supported: false,
  /** The title bar chosen comes with the next start. */
  restartNeeded: false,
});

export function applyWindowState(s: Partial<WindowState>): void {
  if (typeof s.frameless === 'boolean') frame.integrated = s.frameless;
  if (typeof s.maximized === 'boolean') frame.maximized = s.maximized;
  if (s.titleBar === 'integrated' || s.titleBar === 'system') frame.titleBar = s.titleBar;
  if (typeof s.supported === 'boolean') frame.supported = s.supported;
  if (typeof s.restartNeeded === 'boolean') frame.restartNeeded = s.restartNeeded;
}

/** Ask the host for the state of the window, and follow its changes. Returns the function that stops. */
export function watchWindow(): () => void {
  const off = on<{ maximized: boolean }>('window', (d) => applyWindowState(d));
  host.windowState().then(applyWindowState, () => undefined);
  return off;
}

const quiet = <T>(p: Promise<T>) => p.catch(() => undefined);

export function minimizeWindow(): void {
  void quiet(host.minimize());
}

export function toggleMaximize(): void {
  // Shown at once; the event of the window confirms it.
  frame.maximized = !frame.maximized;
  void quiet(host.toggleMaximize());
}

/** The close button: as the one of the system (minimize or tray if the user wants, else the app stops). */
export function closeWindow(): void {
  void quiet(host.closeWindow());
}

export function startResize(edge: WindowEdge): void {
  void quiet(host.startResize(edge));
}

export async function setTitleBar(mode: 'integrated' | 'system'): Promise<void> {
  applyWindowState(await host.setTitleBar(mode));
}

const INTERACTIVE = 'button, a, input, select, textarea, label, summary, [role="button"], [role="menu"], [role="menuitem"], [role="tab"], [role="combobox"], [contenteditable], [tabindex]:not([tabindex="-1"]), [data-no-drag]';

/** Whether a press on this element of the title bar moves the window: not on what can be used. */
export function isDragTarget(target: EventTarget | null, bar: HTMLElement): boolean {
  if (!(target instanceof Element) || !bar.contains(target)) return false;
  const hit = target.closest(INTERACTIVE);
  return !hit || !bar.contains(hit) || hit === bar;
}

/**
 * A press of the main button on the empty part of the title bar: a second press in a row maximizes
 * or restores the window, a first one moves it (the window manager follows the pointer).
 */
export function titleBarMouseDown(e: MouseEvent, bar: HTMLElement): void {
  if (!frame.integrated || e.button !== 0 || !isDragTarget(e.target, bar)) return;
  e.preventDefault();
  if (e.detail >= 2) {
    lastToggle = Date.now();
    toggleMaximize();
    return;
  }
  void quiet(host.startMove());
}

let lastToggle = 0;

/** A double click the press did not see (some window managers eat the second press). */
export function titleBarDoubleClick(e: MouseEvent, bar: HTMLElement): void {
  if (!frame.integrated || !isDragTarget(e.target, bar)) return;
  if (Date.now() - lastToggle < 600) return;
  toggleMaximize();
}
