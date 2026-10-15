// The language of the interface. The messages come from the catalogs of chains-qt (made into
// public/locales by scripts/i18n.mjs), as the host gives them (app.locale).
//
// The interface is written in English, and only its own words are translated: translation is opt-in.
// - The text written in the templates of the components (static text, and the title, placeholder,
//   aria-label and alt attributes, with what is put in them) is made into calls of t() when the
//   components are compiled (scripts/i18n-preprocess.mjs). Only the template is looked up in the
//   catalog; the values put in it are spliced in as they are, never translated nor matched.
// - Words of the app held in code (the labels of tabs and menus, the titles of dialogs) are translated
//   where they are shown, with t() or tn().
// - Text from the chain, peers, the wallet or the user is never translated: it is shown with SafeText
//   (components/SafeText.svelte, translate="no"), and a test (i18n-lint.test.ts) finds the places that
//   show such fields without it.
// Nothing looks at the page (no MutationObserver): a label that reads like a message of the app is shown
// as it is, in every language.
//
// Only the source of a message of the app is looked up, as a whole: t('Sent to %1', label) finds
// "Sent to %1" in the catalog, and the label is put in its place, never looked at. Nothing is matched
// against text the app put together (a toast with an error of the host, "3 hours behind", a message of
// the node): such text is shown as it is. A message of the app that has values in it is written with
// its own placeholders, t('%1 behind', duration), where it is made.
import { on, request } from './bridge';

export type Message = string | string[];

export interface Locale {
  code: string;
  name: string;
  rtl: boolean;
  messages: Record<string, Message>;
}

/** The language shown ('' for English), for the parts of the interface that follow it. */
export const i18n = $state({ code: '', name: 'English', rtl: false, version: 0 });

interface Pattern {
  source: string;
  /** The placeholders of the message, in the order they come in it ("%1", "%n"...). */
  holders: string[];
  value: Message;
}

let exact = new Map<string, Message>();
/** The same, by the message in lower case: the interface writes "Pay to" where chains-qt wrote "Pay To". */
let folded = new Map<string, Message>();
let bySource = new Map<string, Pattern>();

/** What a template of the app becomes: the message found and what its placeholders hold (in the template). */
interface Resolved {
  value: Message;
  values: Record<string, string>;
}
const templateCache = new Map<string, Resolved | null>();

const HOLDER = /%[0-9n]/g;

/** Use a catalog (null: English). */
export function setCatalog(locale: Locale | null): void {
  exact = new Map();
  folded = new Map();
  bySource = new Map();
  templateCache.clear();
  i18n.code = locale?.code ?? '';
  i18n.name = locale?.name ?? 'English';
  i18n.rtl = !!locale?.rtl;
  for (const [source, value] of Object.entries(locale?.messages ?? {})) {
    const holders = source.match(HOLDER) ?? [];
    if (holders.length === 0) {
      if (typeof value === 'string') {
        exact.set(source, value);
        const key = source.toLowerCase();
        if (!folded.has(key)) folded.set(key, value);
      }
      continue;
    }
    bySource.set(source, { source, holders, value });
  }
  i18n.version++;
}

const CANONICAL = ['zero', 'one', 'two', 'few', 'many', 'other'];

/** Which of the plural forms of a message goes with the number, in the order of the CLDR categories of the language. */
export function pluralIndex(code: string, n: number, count: number): number {
  if (count <= 1) return 0;
  let rules: Intl.PluralRules;
  try {
    rules = new Intl.PluralRules(code.replace('_', '-').replace(/@.*$/, ''));
  } catch {
    return n === 1 ? 0 : Math.min(1, count - 1);
  }
  const categories = CANONICAL.filter((c) => rules.resolvedOptions().pluralCategories.includes(c as Intl.LDMLPluralRule));
  const index = categories.indexOf(rules.select(n));
  return Math.max(0, Math.min(index, count - 1));
}

// Written as code points: the compiler of Svelte warns of marks of direction in the source.
const FSI = String.fromCharCode(0x2068);
const PDI = String.fromCharCode(0x2069);
const LRI = String.fromCharCode(0x2066);
const RLI = String.fromCharCode(0x2067);
/** What can turn the direction of the text: a letter of a right-to-left script, or a mark of direction. */
const BIDI = new RegExp('[\\u0590-\\u08FF\\u200E\\u200F\\u202A-\\u202E\\u2066-\\u2069\\uFB1D-\\uFDFF\\uFE70-\\uFEFF]|\\uD802[\\uDC00-\\uDFFF]|\\uD803[\\uDC00-\\uDFFF]|\\uD83A[\\uDC00-\\uDFFF]|\\uD83B[\\uDC00-\\uDFFF]');

