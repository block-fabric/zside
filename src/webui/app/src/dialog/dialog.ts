// The page of the dialogs of the host (src/webui/dialog.h): what the app asks the user itself, in a window of its own
// with a web view of its own, never in the main page (audit port-1 #4). It has one bridge, __chainsDialog, which only
// its web view has:
//   hello  → what to show (text only: put in with textContent, never as HTML) and the look of the app;
//   ready  → the size it needs: the host shows the window then;
//   answer → the user's choice, once ({accept, passphrase?}).
// It draws the look of the app: the theme the host passes (a built-in one, or a theme of the user checked here as
// strictly as a theme file, lib/customthemes.ts), its fonts (built in) and text size. A theme of the user whose text
// cannot be read on the dialog is drawn as its base: what is asked is read before it is answered.
import { baseColors, checkStoredThemes, contrastOf, isDarkColor, isThemeChoice, parseColor, type BaseTheme, type CustomTheme, type TokenName } from '../lib/customthemes';
import { revealBidi } from '../lib/format';
import { applyTheme, clampFontAdjust, DEFAULT_THEME, isTheme, resolveTheme, type FontChoice } from '../lib/themes';

export type Level = 'info' | 'warning' | 'danger';

export interface DialogLook {
  theme: string;
  custom: unknown;
  fontAdjust: number;
  fontFamily: FontChoice;
  moneyFont: 'embedded' | 'system';
  rtl: boolean;
  lang: string;
}

export interface DialogData {
  title: string;
  message: string;
  details: string;
  accept: string;
  cancel: string;
  level: Level;
  passphrase: boolean;
  passphraseLabel: string;
  look: DialogLook;
}

export type DialogBridge = (method: string, params?: Record<string, unknown>) => Promise<unknown>;

/** How long after the window shows the button that accepts takes no click or key (a click or Enter meant for the main window). */
export const ARM_MS = 400;
/** The least contrast of text on its background (WCAG AA), and of what is not text (icons, the focus ring, the fill of a button). */
export const MIN_TEXT_CONTRAST = 4.5;
export const MIN_UI_CONTRAST = 3;

const FONTS: readonly FontChoice[] = ['', 'inter', 'nunito', 'mono', 'system'];

function str(value: unknown, max: number, fallback = ''): string {
  return typeof value === 'string' ? [...value].slice(0, max).join('') : fallback;
}

/** What the host sent, checked: every field of its type, text cut to its length; anything else its default. */
export function readData(raw: unknown): DialogData {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) throw new Error('The dialog has nothing to show.');
  const o = raw as Record<string, unknown>;
  const l = (o.look && typeof o.look === 'object' && !Array.isArray(o.look) ? o.look : {}) as Record<string, unknown>;
  const level = o.level === 'info' || o.level === 'danger' ? o.level : 'warning';
  const font = FONTS.includes(l.fontFamily as FontChoice) ? (l.fontFamily as FontChoice) : '';
  return {
    title: str(o.title, 200),
    message: str(o.message, 6000),
    details: str(o.details, 6000),
    accept: str(o.accept, 60, 'OK') || 'OK',
    cancel: str(o.cancel, 60, 'Cancel') || 'Cancel',
    level,
    passphrase: o.passphrase === true,
    passphraseLabel: str(o.passphraseLabel, 200),
    look: {
      theme: str(l.theme, 40, DEFAULT_THEME),
      custom: l.custom ?? null,
      fontAdjust: typeof l.fontAdjust === 'number' ? clampFontAdjust(l.fontAdjust) : 0,
      fontFamily: font,
      moneyFont: l.moneyFont === 'system' ? 'system' : 'embedded',
      rtl: l.rtl === true,
      lang: typeof l.lang === 'string' && /^[A-Za-z]{2,3}([_-][A-Za-z0-9@]{1,8})?$/.test(l.lang) ? l.lang : '',
    },
  };
}

/**
 * The colour tokens the dialog draws with (dialog.css uses no other): opaque backgrounds only (no gradient, no
 * transparency), text and marks over them.
 */
export const DIALOG_TOKENS = ['surface', 'surface-2', 'raised', 'text', 'text-muted', 'accent', 'on-accent', 'negative', 'warning', 'accent-fg', 'focus'] as const satisfies readonly TokenName[];
export type DialogToken = (typeof DIALOG_TOKENS)[number];

