// Simple or Advanced (user decision, 2026-10-16): Simple, the default, shows what a beginner needs in
// plain words; Advanced is every section of every page, as before. The choice is a preference of the
// app (per network, as the others), switched in the foot of the sidebar, in the palette and on Home's
// first-run tips. A page that has no simple variant shows the same in both.
//
// In Simple mode a page may hide sections, tabs or columns, never what a beginner needs; the pages
// marked `advanced` (PAGES) leave the navigation, and stay reachable by the palette and by links.
import { PAGES, type PageId } from './nav.svelte';
import { prefs, savePrefs } from './state.svelte';

export type UiMode = 'simple' | 'advanced';

export const UI_MODES: readonly { id: UiMode; label: string; summary: string }[] = [
  { id: 'simple', label: 'Simple', summary: 'The pages a beginner needs, in plain words' },
  { id: 'advanced', label: 'Advanced', summary: 'Every section, table and option of every page' },
];

export function isUiMode(value: unknown): value is UiMode {
  return value === 'simple' || value === 'advanced';
}

/** Whether the app is in Simple mode now (call it in a template or a $derived: it follows the preference). */
export function simpleMode(): boolean {
  return prefs.uiMode === 'simple';
}

export function setUiMode(mode: UiMode): void {
  if (prefs.uiMode !== mode) savePrefs({ uiMode: mode });
}

export function toggleUiMode(): void {
  setUiMode(prefs.uiMode === 'simple' ? 'advanced' : 'simple');
}

/** The label of the command that switches to the other mode (English: where it is shown, t() translates it). */
export function switchModeLabel(): string {
  return prefs.uiMode === 'simple' ? 'Switch to Advanced mode' : 'Switch to Simple mode';
}

/** Whether a page is in the navigation (sidebar, workspace tabs, quick actions) in the mode shown. */
export function pageShown(id: PageId): boolean {
  return !(simpleMode() && PAGES[id]?.advanced);
}