/**
 * A value put in a message of the app, kept to itself as to its direction (audit port-1 #2): a market title
 * that starts with U+202E would otherwise turn the rest of the sentence around, amounts included. The value
 * goes between FSI and PDI (Unicode bidi isolates); a PDI of its own without its opener is dropped, and the
 * isolates it opens are closed, so that nothing in it reaches past its end. Plain ASCII cannot turn the
 * text and is left as it is.
 */
export function isolate(value: string): string {
  if (!BIDI.test(value)) return value;
  let open = 0;
  let out = '';
  for (const ch of value) {
    if (ch === LRI || ch === RLI || ch === FSI) open++;
    else if (ch === PDI) {
      if (open === 0) continue;
      open--;
    }
    out += ch;
  }
  return FSI + out + PDI.repeat(open) + PDI;
}

/** Put the values in place of the placeholders, in one pass: what the values hold is not looked at again. */
function fill(text: string, values: Record<string, string>): string {
  return text.replace(HOLDER, (h) => values[h] ?? h);
}

function numberOf(text: string): number {
  return Number(text.replace(/[^\d-]/g, ''));
}

function choose(value: Message, n: () => number): string {
  return Array.isArray(value) ? value[pluralIndex(i18n.code, n(), value.length)] : value;
}

function template(source: string): Resolved | null {
  const cached = templateCache.get(source);
  if (cached !== undefined) return cached;
  // The source as a whole (in lower case too: the interface writes "Pay to" where chains-qt wrote "Pay
  // To"); its placeholders stand for the values given with it.
  let found: Resolved | null = null;
  const direct = exact.get(source) ?? (/\s/.test(source) ? folded.get(source.toLowerCase()) : undefined);
  if (typeof direct === 'string') {
    found = { value: direct, values: {} };
  } else {
    const own = bySource.get(source);
    if (own) found = { value: own.value, values: Object.fromEntries(own.holders.map((h) => [h, h])) };
  }
  if (templateCache.size > 5000) templateCache.clear();
  templateCache.set(source, found);
  return found;
}

/**
 * A message of the app in the language shown, with %1, %2... filled in. Only the source, a message
 * written in the app, is looked up in the catalog, as a whole; the values are put in as they are.
 */
export function t(source: string, ...args: unknown[]): string {
  void i18n.version;
  const values: Record<string, string> = Object.fromEntries(args.map((a, i) => [`%${i + 1}`, a === null || a === undefined ? '' : isolate(String(a))]));
  const found = template(source);
  if (!found) return fill(source, values);
  const form = choose(found.value, () => numberOf(fill(found.values['%n'] ?? '0', values)));
  return fill(fill(form, found.values), values);
}

/** A message with plural forms (%n), in the language shown. */
export function tn(source: string, n: number, ...args: unknown[]): string {
  void i18n.version;
  const values: Record<string, string> = { '%n': String(n), ...Object.fromEntries(args.map((a, i) => [`%${i + 1}`, a === null || a === undefined ? '' : isolate(String(a))])) };
  const found = template(source);
  // In English (or without the message), "(s)" is "s" but for one, as the English forms of chains-qt.
  if (!found) return fill(source.replace(/\(s\)/g, n === 1 ? '' : 's'), values);
  const form = choose(found.value, () => n);
  return fill(fill(form, found.values), values);
}

/** Show the page in the language of the catalog now (after setCatalog): its language and direction. */
export function refreshPage(): void {
  if (typeof document === 'undefined') return;
  document.documentElement.lang = i18n.code ? i18n.code.replace('_', '-') : 'en';
  document.documentElement.dir = i18n.rtl ? 'rtl' : 'ltr';
}

/** The language of the host, now; and again whenever it changes (the language option, applied). */
export async function loadLocale(): Promise<void> {
  try {
    const locale = await request<Locale>('host', 'app.locale', {});
    setCatalog(locale.code ? locale : null);
  } catch {
    setCatalog(null);
  }
  refreshPage();
}

let following = false;
export function followLocale(): void {
  if (following) return;
  following = true;
  on('locale', () => {
    loadLocale();
  });
}
