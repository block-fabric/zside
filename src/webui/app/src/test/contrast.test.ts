import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { contrastRatio, themeTokens } from '../lib/contrast';
import { DEFAULT_THEME, THEMES } from '../lib/themes';

const tokens = themeTokens(readFileSync(join(dirname(fileURLToPath(import.meta.url)), '../styles/themes.css'), 'utf8'));

/** Text colours on the backgrounds they are drawn on, and the ratio they need (WCAG AA: 4.5 for text, 3 for parts of controls and icons). */
const PAIRS: [fg: string, bg: string, min: number][] = [
  ...['bg', 'surface', 'surface-2', 'raised'].flatMap((bg): [string, string, number][] => [
    ['text', bg, 4.5],
    ['text-muted', bg, 4.5],
  ]),
  ...['bg', 'surface', 'surface-2'].map((bg): [string, string, number] => ['text-faint', bg, 4.5]),
  ...['bg', 'surface', 'surface-2'].flatMap((bg): [string, string, number][] => [
    ['link', bg, 4.5],
    ['positive', bg, 4.5],
    ['negative', bg, 4.5],
    ['warning', bg, 4.5],
    ['accent-fg', bg, 4.5],
  ]),
  ['on-accent', 'accent', 4.5],
  ['accent-strong', 'accent-soft', 4.5],
  ['text', 'accent-soft', 4.5],
  ['warning', 'warning-soft', 4.5],
  ['negative', 'danger-soft', 4.5],
  ['text', 'danger-soft', 4.5],
  ['positive', 'accent-soft', 4.5],
  ['bg', 'text', 4.5],
];

describe('the themes', () => {
  it('are all in the style sheet, Midnight the default', () => {
    const drawn = THEMES.filter((t) => t.id !== 'system').map((t) => t.id);
    expect(Object.keys(tokens).sort()).toEqual([...drawn].sort());
    expect(DEFAULT_THEME).toBe('midnight');
  });

  for (const [theme, vars] of Object.entries(tokens)) {
    it(`${theme}: every text colour passes WCAG AA on its backgrounds`, () => {
      const failures: string[] = [];
      for (const [fg, bg, min] of PAIRS) {
        expect(vars[fg], `--${fg}`).toMatch(/^#/);
        expect(vars[bg], `--${bg}`).toMatch(/^#/);
        const ratio = contrastRatio(vars[fg], vars[bg]);
        if (ratio < min) failures.push(`--${fg} on --${bg}: ${ratio.toFixed(2)} < ${min}`);
      }
      expect(failures).toEqual([]);
    });
  }
});

// The check of the audit (M5), kept with the one above: text reaches the 4.5:1 contrast of WCAG AA in
// every theme, each colour text is drawn in,
// on each colour it is drawn on.
const CSS = readFileSync(join(dirname(fileURLToPath(import.meta.url)), '../styles/themes.css'), 'utf8');

function themes(): Record<string, Record<string, string>> {
  const out: Record<string, Record<string, string>> = {};
  for (const m of CSS.matchAll(/(:root[^{]*)\{([^}]*)\}/g)) {
    if (/data-font|data-money/.test(m[1])) continue;
    const name = m[1].match(/data-theme='(\w+)'/)?.[1] ?? 'light';
    const vars: Record<string, string> = {};
    for (const v of m[2].matchAll(/--([\w-]+):\s*(#[0-9a-fA-F]{6})/g)) vars[v[1]] = v[2];
    out[name] = { ...(out[name] ?? {}), ...vars };
  }
  return out;
}

function luminance(hex: string): number {
  const [r, g, b] = [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16) / 255).map((v) => (v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4));
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

function contrast(a: string, b: string): number {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

const TEXT = ['text', 'text-muted', 'accent-fg', 'accent-strong', 'link', 'positive', 'negative', 'warning'];
const SURFACES = ['bg', 'surface', 'surface-2', 'raised'];

describe('the contrast of text', () => {
  const all = themes();

  it('is defined for every theme of the menus', () => {
    for (const t of THEMES.filter((t) => t.id !== 'system')) expect(Object.keys(all)).toContain(t.id);
  });

  for (const [name, v] of Object.entries(all)) {
    it(`reaches 4.5:1 in the ${name} theme`, () => {
      const failing: string[] = [];
      const check = (fg: string, bg: string) => {
        const r = contrast(v[fg], v[bg]);
        if (!(r >= 4.5)) failing.push(`${fg} on ${bg}: ${r.toFixed(2)}`);
      };
      for (const fg of TEXT) for (const bg of SURFACES) check(fg, bg);
      // Captions and hints: never drawn on controls (raised).
      for (const bg of ['bg', 'surface', 'surface-2']) check('text-faint', bg);
      // Buttons, and the badges.
      check('on-accent', 'accent');
      check('warning', 'warning-soft');
      check('accent-strong', 'accent-soft');
      check('negative', 'danger-soft');
      expect(failing).toEqual([]);
    });
  }
});