/**
 * Every pair of text and background the dialog draws (dialog.css): the title, the text and the details on the
 * surface, the footer and the details on surface-2, a button under the pointer on raised, the field's label in
 * text-muted, the borders of the buttons and of the field in text-muted, the button that accepts (on-accent on
 * accent), the one of a danger (negative, on surface-2 and raised).
 */
export const TEXT_PAIRS: readonly (readonly [DialogToken, DialogToken])[] = [
  ['text', 'surface'],
  ['text', 'surface-2'],
  ['text', 'raised'],
  ['text-muted', 'surface'],
  ['text-muted', 'surface-2'],
  ['text-muted', 'raised'],
  ['on-accent', 'accent'],
  ['negative', 'surface-2'],
  ['negative', 'raised'],
];
/**
 * What is not text: the icons of the levels and the focus ring. (The button that accepts is told by its label,
 * on-accent on accent above, and by being filled where the other is outlined in text-muted.)
 */
export const UI_PAIRS: readonly (readonly [DialogToken, DialogToken])[] = [
  ['warning', 'surface'],
  ['negative', 'surface'],
  ['accent-fg', 'surface'],
  ['focus', 'surface'],
  ['focus', 'surface-2'],
];

function failing(colors: Record<DialogToken, string>): (readonly [DialogToken, DialogToken])[] {
  const below = (min: number) => ([fg, bg]: readonly [DialogToken, DialogToken]) => {
    const ratio = contrastOf(colors[fg], colors[bg]);
    return ratio === null || ratio < min;
  };
  return [...TEXT_PAIRS.filter(below(MIN_TEXT_CONTRAST)), ...UI_PAIRS.filter(below(MIN_UI_CONTRAST))];
}

/** Whether every pair the dialog draws is legible with these colours (and every one of them opaque). */
export function legible(colors: Record<DialogToken, string>): boolean {
  return DIALOG_TOKENS.every((t) => parseColor(colors[t])?.a === 1) && failing(colors).length === 0;
}

/**
 * The colours of the dialog for a look: those of the built-in base, with the user's own where they are opaque
 * colours and every pair stays legible. A colour of the user that is transparent, or in a pair that fails, gives way
 * to the base's (both colours of the pair); when that is not enough, every colour is the base's. Never a failing pair.
 */
export function dialogColors(base: BaseTheme, custom: Pick<CustomTheme, 'colors'> | null): { colors: Record<DialogToken, string>; replaced: DialogToken[] } {
  const fallback = baseColors(base) as Record<DialogToken, string>;
  const colors = {} as Record<DialogToken, string>;
  const replaced = new Set<DialogToken>();
  for (const t of DIALOG_TOKENS) {
    const own = custom?.colors[t];
    if (own !== undefined && parseColor(own)?.a === 1) colors[t] = own.trim();
    else {
      colors[t] = fallback[t];
      if (own !== undefined) replaced.add(t);
    }
  }
  for (let pass = 0; pass < 3; pass++) {
    const bad = failing(colors);
    if (!bad.length) break;
    for (const [fg, bg] of bad) {
      for (const t of [fg, bg]) {
        if (colors[t] !== fallback[t]) replaced.add(t);
        colors[t] = fallback[t];
      }
    }
  }
  if (!legible(colors)) {
    for (const t of DIALOG_TOKENS) {
      if (colors[t] !== fallback[t]) replaced.add(t);
      colors[t] = fallback[t];
    }
  }
  return { colors, replaced: DIALOG_TOKENS.filter((t) => replaced.has(t)) };
}

/**
 * Put the look of the app on the dialog: the theme chosen (one of the user's only once checked as a theme file), its
 * colours for the dialog's tokens only as dialogColors keeps them legible, the font and the size, the language and its
 * direction. What is drawn: the choice, and the colours of the user that gave way to the base's.
 */
export function applyDialogLook(doc: Document, look: DialogLook, prefersDark: boolean): { choice: string; replaced: DialogToken[] } {
  const customs: CustomTheme[] = look.custom ? checkStoredThemes([look.custom]) : [];
  const choice = isThemeChoice(look.theme, customs) ? look.theme : DEFAULT_THEME;
  const custom = customs.find((c) => c.id === choice) ?? null;
  const base: BaseTheme = custom ? custom.base : resolveTheme(isTheme(choice) ? choice : DEFAULT_THEME, prefersDark);
  applyTheme(doc, base, look.fontAdjust, prefersDark, look.fontFamily, look.moneyFont);
  const root = doc.documentElement;
  const { colors, replaced } = dialogColors(base, custom);
  // Every colour of the dialog set one by one on its known token (never CSS text of a theme).
  for (const t of DIALOG_TOKENS) root.style.setProperty(`--${t}`, colors[t]);
  if (custom) root.dataset.themeChoice = 'custom';
  root.style.colorScheme = isDarkColor(colors.surface) ? 'dark' : 'light';
  root.lang = look.lang ? look.lang.replace('_', '-') : 'en';
  root.dir = look.rtl ? 'rtl' : 'ltr';
  return { choice, replaced };
}

