// The keyboard shortcuts of the main window (as chains-qt).
import { openPalette } from './commands.svelte';
import { go, nav, type PageId } from './nav.svelte';
import { host } from './rpc';
import { prefs, savePrefs, wallets } from './state.svelte';
import { clampFontAdjust } from './themes';
import { SIDECHAIN } from '../chain/config';

/** Ctrl+letter: the tabs of the node window (the Window menu of chains-qt). */
const NODE_TABS: Record<string, string> = { i: 'info', t: 'console', n: 'traffic', p: 'peers' };
// Alt+5: the sidechains of the mainchain, the peg of a sidechain.
const PAGE_KEYS: Record<string, PageId> = { '1': 'overview', '2': 'send', '3': 'receive', '4': 'transactions', '5': SIDECHAIN ? 'peg' : 'sidechains' };
const WALLET_PAGES = new Set<PageId>(['send', 'receive', 'transactions']);

/** Close what is on top: the newest open dialog (as Escape does), else the node window. */
export function closeTop(): boolean {
  const open = [...document.querySelectorAll<HTMLDialogElement>('dialog[open]')];
  const top = open[open.length - 1];
  if (top) {
    top.dispatchEvent(new Event('cancel', { cancelable: true }));
    return true;
  }
  if (nav.page === 'node') {
    go('overview');
    return true;
  }
  return false;
}

/** Handle a key press; true if it was a shortcut. */
export function handleShortcut(e: KeyboardEvent): boolean {
  const ctrl = e.ctrlKey || e.metaKey;
  if (e.altKey && !ctrl && PAGE_KEYS[e.key]) {
    const page = PAGE_KEYS[e.key];
    if (WALLET_PAGES.has(page) && wallets.list.length === 0) return true;
    go(page);
    return true;
  }
  if (!ctrl) return false;
  const key = e.key.toLowerCase();
  if (e.shiftKey && key === 'm') {
    savePrefs({ mask: !prefs.mask });
    return true;
  }
  if (e.shiftKey && key === 'c') {
    go('node', 'console');
    return true;
  }
  if (e.shiftKey && key === 'd') {
    go('node', nav.page === 'node' ? nav.tab : 'info');
    return true;
  }
  if (e.shiftKey) return false;
  if (key === 'k') {
    openPalette();
    return true;
  }
  // Ctrl+W closes the dialog on top (GUIUtil::handleCloseWindowShortcut), or the node window.
  if (key === 'w') return closeTop();
  if (NODE_TABS[key]) {
    go('node', NODE_TABS[key]);
    return true;
  }
  if (key === 'q') {
    host.quit();
    return true;
  }
  if (key === 'm') {
    host.minimize();
    return true;
  }
  if (key === '+' || key === '=') {
    savePrefs({ fontAdjust: clampFontAdjust(prefs.fontAdjust + 1) });
    return true;
  }
  if (key === '-' || key === '_') {
    savePrefs({ fontAdjust: clampFontAdjust(prefs.fontAdjust - 1) });
    return true;
  }
  return false;
}
