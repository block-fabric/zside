// The page of the dialogs of the host (src/webui/dialog.h): what the app asks the user itself, in a window of its own
// with a web view of its own, never in the main page (audit port-1 #4). It has one bridge, __chainsDialog, which only
// its web view has:
//   hello  → what to show (text only: put in with textContent, never as HTML) and the look of the app;
//   ready  → the size it needs: the host shows the window then;
//   answer → the user's choice, once ({accept, passphrase?}).
// It draws the look of the app: the theme the host passes (a built-in one, or a theme of the user checked here as
// strictly as a theme file, lib/customthemes.ts), its fonts (built in) and text size. A theme of the user whose text
// cannot be read on the dialog is drawn as its base: what is asked is read before it is answered.
import { baseColors, checkStoredThemes, contrastOf, isDarkColor, isThemeChoice, parseColor, toHex, type BaseTheme, type CustomTheme, type TokenName } from '../lib/customthemes';
import { revealBidi } from '../lib/format';
import { applyTheme, clampFontAdjust, DEFAULT_THEME, isTheme, resolveTheme, type FontChoice } from '../lib/themes';

export type Level = 'info' | 'warning' | 'danger';

/** A fact the host decoded (dialog.h Row): a destination, whole, before its amount; the fee; the total; what a vote changes. */
export type RowKind = 'text' | 'address' | 'amount' | 'total' | 'heading' | 'note';
export interface Row {
  label: string;
  value: string;
  kind: RowKind;
}
const ROW_KINDS: readonly RowKind[] = ['text', 'address', 'amount', 'total', 'heading', 'note'];
/** At most so many facts, and so long (the host cuts them so already). */
const MAX_ROWS = 81;

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
  /** An error shown at the field: the passphrase typed before was wrong. */
  error: string;
  rows: Row[];
  look: DialogLook;
}

export type DialogBridge = (method: string, params?: Record<string, unknown>) => Promise<unknown>;

/**
 * How long after the window shows with the focus the button that accepts takes no click or key (a click or Enter meant for
 * the main window, a "game" of the page that puts the user's pointer where the button comes): as the dialogs of Firefox
 * (audit 7 #7). The delay starts again each time the window shows again or gets the focus back.
 */
export const ARM_MS = 1000;
/** Without a word of the host that the window shows (dialog.h), the delay starts this long after the page is ready. */
export const SHOWN_WAIT_MS = 1500;
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
    error: str(o.error, 400),
    rows: Array.isArray(o.rows)
      ? o.rows.slice(0, MAX_ROWS).flatMap((raw): Row[] => {
          if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return [];
          const r = raw as Record<string, unknown>;
          const kind = ROW_KINDS.includes(r.kind as RowKind) ? (r.kind as RowKind) : 'text';
          return [{ label: str(r.label, 60), value: str(r.value, 2000), kind }];
        })
      : [],
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
 * accent), the one of a danger (negative, on surface-2 and raised), the error of a wrong passphrase (negative on surface).
 */
