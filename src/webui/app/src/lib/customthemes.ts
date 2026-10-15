// Themes of the user (Settings → Advanced theme): a built-in look as a base, with some of its colours
// changed. They are kept with the preferences, and shared as theme files (JSON).
//
// A theme file comes from anywhere, so it is untrusted: it is read strictly. It is a JSON object with a
// format and a version, a name (plain text, shown with SafeText, never translated nor read as HTML), the
// built-in look it starts from, and colours by the name of a known colour token, each a colour and
// nothing else (#rgb, #rrggbb, #rrggbbaa, rgb()/rgba()/hsl()/hsla() of numbers). Anything else (another
// field, an unknown token, url(), var(), an expression, a semicolon or a brace) refuses the whole file.
// The colours are put on the page one by one with style.setProperty on the known names: no CSS text of
// a file ever reaches a style sheet.
import THEMES_CSS from '../styles/themes.css?raw';
import { themeTokens } from './contrast';
import { t } from './i18n.svelte';
import { applyTheme, DEFAULT_THEME, isDark, isTheme, THEMES, toggledTheme, type FontChoice, type ThemeId } from './themes';

export const THEME_FORMAT = 'chains-app-theme';
export const THEME_VERSION = 1;
/** The largest theme file read (bytes). */
export const MAX_THEME_FILE = 64 * 1024;
/** The most themes of the user kept. */
export const MAX_CUSTOM_THEMES = 50;
/** The longest name of a theme (characters). */
export const MAX_THEME_NAME = 48;
/** The longest colour value read. */
const MAX_COLOUR = 64;

/** The built-in looks a theme can start from. */
export type BaseTheme = Exclude<ThemeId, 'system'>;
export type CustomThemeId = `custom-${string}`;

export interface CustomTheme {
  id: CustomThemeId;
  name: string;
  base: BaseTheme;
  /** The colours changed, by token (without the "--"). */
  colors: Partial<Record<TokenName, string>>;
}

/** The colour tokens of themes.css a theme can set, grouped as the editor shows them. */
export const TOKEN_GROUPS = [
  {
    label: 'Backgrounds',
    tokens: [
      { name: 'bg', label: 'Page' },
      { name: 'surface', label: 'Panels and cards' },
      { name: 'surface-2', label: 'Panels, second level' },
      { name: 'raised', label: 'Controls' },
      { name: 'line', label: 'Lines' },
      { name: 'line-strong', label: 'Strong lines' },
      { name: 'chain-link', label: 'Links between blocks' },
    ],
  },
  {
    label: 'Text',
    tokens: [
      { name: 'text', label: 'Text' },
      { name: 'text-muted', label: 'Muted text' },
      { name: 'text-faint', label: 'Captions and hints' },
      { name: 'link', label: 'Links' },
    ],
  },
  {
    label: 'Accent',
    tokens: [
      { name: 'accent', label: 'Accent (buttons, the current page)' },
      { name: 'accent-strong', label: 'Strong accent' },
      { name: 'accent-soft', label: 'Soft accent (badges)' },
      { name: 'accent-fg', label: 'Accent drawn on the page' },
      { name: 'on-accent', label: 'Text on the accent' },
      { name: 'focus', label: 'Focus ring' },
    ],
  },
  {
    label: 'States',
    tokens: [
      { name: 'positive', label: 'Positive (received)' },
      { name: 'negative', label: 'Negative (sent, errors)' },
      { name: 'warning', label: 'Warnings' },
      { name: 'warning-soft', label: 'Background of warnings' },
      { name: 'danger-soft', label: 'Background of errors' },
    ],
  },
  {
    label: 'Light and shade',
    tokens: [
      { name: 'ambient', label: 'Ambient glow' },
      { name: 'ambient-2', label: 'Second ambient glow' },
      { name: 'card-top', label: 'Light on cards' },
      { name: 'chart-fill', label: 'Chart fill' },
      { name: 'overlay', label: 'Behind dialogs' },
    ],
  },
] as const;

export type TokenName = (typeof TOKEN_GROUPS)[number]['tokens'][number]['name'];
export const TOKENS: readonly TokenName[] = TOKEN_GROUPS.flatMap((g) => g.tokens.map((x) => x.name));
const TOKEN_SET: ReadonlySet<string> = new Set(TOKENS);

