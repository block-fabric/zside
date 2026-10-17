// Times, durations and sizes, as chains-qt writes them.
import { t, tn } from './i18n.svelte';


/** How long ago, in words (GUIUtil::formatNiceTimeOffset). */
export function niceTimeOffset(secs: number): string {
  const HOUR = 3600;
  const DAY = 24 * HOUR;
  const WEEK = 7 * DAY;
  const YEAR = 31556952;
  const s = Math.max(0, Math.floor(secs));
  if (s < 60) return tn('%n second(s)', s);
  if (s < 2 * HOUR) return tn('%n minute(s)', Math.floor(s / 60));
  if (s < 2 * DAY) return tn('%n hour(s)', Math.floor(s / HOUR));
  if (s < 2 * WEEK) return tn('%n day(s)', Math.floor(s / DAY));
  if (s < YEAR) return tn('%n week(s)', Math.floor(s / WEEK));
  return t('%1 and %2', tn('%n year(s)', Math.floor(s / YEAR)), tn('%n week(s)', Math.floor((s % YEAR) / WEEK)));
}

/** A duration, short: "3 d 4 h", "5 m 2 s" (GUIUtil::formatDurationStr). */
export function shortDuration(secs: number): string {
  let s = Math.max(0, Math.floor(secs));
  const parts: string[] = [];
  const days = Math.floor(s / 86400);
  s %= 86400;
  const hours = Math.floor(s / 3600);
  s %= 3600;
  const minutes = Math.floor(s / 60);
  s %= 60;
  if (days) parts.push(`${days} d`);
  if (hours) parts.push(`${hours} h`);
  if (minutes) parts.push(`${minutes} m`);
  if (s || parts.length === 0) parts.push(`${s} s`);
  return parts.slice(0, 2).join(' ');
}

/** Bytes as kB/MB/GB (GUIUtil::formatBytes: decimal units). */
export function formatBytes(bytes: number): string {
  if (bytes < 1_000) return `${bytes} B`;
  if (bytes < 1_000_000) return `${Math.floor(bytes / 1_000)} kB`;
  if (bytes < 1_000_000_000) return `${Math.floor(bytes / 1_000_000)} MB`;
  return `${Math.floor(bytes / 1_000_000_000)} GB`;
}

/** A date and time of the chain (seconds since 1970), in the local time. */
export function formatTime(secs: number, short = false): string {
  if (!secs) return '–';
  const d = new Date(secs * 1000);
  const pad = (n: number) => String(n).padStart(2, '0');
  // Short: the year left out (a narrow window).
  if (short) return `${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/** An integer with thin spaces between groups of three digits. */
export function formatCount(n: number): string {
  return String(Math.trunc(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ' ');
}

/** The middle of a long hash, left out: "0000a1b2…e3f4". */
export function shortHash(hash: string, keep = 8): string {
  return hash.length <= keep * 2 + 1 ? hash : `${hash.slice(0, keep)}…${hash.slice(-keep)}`;
}

/**
 * A character that is not shown as itself: every control (Cc), every format character (Cf: the marks that set the
 * direction of what follows, the invisible ones, the tag characters), the line and paragraph separators, and the letters
 * that show as nothing (the Hangul fillers, the combining grapheme joiner, the Mongolian and Khmer invisible marks). The
 * same set as the host's (guard::IsControlOrFormat, audit 7 #11).
 */
export function isControlOrFormat(cp: number): boolean {
  return (
    cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp === 0xad || cp === 0x034f || (cp >= 0x0600 && cp <= 0x0605) || cp === 0x061c ||
    cp === 0x06dd || cp === 0x070f || cp === 0x0890 || cp === 0x0891 || cp === 0x08e2 || cp === 0x115f || cp === 0x1160 ||
    cp === 0x17b4 || cp === 0x17b5 || (cp >= 0x180b && cp <= 0x180f) || (cp >= 0x200b && cp <= 0x200f) ||
    (cp >= 0x2028 && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x206f) || cp === 0x3164 || cp === 0xfeff || cp === 0xffa0 ||
    (cp >= 0xfff9 && cp <= 0xfffb) || cp === 0x110bd || cp === 0x110cd || (cp >= 0x13430 && cp <= 0x1343f) ||
    (cp >= 0x1bca0 && cp <= 0x1bca3) || (cp >= 0x1d173 && cp <= 0x1d17a) || (cp >= 0xe0000 && cp <= 0xe007f)
  );
}

/** A variation selector (U+FE00 to U+FE0F, U+E0100 to U+E01EF). */
export function isVariationSelector(cp: number): boolean {
  return (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef);
}

/** A character a variation selector may follow and change the look of: an emoji, a keycap, a CJK ideograph. */
function takesVariation(cp: number): boolean {
  return (
    (cp >= 0x30 && cp <= 0x39) || cp === 0x23 || cp === 0x2a || cp === 0xa9 || cp === 0xae || cp === 0x203c || cp === 0x2049 ||
    cp === 0x2122 || cp === 0x2139 || (cp >= 0x2194 && cp <= 0x21aa) || (cp >= 0x231a && cp <= 0x23ff) || cp === 0x24c2 ||
    (cp >= 0x25aa && cp <= 0x27bf) || cp === 0x2934 || cp === 0x2935 || (cp >= 0x2b05 && cp <= 0x2b55) || cp === 0x3030 ||
    cp === 0x303d || cp === 0x3297 || cp === 0x3299 || (cp >= 0x1f000 && cp <= 0x1faff) || (cp >= 0x3400 && cp <= 0x4dbf) ||
    (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0x20000 && cp <= 0x3134f)
  );
}

/** Whether a character is not shown as itself after `previous` (guard::IsHidden of the host). */
export function isHidden(previous: number, cp: number): boolean {
  return isControlOrFormat(cp) || (isVariationSelector(cp) && !takesVariation(previous));
}

/** The written-out form of a character: "[U+202E]". */
function codeOf(cp: number): string {
  return `[U+${cp.toString(16).toUpperCase().padStart(4, '0')}]`;
}

/**
 * Text of the chain shown as it is in a block of code (a message to copy, to sign): what is not shown as itself (a mark
 * that would turn it around, U+202E; an invisible one, U+200B) is written out, "[U+202E]", so that what is read is the
 * order of the characters (audit port-2 #16, audit 7 #11). Line breaks and tabs stay. The copy keeps the text as it is.
 */
export function revealBidi(text: string): string {
  let out = '';
  let previous = 0;
  for (const ch of text) {
    const cp = ch.codePointAt(0)!;
    out += cp !== 0x0a && cp !== 0x0d && cp !== 0x09 && isHidden(previous, cp) ? codeOf(cp) : ch;
    previous = cp;
  }
  return out;
}

/**
 * Text of outside (a label of a payment URI, a message) on one line: line breaks and every other control a space, spaces
 * folded, what is not shown as itself written out, cut to `max` characters with "…" (audit 7 #4).
 */
export function oneLine(text: string, max = 100): string {
  // eslint-disable-next-line no-control-regex
  const flat = text.replace(/[\u0000-\u001f\u007f-\u009f\u2028\u2029\u0085]/g, ' ').replace(/\s+/g, ' ').trim();
  const chars = [...revealBidi(flat)];
  return chars.length > max ? `${chars.slice(0, max - 1).join('').trimEnd()}…` : chars.join('');
}