export const TEXT_PAIRS: readonly (readonly [DialogToken, DialogToken])[] = [
  ['text', 'surface'],
  ['text', 'surface-2'],
  ['text', 'raised'],
  ['text-muted', 'surface'],
  ['text-muted', 'surface-2'],
  ['text-muted', 'raised'],
  ['on-accent', 'accent'],
  ['negative', 'surface'],
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
    const parsed = own === undefined ? null : parseColor(own);
    // Drawn as the hex of what was parsed: what is checked is what is drawn (audit 7 #9).
    if (parsed?.a === 1) colors[t] = toHex(parsed);
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
  const root = el(doc, 'main', { id: 'dialog', className: `dialog ${data.level}${data.rows.length ? ' wide' : ''}` });
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
  if (data.rows.length) body.append(renderRows(doc, data.rows));
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
    if (data.error) {
      const error = el(doc, 'span', { id: 'dialog-error', className: 'error', text: revealBidi(data.error) });
      error.setAttribute('role', 'alert');
      field.setAttribute('aria-invalid', 'true');
      field.setAttribute('aria-describedby', 'dialog-error');
      label.append(error);
    }
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

/**
 * The facts, one a line, in the order the host gave them: a heading over the facts of one recipient, a note across, else
 * a label and its value. A destination is shown whole in a monospaced font (wrapped, never cut), left to right.
 */
function renderRows(doc: Document, rows: Row[]): HTMLElement {
  const box = el(doc, 'dl', { id: 'dialog-rows', className: 'rows' });
  for (const row of rows) {
    if (row.kind === 'heading') {
      const h = el(doc, 'div', { className: 'row heading', text: revealBidi(row.value ? `${row.label} ${row.value}` : row.label) });
      h.dir = 'auto';
      box.append(h);
      continue;
    }
    if (row.kind === 'note') {
      const n = el(doc, 'div', { className: 'row note', text: revealBidi(row.value) });
      n.dir = 'auto';
      box.append(n);
      continue;
    }
    const line = el(doc, 'div', { className: `row ${row.kind}` });
    line.dataset.kind = row.kind;
    const dt = el(doc, 'dt', { text: revealBidi(row.label) });
    const dd = el(doc, 'dd', { text: revealBidi(row.value) });
    if (row.kind === 'address' || row.kind === 'amount' || row.kind === 'total') {
      dd.dir = 'ltr';
      dd.setAttribute('translate', 'no');
    } else {
      dd.dir = 'auto';
    }
    line.append(dt, dd);
    box.append(line);
  }
  return box;
}

/** The elements Tab goes through, in order: the field, Cancel, the button that accepts. */
function focusables(r: Rendered): HTMLElement[] {
  return [r.field, r.cancel, r.accept].filter((x): x is HTMLInputElement | HTMLButtonElement => !!x && !x.disabled);
}

export interface Controller {
  /**
   * The window shows with the focus: the button that accepts takes clicks and keys ARM_MS after `now`, and only a press
   * that starts after that (never one held from before, nor a key repeated by the keyboard).
   */
  arm(now?: number): void;
  /** The window hides or loses the focus: the button that accepts waits for the next arm. */
  disarm(): void;
  /** Whether the dialog has been answered. */
  answered(): boolean;
  /** Its keys no longer listened to (the tests). */
  dispose(): void;
}

/**
 * The buttons and the keys of the dialog: Escape cancels, Enter does what the focus is on (Cancel at first, the safe
 * default; in the passphrase field, accept), Tab stays in the dialog. `answer` is called once.
 *
 * The button that accepts (audit 7 #7) takes a click only when the press of the pointer that made it started after the
 * dialog was armed, and a key only when it was pressed after (a key held down repeats: those are never taken).
 */
export function wire(doc: Document, r: Rendered, answer: (accept: boolean, passphrase?: string) => void, clock: () => number = () => performance.now()): Controller {
  let armedAt = Number.POSITIVE_INFINITY;
  let done = false;
  // The press of the pointer not clicked yet, and the last Enter or Space: when they started.
  let pressAt: number | null = null;
  let keyAt: number | null = null;
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
  const onPress = () => {
    pressAt = clock();
  };
  doc.addEventListener('pointerdown', onPress, true);
  doc.addEventListener('mousedown', onPress, true);
  r.cancel.addEventListener('click', () => finish(false));
  r.accept.addEventListener('click', () => {
    // A click of the pointer counts from its press; a click of a key (Space, Enter) from the key; one of neither (the
    // tests, assistive tools) from now.
    const started = pressAt ?? keyAt ?? clock();
    pressAt = null;
    if (started < armedAt) return;
    finish(true);
  });
  const onKey = (e: KeyboardEvent) => {
    if (e.key === 'Enter' || e.key === ' ') {
      // A key held down repeats: never taken (it was pressed before the dialog showed, for the main window).
      if (e.repeat) {
        e.preventDefault();
        return;
      }
      keyAt = clock();
    }
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
      if (done) return;
      armedAt = now + ARM_MS;
      // A press or a key from before does not count.
      pressAt = null;
      keyAt = null;
      (r.field ?? r.cancel).focus();
    },
    disarm() {
      armedAt = Number.POSITIVE_INFINITY;
    },
    answered: () => done,
    dispose: () => {
      doc.removeEventListener('keydown', onKey);
      doc.removeEventListener('pointerdown', onPress, true);
      doc.removeEventListener('mousedown', onPress, true);
    },
  };
}

/** The size the dialog needs, in CSS pixels: as wide as its own width, as high as its content. */
export function measure(doc: Document, root: HTMLElement): { width: number; height: number } {
  doc.documentElement.classList.add('measuring');
  const rect = root.getBoundingClientRect();
  doc.documentElement.classList.remove('measuring');
  return { width: Math.ceil(rect.width), height: Math.ceil(rect.height) };
}

declare global {
  interface Window {
    /** The host says the window shows with the focus (true), or hides or lost it (false): dialog.h, window_gtk.cpp, window_win32.cpp. */
    __chainsDialogArm?: (on: boolean) => void;
  }
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
  // The delay of the button that accepts starts when the window is on the screen with the focus, as the host and the
  // page see it, and again each time it comes back (audit 7 #7).
  let shown = false;
  win.__chainsDialogArm = (on: boolean) => {
    shown = true;
    if (on) control.arm();
    else control.disarm();
  };
  win.addEventListener('blur', () => control.disarm());
  win.addEventListener('focus', () => {
    shown = true;
    control.arm();
  });
  doc.addEventListener('visibilitychange', () => {
    if (doc.visibilityState === 'hidden') control.disarm();
    else if (shown) control.arm();
  });
  // The fonts first: the size depends on them.
  if (doc.fonts?.ready) await Promise.race([doc.fonts.ready, new Promise((resolve) => setTimeout(resolve, 1500))]);
  await bridge('ready', measure(doc, r.root));
  doc.body.classList.remove('loading');
  // The focus on the safe default from the start (Cancel, or the field), armed or not.
  (r.field ?? r.cancel).focus();
  // Without a word of the host that the window shows (a platform that says nothing), from a moment after now.
  setTimeout(() => {
    if (!shown) control.arm();
  }, SHOWN_WAIT_MS);
  return control;
}
