// Simple or Advanced (user decisions, 2026-10-16): Simple shows what a beginner needs in plain words;
// Advanced, the default, is every section of every page, as before. The choice is a preference of the
// app (per network, as the others), switched in the foot of the sidebar, in the palette and on Home's
// first-run tips. A page that has no simple variant shows the same in both.
//
// In Simple mode a page may hide sections, tabs or columns, never what a beginner needs; the pages
// marked `advanced` (PAGES) leave the navigation, and stay reachable by the palette and by links.
import { t } from './i18n.svelte';
import { PAGES, type PageId } from './nav.svelte';
import { prefs, savePrefs } from './state.svelte';
import { SIDECHAIN } from '../chain/config';
import { LOCAL_PAGES, type LocalPage } from '../chain/local';

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

/** The name of a page in the mode shown, in English (for what t() translates later: commands, places). */
export function pageTitleSource(id: PageId): string {
  const page = PAGES[id];
  return simpleMode() && page?.simpleTitle ? page.simpleTitle : (page?.title ?? id);
}

/** The name of a page in the mode shown, in the language shown. */
export function pageTitle(id: PageId): string {
  const source = pageTitleSource(id);
  return t(source);
}

// The navigation of Simple mode: the same short list in every app (user feedback 2026-10-19; the rule is
// in doc/webui.md, "Simple and Advanced modes", and test/simple-nav.test.ts holds every app to it):
//   Home · Send · Receive · Activity · Move coins · at most three main features of the chain · Settings
// Each in plain words with a line saying what it is for (simpleTitle, simpleSummary). Everything else is
// in Advanced, and still reached in Simple mode by the palette and by links.

/** The most pages of a chain's own a Simple navigation holds (chain/local.ts `simple: true`). */
export const SIMPLE_MAX_FEATURES = 3;

/** What every app's Simple navigation starts with (Activity is the Transactions page). */
export const SIMPLE_START: readonly PageId[] = ['home', 'send', 'receive', 'transactions'];

/** Moving coins between the chains: the peg of a sidechain, the Sidechains page (Move coins) of Chains. */
export const SIMPLE_MOVE: PageId = SIDECHAIN ? 'peg' : 'sidechains';

/** What every app's Simple navigation ends with. */
export const SIMPLE_END: readonly PageId[] = ['settings'];

/** The chain's main features in Simple mode: its pages marked `simple`, in their order (at most three). */
export function simpleFeatures(): PageId[] {
  return (LOCAL_PAGES as readonly LocalPage[])
    .filter((p) => p.simple)
    .slice(0, SIMPLE_MAX_FEATURES)
    .map((p) => p.id as PageId);
}

/** The pages of Simple mode's navigation, in their order. */
export function simpleNavPages(): PageId[] {
  return [...SIMPLE_START, SIMPLE_MOVE, ...simpleFeatures(), ...SIMPLE_END];
}

/** Whether a page is in the navigation (sidebar, workspace tabs, quick actions) in the mode shown. */
export function pageShown(id: PageId): boolean {
  if (!simpleMode()) return true;
  return simpleNavPages().includes(id);
}

/** What a page is for, in the mode shown, in the language shown. */
export function pageSummary(id: PageId): string {
  const source = pageSummarySource(id);
  return t(source);
}

/** What a page is for, in the mode shown, in English (t() translates it where it is shown). */
export function pageSummarySource(id: PageId): string {
  const page = PAGES[id];
  return simpleMode() && page?.simpleSummary ? page.simpleSummary : (page?.summary ?? '');
}