export function isToken(name: unknown): name is TokenName {
  return typeof name === 'string' && TOKEN_SET.has(name);
}

export const BASE_THEMES: readonly BaseTheme[] = THEMES.filter((x) => x.id !== 'system').map((x) => x.id as BaseTheme);

export function isBaseTheme(value: unknown): value is BaseTheme {
  return typeof value === 'string' && (BASE_THEMES as readonly string[]).includes(value);
}

export function isCustomId(value: unknown): value is CustomThemeId {
  return typeof value === 'string' && /^custom-[a-z0-9]{1,24}$/.test(value);
}

export function newCustomId(taken: readonly { id: string }[] = []): CustomThemeId {
  for (;;) {
    const id = `custom-${Date.now().toString(36)}${Math.floor(Math.random() * 36 ** 4).toString(36)}` as CustomThemeId;
    if (isCustomId(id) && !taken.some((x) => x.id === id)) return id;
  }
}

// --- Colours ---

export interface Rgba {
  r: number;
  g: number;
  b: number;
  /** 0…1 */
  a: number;
}

const NUMBER = /^(?:\d{1,3}(?:\.\d{1,6})?|\.\d{1,6})%?$/;

/** A number of a colour function: its value, and whether it was a percentage; null if not a plain number. */
function num(text: string): { v: number; pct: boolean } | null {
  if (!NUMBER.test(text)) return null;
  const pct = text.endsWith('%');
  return { v: Number(pct ? text.slice(0, -1) : text), pct };
}

function alphaOf(text: string | undefined): number | null {
  if (text === undefined) return 1;
  const n = num(text);
  if (!n) return null;
  const a = n.pct ? n.v / 100 : n.v;
  return a >= 0 && a <= 1 ? a : null;
}

function hslToRgb(h: number, s: number, l: number): [number, number, number] {
  const k = (n: number) => (n + h / 30) % 12;
  const f = (n: number) => l - s * Math.min(l, 1 - l) * Math.max(-1, Math.min(k(n) - 3, 9 - k(n), 1));
  return [f(0), f(8), f(4)].map((x) => Math.round(x * 255)) as [number, number, number];
}

/**
 * A colour as a theme may give it, or null: #rgb, #rrggbb, #rrggbbaa, or rgb(), rgba(), hsl(), hsla() of
 * plain numbers (commas, or spaces with "/ alpha"). Nothing else is a colour here.
 */
