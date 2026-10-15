// The themes of the user (Settings → Advanced theme): kept with the preferences (prefs.customThemes),
// saved at once (a library, not a setting waiting for Apply), shared as theme files through the file
// chooser of the desktop (lib/files.ts: the page never names a path itself), or as text on the clipboard.
import { baseColors, checkTheme, MAX_CUSTOM_THEMES, MAX_THEME_FILE, newCustomId, parseThemeFile, themeFileText, ThemeFileError, uniqueName, type CustomTheme, type CustomThemeId, type TokenName } from './customthemes';
import { base64ToBytes, chooseFile, FILTERS, readFile, safeFileName, writeText } from './files';
import { t } from './i18n.svelte';
import { host } from './rpc';
import { draft } from './settingsdraft.svelte';
import { prefs, savePrefs } from './state.svelte';

/** The theme being edited, drawn on the page while the editor is open (null: the chosen one). */
export const look = $state({ preview: null as CustomTheme | null });

function plain(list: CustomTheme[]): CustomTheme[] {
  return list.map((x) => ({ id: x.id, name: x.name, base: x.base, colors: { ...x.colors } }));
}

function store(list: CustomTheme[]): void {
  savePrefs({ customThemes: plain(list) });
}

/** Save a theme of the user (new or changed): checked again first. Throws ThemeFileError. */
export function saveTheme(theme: CustomTheme): CustomTheme {
  const checked = checkTheme(theme);
  const saved: CustomTheme = { id: theme.id, ...checked, name: uniqueName(checked.name, prefs.customThemes, theme.id) };
  const list = [...prefs.customThemes];
  const at = list.findIndex((x) => x.id === theme.id);
  if (at >= 0) list[at] = saved;
  else {
    if (list.length >= MAX_CUSTOM_THEMES) throw new ThemeFileError(t('There are already %1 themes: delete one first.', MAX_CUSTOM_THEMES));
    list.push(saved);
  }
  store(list);
  return saved;
}

/** A new theme of the user (not saved): a copy of the colours of another, under a free name. */
export function newTheme(from: Pick<CustomTheme, 'name' | 'base' | 'colors'>): CustomTheme {
  return { id: newCustomId(prefs.customThemes), name: uniqueName(from.name, prefs.customThemes), base: from.base, colors: { ...from.colors } };
}

export function duplicateTheme(id: CustomThemeId): CustomTheme | null {
  const theme = prefs.customThemes.find((x) => x.id === id);
  if (!theme) return null;
  return saveTheme(newTheme({ ...theme, name: t('%1 (copy)', theme.name) }));
}

/** Delete a theme of the user (asked by the page first). The look in use, if it was that one, goes back to its base. */
export function deleteTheme(id: CustomThemeId): void {
  const theme = prefs.customThemes.find((x) => x.id === id);
  if (!theme) return;
  const list = prefs.customThemes.filter((x) => x.id !== id);
  const changes: { customThemes: CustomTheme[]; theme?: CustomTheme['base'] } = { customThemes: plain(list) };
  // Settings waiting for Apply that name it: its base instead.
  if (draft.prefs.theme === id) {
    draft.prefs.theme = theme.base;
    prefs.theme = theme.base;
  } else if (prefs.theme === id) changes.theme = theme.base;
  if (draft.original.theme === id) draft.original.theme = theme.base;
  savePrefs(changes);
  if (look.preview?.id === id) look.preview = null;
}

/** A theme from the text of a theme file: checked, then saved under a free name. Throws ThemeFileError. */
export function importText(text: string): CustomTheme {
  const parsed = parseThemeFile(text);
  if (prefs.customThemes.length >= MAX_CUSTOM_THEMES) throw new ThemeFileError(t('There are already %1 themes: delete one first.', MAX_CUSTOM_THEMES));
  // Files carry every colour; kept are those that differ from the base (the editor marks them changed).
  const base = baseColors(parsed.base);
  const colors = Object.fromEntries(Object.entries(parsed.colors).filter(([k, v]) => v.toLowerCase() !== base[k as TokenName]?.toLowerCase()));
  return saveTheme(newTheme({ ...parsed, colors }));
}

/** Choose a theme file in the file chooser of the desktop and import it; null when cancelled. Throws ThemeFileError. */
export async function importFile(): Promise<CustomTheme | null> {
  const path = await chooseFile({ title: t('Import Theme'), filters: [FILTERS.theme, FILTERS.all] });
  if (!path) return null;
  const file = await readFile(path);
  if (file.size > MAX_THEME_FILE) throw new ThemeFileError(t('The theme file is larger than %1 KB.', MAX_THEME_FILE / 1024));
  let text: string;
  try {
    text = new TextDecoder('utf-8', { fatal: true }).decode(base64ToBytes(file.base64));
  } catch {
    throw new ThemeFileError(t('The theme file is not text (UTF-8).'));
  }
  return importText(text);
}

/** Save a theme as a theme file, where the user chooses; the path, or null when cancelled. */
export async function exportFile(theme: Pick<CustomTheme, 'name' | 'base' | 'colors'>): Promise<string | null> {
  const path = await chooseFile({ save: true, title: t('Export Theme'), name: safeFileName(`${theme.name}.json`), filters: [FILTERS.theme] });
  if (!path) return null;
  await writeText(path, themeFileText(theme));
  return path;
}

export async function copyTheme(theme: Pick<CustomTheme, 'name' | 'base' | 'colors'>): Promise<void> {
  await host.copy(themeFileText(theme));
}
