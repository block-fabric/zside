// The looks of the app: Midnight (the default, the look of chains-app), the themes of chains-qt
// (theme.cpp), as sets of CSS variables (styles/themes.css), plus the look of the desktop.

export type ThemeId = 'system' | 'midnight' | 'light' | 'dark' | 'ocean' | 'matrix' | 'qdarkstyle';

export interface Theme {
  id: ThemeId;
  name: string;
  /** Whether the theme is dark (for the colour scheme of form controls). */
  dark: boolean | 'auto';
}

export const THEMES: readonly Theme[] = [
  { id: 'system', name: 'Same as the desktop', dark: 'auto' },
  { id: 'midnight', name: 'Midnight', dark: true },
  { id: 'light', name: 'Light', dark: false },
  { id: 'dark', name: 'Dark', dark: true },
  { id: 'ocean', name: 'Ocean Teal', dark: true },
  { id: 'matrix', name: 'Matrix Green', dark: true },
  { id: 'qdarkstyle', name: 'Dark (classic Drivechain)', dark: true },
];

/** The look of a first start. */
export const DEFAULT_THEME: ThemeId = 'midnight';

export const MIN_FONT_ADJUST = -6;
export const MAX_FONT_ADJUST = 12;
export const BASE_FONT_PX = 14;

export function isTheme(value: unknown): value is ThemeId {
  return typeof value === 'string' && THEMES.some((t) => t.id === value);
}

/** The theme drawn for a choice: "system" follows the desktop. */
export function resolveTheme(id: ThemeId, prefersDark: boolean): Exclude<ThemeId, 'system'> {
  if (id !== 'system') return id;
  return prefersDark ? 'dark' : 'light';
}

/** Whether a choice is drawn dark. */
export function isDark(id: ThemeId, prefersDark: boolean): boolean {
  return THEMES.find((t) => t.id === resolveTheme(id, prefersDark))?.dark === true;
}

/** The quick switch of the top bar: a dark look goes light; the light one goes back to the dark look last used (Midnight at first). */
export function toggledTheme(id: ThemeId, prefersDark: boolean, lastDark: ThemeId | null): ThemeId {
  if (isDark(id, prefersDark)) return 'light';
  return lastDark && lastDark !== 'light' && lastDark !== 'system' ? lastDark : DEFAULT_THEME;
}

export function clampFontAdjust(n: number): number {
  if (!Number.isFinite(n)) return 0;
  return Math.max(MIN_FONT_ADJUST, Math.min(MAX_FONT_ADJUST, Math.round(n)));
}

export type FontChoice = '' | 'inter' | 'nunito' | 'mono' | 'system';

/** The fonts that can go with any colours (the custom look of chains-qt: any colours with any font). */
export const FONTS: readonly { id: FontChoice; name: string }[] = [
  { id: '', name: 'The font of the theme' },
  { id: 'inter', name: 'Inter' },
  { id: 'nunito', name: 'Nunito' },
  { id: 'mono', name: 'JetBrains Mono' },
  { id: 'system', name: 'The font of the desktop' },
];

/** Put a look on the document: colours, a font apart from them, the font of amounts, the size. */
export function applyTheme(doc: Document, id: ThemeId, fontAdjust: number, prefersDark: boolean, font: FontChoice = '', moneyFont: 'embedded' | 'system' = 'embedded'): void {
  const resolved = resolveTheme(id, prefersDark);
  const root = doc.documentElement;
  root.dataset.theme = resolved;
  root.dataset.themeChoice = id;
  const theme = THEMES.find((t) => t.id === resolved);
  root.style.colorScheme = theme?.dark ? 'dark' : 'light';
  root.style.fontSize = `${BASE_FONT_PX + clampFontAdjust(fontAdjust)}px`;
  if (font) root.dataset.font = font;
  else delete root.dataset.font;
  root.dataset.moneyFont = moneyFont;
}