const SVG = 'http://www.w3.org/2000/svg';
//! The icon of each level (stroked, 24×24).
const ICONS: Record<Level, string[]> = {
  info: ['M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18z', 'M12 11v6', 'M12 7.5v.01'],
  warning: ['M10.3 3.9L2.4 17.6a2 2 0 0 0 1.7 3h15.8a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z', 'M12 9v4.5', 'M12 17v.01'],
  danger: ['M8.5 2.5h7l5 5v7l-5 5h-7l-5-5v-7z', 'M12 7.5v5.5', 'M12 16.5v.01'],
};

function icon(doc: Document, level: Level): SVGSVGElement {
  const svg = doc.createElementNS(SVG, 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('aria-hidden', 'true');
  svg.setAttribute('class', 'icon');
  for (const d of ICONS[level]) {
    const path = doc.createElementNS(SVG, 'path');
    path.setAttribute('d', d);
    svg.append(path);
  }
  return svg;
}

function el<K extends keyof HTMLElementTagNameMap>(doc: Document, tag: K, props: { id?: string; className?: string; text?: string } = {}): HTMLElementTagNameMap[K] {
  const e = doc.createElement(tag);
  if (props.id) e.id = props.id;
  if (props.className) e.className = props.className;
  if (props.text !== undefined) e.textContent = props.text;
  return e;
}

export interface Rendered {
  root: HTMLElement;
  accept: HTMLButtonElement;
  cancel: HTMLButtonElement;
  field: HTMLInputElement | null;
}

/**
 * The dialog, made with the DOM: every text with textContent, the marks that turn text around written out
 * (revealBidi), each paragraph isolated as to direction.
 */
export function render(doc: Document, data: DialogData): Rendered {
  const root = el(doc, 'main', { id: 'dialog', className: `dialog ${data.level}` });
  root.setAttribute('role', 'alertdialog');
  root.setAttribute('aria-modal', 'true');
  root.setAttribute('aria-labelledby', 'dialog-title');
  root.setAttribute('aria-describedby', 'dialog-message');
  root.dataset.level = data.level;

  const header = el(doc, 'header');
  header.append(icon(doc, data.level));
  const title = el(doc, 'h1', { id: 'dialog-title', text: revealBidi(data.title) });
  title.dir = 'auto';
  header.append(title);

  const body = el(doc, 'div', { className: 'body' });
  const message = el(doc, 'div', { id: 'dialog-message', className: 'message' });
  for (const part of data.message.split(/\n{2,}/)) {
    if (!part.trim()) continue;
    const p = el(doc, 'p', { text: revealBidi(part) });
    p.dir = 'auto';
    message.append(p);
  }
  body.append(message);
  if (data.details) {
    const details = el(doc, 'pre', { id: 'dialog-details', className: 'details', text: revealBidi(data.details) });
    details.dir = 'ltr';
    details.setAttribute('translate', 'no');
    body.append(details);
  }
  let field: HTMLInputElement | null = null;
  if (data.passphrase) {
    const label = el(doc, 'label', { className: 'field' });
    label.append(el(doc, 'span', { text: revealBidi(data.passphraseLabel || 'Passphrase') }));
    field = el(doc, 'input', { id: 'dialog-passphrase' });
    field.type = 'password';
    field.autocomplete = 'off';
    field.spellcheck = false;
    field.setAttribute('autocapitalize', 'off');
    field.maxLength = 1024;
    label.append(field);
    body.append(label);
  }

  const footer = el(doc, 'footer');
  const cancel = el(doc, 'button', { id: 'dialog-cancel', className: 'btn', text: revealBidi(data.cancel) });
  cancel.type = 'button';
  const accept = el(doc, 'button', { id: 'dialog-accept', className: `btn ${data.level === 'danger' ? 'danger' : 'primary'}` });
  accept.type = 'button';
  // A danger is told by its mark and its heavy border too, not by its colour alone.
  if (data.level === 'danger') accept.append(icon(doc, 'danger'));
  accept.append(el(doc, 'span', { text: revealBidi(data.accept) }));
  footer.append(cancel, accept);

  root.append(header, body, footer);
  doc.title = data.title || doc.title;
  return { root, accept, cancel, field };
}

/** The elements Tab goes through, in order: the field, Cancel, the button that accepts. */
function focusables(r: Rendered): HTMLElement[] {
  return [r.field, r.cancel, r.accept].filter((x): x is HTMLInputElement | HTMLButtonElement => !!x && !x.disabled);
}

export interface Controller {
  /** The window shows: the button that accepts takes clicks and keys after ARM_MS. */
  arm(now?: number): void;
  /** Whether the dialog has been answered. */
  answered(): boolean;
  /** Its keys no longer listened to (the tests). */
  dispose(): void;
}

/**
 * The buttons and the keys of the dialog: Escape cancels, Enter does what the focus is on (Cancel at first, the safe
 * default; in the passphrase field, accept), Tab stays in the dialog. `answer` is called once.
 */
export function wire(doc: Document, r: Rendered, answer: (accept: boolean, passphrase?: string) => void, clock: () => number = () => performance.now()): Controller {
  let armedAt = Number.POSITIVE_INFINITY;
  let done = false;
  const armed = () => clock() >= armedAt;
  const finish = (accept: boolean) => {
    if (done) return;
    if (accept && !armed()) return;
    let passphrase: string | undefined;
    if (accept && r.field) {
      passphrase = r.field.value;
      // Nothing typed: the field is needed.
      if (!passphrase) {
        r.field.setAttribute('aria-invalid', 'true');
        r.field.focus();
        return;
      }
    }
    done = true;
    if (r.field) r.field.value = '';
    r.accept.disabled = true;
    r.cancel.disabled = true;
    answer(accept, passphrase);
  };
  r.cancel.addEventListener('click', () => finish(false));
  r.accept.addEventListener('click', () => finish(true));
  const onKey = (e: KeyboardEvent) => {
    if (e.key === 'Escape') {
      e.preventDefault();
      finish(false);
    } else if (e.key === 'Enter') {
      const target = doc.activeElement;
      e.preventDefault();
      if (target === r.accept) finish(true);
      else if (r.field && target === r.field) finish(true);
      else finish(false);
    } else if (e.key === 'Tab') {
      const list = focusables(r);
      if (!list.length) return;
      e.preventDefault();
      const at = list.indexOf(doc.activeElement as HTMLElement);
      const next = at < 0 ? 0 : (at + (e.shiftKey ? list.length - 1 : 1)) % list.length;
      list[next].focus();
    }
  };
  doc.addEventListener('keydown', onKey);
  return {
    arm(now = clock()) {
      armedAt = now + ARM_MS;
      (r.field ?? r.cancel).focus();
    },
    answered: () => done,
    dispose: () => doc.removeEventListener('keydown', onKey),
  };
}

/** The size the dialog needs, in CSS pixels: as wide as its own width, as high as its content. */
export function measure(doc: Document, root: HTMLElement): { width: number; height: number } {
  doc.documentElement.classList.add('measuring');
  const rect = root.getBoundingClientRect();
  doc.documentElement.classList.remove('measuring');
  return { width: Math.ceil(rect.width), height: Math.ceil(rect.height) };
}

/** The whole dialog: asks the host what to show, draws it in the look of the app, says its size, gives the answer. */
export async function start(bridge: DialogBridge, doc: Document = document, win: Window = window): Promise<Controller> {
  const data = readData(await bridge('hello', {}));
  const prefersDark = typeof win.matchMedia === 'function' && win.matchMedia('(prefers-color-scheme: dark)').matches;
  applyDialogLook(doc, data.look, prefersDark);
  const r = render(doc, data);
  doc.body.replaceChildren(r.root);
  const control = wire(doc, r, (accept, passphrase) => {
    void bridge('answer', passphrase === undefined ? { accept } : { accept, passphrase });
  });
  // The fonts first: the size depends on them.
  if (doc.fonts?.ready) await Promise.race([doc.fonts.ready, new Promise((resolve) => setTimeout(resolve, 1500))]);
  await bridge('ready', measure(doc, r.root));
  doc.body.classList.remove('loading');
  control.arm();
  return control;
}