export function parseColor(value: unknown): Rgba | null {
  if (typeof value !== 'string' || value.length === 0 || value.length > MAX_COLOUR) return null;
  const v = value.trim().toLowerCase();
  // Only these characters can be in any colour of the grammar.
  if (!/^[#a-z0-9.,%()/ ]+$/.test(v)) return null;
  const hex = /^#([0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$/.exec(v);
  if (hex) {
    let h = hex[1];
    if (h.length === 3) h = [...h].map((c) => c + c).join('');
    const byte = (i: number) => parseInt(h.slice(i, i + 2), 16);
    return { r: byte(0), g: byte(2), b: byte(4), a: h.length === 8 ? Math.round((byte(6) / 255) * 1000) / 1000 : 1 };
  }
  const fn = /^(rgba?|hsla?)\(([0-9.,%/ ]*)\)$/.exec(v);
  if (!fn) return null;
  const inner = fn[2].trim();
  let parts: string[];
  let alpha: string | undefined;
  if (inner.includes(',')) {
    if (inner.includes('/')) return null;
    parts = inner.split(',').map((s) => s.trim());
    if (parts.length === 4) alpha = parts.pop();
  } else {
    const slash = inner.split('/');
    if (slash.length > 2) return null;
    parts = slash[0].trim().split(/\s+/);
    if (slash.length === 2) alpha = slash[1].trim();
  }
  if (parts.length !== 3) return null;
  const a = alphaOf(alpha);
  if (a === null) return null;
  const n = parts.map(num);
  if (n.some((x) => x === null)) return null;
  const [x, y, z] = n as { v: number; pct: boolean }[];
  if (fn[1].startsWith('rgb')) {
    const ch = (c: { v: number; pct: boolean }) => (c.pct ? (c.v <= 100 ? Math.round((c.v / 100) * 255) : -1) : c.v <= 255 ? Math.round(c.v) : -1);
    const rgb = [ch(x), ch(y), ch(z)];
    if (rgb.some((c) => c < 0)) return null;
    return { r: rgb[0], g: rgb[1], b: rgb[2], a };
  }
  // hsl: a hue in degrees (a plain number), saturation and lightness in percent.
  if (x.pct || x.v > 360 || !y.pct || !z.pct || y.v > 100 || z.v > 100) return null;
  const [r, g, b] = hslToRgb(x.v, y.v / 100, z.v / 100);
  return { r, g, b, a };
}

export function isColor(value: unknown): value is string {
  return parseColor(value) !== null;
}

const hex2 = (n: number) => Math.max(0, Math.min(255, Math.round(n))).toString(16).padStart(2, '0');

/** #rrggbb, with the alpha (#rrggbbaa) when it is not opaque. */
export function toHex(c: Rgba): string {
  return `#${hex2(c.r)}${hex2(c.g)}${hex2(c.b)}${c.a < 1 ? hex2(c.a * 255) : ''}`;
}

/** A colour over another (opaque) one. */
function over(fg: Rgba, bg: Rgba): Rgba {
  return { r: fg.r * fg.a + bg.r * (1 - fg.a), g: fg.g * fg.a + bg.g * (1 - fg.a), b: fg.b * fg.a + bg.b * (1 - fg.a), a: 1 };
}

function luminanceOf(c: Rgba): number {
  const [r, g, b] = [c.r, c.g, c.b].map((v) => {
    const s = v / 255;
    return s <= 0.04045 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/** The WCAG contrast of a text colour on a background (a transparent colour drawn over the one below, black under all). */
export function contrastOf(fg: string, bg: string): number | null {
  const f = parseColor(fg);
  const b0 = parseColor(bg);
  if (!f || !b0) return null;
  const b = over(b0, { r: 0, g: 0, b: 0, a: 1 });
  const [x, y] = [luminanceOf(over(f, b)), luminanceOf(b)].sort((p, q) => q - p);
  return (x + 0.05) / (y + 0.05);
}

/** Whether a background is dark (form controls are then drawn dark). */
export function isDarkColor(value: string): boolean {
  const c = parseColor(value);
  return c ? luminanceOf(over(c, { r: 0, g: 0, b: 0, a: 1 })) < 0.2 : true;
}

/** The pairs the editor checks: text on its backgrounds, under 4.5:1 a warning (never a refusal). */
export const CONTRAST_PAIRS: readonly [fg: TokenName, bg: TokenName][] = [
  ['text', 'bg'],
  ['text', 'surface'],
  ['text-muted', 'surface'],
  ['text-faint', 'bg'],
  ['link', 'surface'],
  ['on-accent', 'accent'],
];

export function contrastWarnings(colors: Partial<Record<TokenName, string>>): { fg: TokenName; bg: TokenName; ratio: number }[] {
  const out: { fg: TokenName; bg: TokenName; ratio: number }[] = [];
  for (const [fg, bg] of CONTRAST_PAIRS) {
    const ratio = contrastOf(colors[fg] ?? '', colors[bg] ?? '');
    if (ratio !== null && ratio < 4.5) out.push({ fg, bg, ratio });
  }
  return out;
}

/** The chevron of the dropdowns in a colour (themes.css draws it in --text-muted): made here from a parsed colour, never from text of a file. */
export function chevronFor(color: string): string | null {
  const c = parseColor(color);
  if (!c) return null;
  const stroke = toHex({ ...c, a: 1 }).slice(1);
  return `url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%23${stroke}' stroke-width='2.25' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M6 9l6 6 6-6'/%3E%3C/svg%3E")`;
}

// --- Names ---

// Control characters, and the marks that reorder text (a name could otherwise show reversed or hide a part).
// eslint-disable-next-line no-control-regex
const BAD_NAME = /[\u0000-\u001f\u007f-\u009f\u200e\u200f\u2028\u2029\u202a-\u202e\u2066-\u2069\ufeff]/;

/** A name cleaned of spaces at its ends; an error message when it is not acceptable. */
export function checkName(name: unknown): { name: string } | { error: string } {
  if (typeof name !== 'string') return { error: t('The theme has no name.') };
  if (BAD_NAME.test(name)) return { error: t('The name of the theme has characters that cannot be shown.') };
  const clean = name.trim().replace(/ +/g, ' ');
  if (!clean) return { error: t('The theme has no name.') };
  if ([...clean].length > MAX_THEME_NAME) return { error: t('The name of the theme is longer than %1 characters.', MAX_THEME_NAME) };
  return { name: clean };
}

// --- Theme files ---

export class ThemeFileError extends Error {}

const FILE_FIELDS = new Set(['format', 'version', 'name', 'base', 'colors']);

/** The colours of an object, checked: every key a known token, every value a colour. */
function checkColors(raw: unknown): Partial<Record<TokenName, string>> {
  if (raw === undefined) return {};
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) throw new ThemeFileError(t('The colours of the theme are not a list of names and colours.'));
  const out: Partial<Record<TokenName, string>> = {};
  const entries = Object.entries(raw as Record<string, unknown>);
  if (entries.length > TOKENS.length) throw new ThemeFileError(t('The theme has more colours than there are.'));
  for (const [key, value] of entries) {
    if (!isToken(key)) throw new ThemeFileError(t('Unknown colour in the theme: %1', key.slice(0, 40)));
    if (!isColor(value)) throw new ThemeFileError(t('The colour %1 of the theme is not a colour (#rrggbb, rgb() or hsl() of numbers).', key));
    out[key] = (value as string).trim();
  }
  return out;
}

/** A theme as stored or read: checked as strictly as a file. Throws ThemeFileError. */
export function checkTheme(raw: unknown): { name: string; base: BaseTheme; colors: Partial<Record<TokenName, string>> } {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) throw new ThemeFileError(t('This is not a theme file.'));
  const o = raw as Record<string, unknown>;
  const name = checkName(o.name);
  if ('error' in name) throw new ThemeFileError(name.error);
  if (o.base !== undefined && !isBaseTheme(o.base)) throw new ThemeFileError(t('The theme starts from a look this app does not have.'));
  return { name: name.name, base: (o.base as BaseTheme | undefined) ?? 'midnight', colors: checkColors(o.colors) };
}

/** Read a theme file (its text). Throws ThemeFileError with what is wrong. */
export function parseThemeFile(text: string): { name: string; base: BaseTheme; colors: Partial<Record<TokenName, string>> } {
  if (typeof text !== 'string' || text.length === 0) throw new ThemeFileError(t('The theme file is empty.'));
  if (new TextEncoder().encode(text).length > MAX_THEME_FILE) throw new ThemeFileError(t('The theme file is larger than %1 KB.', MAX_THEME_FILE / 1024));
  let raw: unknown;
  try {
    raw = JSON.parse(text.replace(/^\ufeff/, ''));
  } catch {
    throw new ThemeFileError(t('The theme file is not JSON.'));
  }
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) throw new ThemeFileError(t('This is not a theme file.'));
  const o = raw as Record<string, unknown>;
  if (o.format !== THEME_FORMAT) throw new ThemeFileError(t('This is not a theme file.'));
  if (o.version !== THEME_VERSION) throw new ThemeFileError(t('This theme file is of a version this app cannot read.'));
  for (const key of Object.keys(o)) {
    if (!FILE_FIELDS.has(key)) throw new ThemeFileError(t('Unknown field in the theme file: %1', key.slice(0, 40)));
  }
  return checkTheme(o);
}

/** The theme file of a theme (JSON text), the colours in the order of the editor. */
export function themeFileText(theme: Pick<CustomTheme, 'name' | 'base' | 'colors'>): string {
  const colors: Record<string, string> = {};
  for (const token of TOKENS) {
    const v = theme.colors[token];
    if (v !== undefined && isColor(v)) colors[token] = v;
  }
  return `${JSON.stringify({ format: THEME_FORMAT, version: THEME_VERSION, name: theme.name, base: theme.base, colors }, null, 2)}\n`;
}

/** The themes of the user as kept in the preferences: each checked again, the bad ones dropped, at most MAX_CUSTOM_THEMES. */
export function checkStoredThemes(raw: unknown): CustomTheme[] {
  if (!Array.isArray(raw)) return [];
  const out: CustomTheme[] = [];
  for (const item of raw) {
    if (out.length >= MAX_CUSTOM_THEMES) break;
    if (!item || typeof item !== 'object' || !isCustomId((item as { id?: unknown }).id)) continue;
    const id = (item as { id: CustomThemeId }).id;
    if (out.some((x) => x.id === id)) continue;
    try {
      out.push({ id, ...checkTheme(item) });
    } catch {
      // A theme that no longer reads is left out.
    }
  }
  return out;
}

/** A name not yet used by the themes of the user: the name, or the name with (2), (3)... */
export function uniqueName(name: string, themes: readonly { name: string; id?: string }[], except?: string): string {
  const used = (n: string) => themes.some((x) => x.id !== except && x.name.toLowerCase() === n.toLowerCase());
  if (!used(name)) return name;
  for (let i = 2; ; i++) {
    const suffix = ` (${i})`;
    const candidate = [...name].slice(0, MAX_THEME_NAME - suffix.length).join('') + suffix;
    if (!used(candidate)) return candidate;
  }
}

// --- On the page ---


/** A theme as chosen: a built-in look, or one of the user's. */
export type ThemeChoice = ThemeId | CustomThemeId;

let baseCache: Record<string, Record<string, string>> | null = null;

/** The colours of a built-in look, as themes.css has them. */
export function baseColors(base: BaseTheme): Record<TokenName, string> {
  baseCache ??= themeTokens(THEMES_CSS);
  const all = baseCache[base] ?? baseCache.midnight ?? {};
  const out = {} as Record<TokenName, string>;
  for (const token of TOKENS) out[token] = all[token] ?? '';
  return out;
}

/** Every colour of a theme of the user: its own over those of its base. */
export function effectiveColors(theme: Pick<CustomTheme, 'base' | 'colors'>): Record<TokenName, string> {
  const out = baseColors(theme.base);
  for (const token of TOKENS) {
    const v = theme.colors[token];
    if (v !== undefined && isColor(v)) out[token] = v;
  }
  return out;
}

export function isThemeChoice(value: unknown, customs: readonly CustomTheme[]): value is ThemeChoice {
  return isTheme(value) || (isCustomId(value) && customs.some((x) => x.id === value));
}

export function findCustom(choice: string, customs: readonly CustomTheme[]): CustomTheme | null {
  return isCustomId(choice) ? (customs.find((x) => x.id === choice) ?? null) : null;
}

/** Whether a choice is drawn dark. */
export function choiceIsDark(choice: string, prefersDark: boolean, customs: readonly CustomTheme[]): boolean {
  const custom = findCustom(choice, customs);
  if (custom) return isDarkColor(effectiveColors(custom).bg);
  return isTheme(choice) ? isDark(choice, prefersDark) : true;
}

/** The quick switch of the top bars with the themes of the user: a dark one goes light, and back. */
export function toggledChoice(choice: string, prefersDark: boolean, lastDark: ThemeChoice | null, customs: readonly CustomTheme[]): ThemeChoice {
  if (choiceIsDark(choice, prefersDark, customs)) return 'light';
  if (lastDark && findCustom(lastDark, customs) && choiceIsDark(lastDark, prefersDark, customs)) return lastDark;
  return toggledTheme('light', prefersDark, lastDark && isTheme(lastDark) ? lastDark : null);
}

/**
 * Put a look on the document. A theme of the user (or the one being edited, `preview`) is drawn as its
 * base look, then each of its colours is set on its known token with style.setProperty: no text of the
 * theme is ever read as CSS.
 */
export function applyLook(doc: Document, choice: string, customs: readonly CustomTheme[], preview: CustomTheme | null, fontAdjust: number, prefersDark: boolean, font: FontChoice = '', moneyFont: 'embedded' | 'system' = 'embedded'): void {
  const custom = preview ?? findCustom(choice, customs);
  const builtin = custom ? custom.base : isTheme(choice) ? choice : DEFAULT_THEME;
  applyTheme(doc, builtin, fontAdjust, prefersDark, font, moneyFont);
  const root = doc.documentElement;
  for (const token of TOKENS) root.style.removeProperty(`--${token}`);
  root.style.removeProperty('--chevron');
  if (!custom) return;
  root.dataset.themeChoice = 'custom';
  for (const token of TOKENS) {
    const v = custom.colors[token];
    if (v !== undefined && isColor(v)) root.style.setProperty(`--${token}`, v.trim());
  }
  const muted = custom.colors['text-muted'];
  const chevron = muted ? chevronFor(muted) : null;
  if (chevron) root.style.setProperty('--chevron', chevron);
  root.style.colorScheme = isDarkColor(effectiveColors(custom).bg) ? 'dark' : 'light';
}
