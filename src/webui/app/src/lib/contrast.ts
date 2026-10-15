// Contrast of colours (WCAG 2.x): the relative luminance of sRGB colours and their ratio. Used by
// the tests of the themes (test/contrast.test.ts).

/** "#rgb" or "#rrggbb" to [r, g, b] in 0…255. */
export function parseHex(hex: string): [number, number, number] {
  let h = hex.trim().replace(/^#/, '');
  if (h.length === 3) h = [...h].map((c) => c + c).join('');
  if (!/^[0-9a-f]{6}$/i.test(h)) throw new Error(`not a colour: ${hex}`);
  return [0, 2, 4].map((i) => parseInt(h.slice(i, i + 2), 16)) as [number, number, number];
}

export function luminance(hex: string): number {
  const [r, g, b] = parseHex(hex).map((v) => {
    const c = v / 255;
    return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

export function contrastRatio(a: string, b: string): number {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

/** The custom properties of each theme of a style sheet as themes.css writes them (the light theme is also :root). */
export function themeTokens(css: string): Record<string, Record<string, string>> {
  const out: Record<string, Record<string, string>> = {};
  const block = /([^{}]+)\{([^{}]*)\}/g;
  for (const m of css.replace(/\/\*[\s\S]*?\*\//g, '').matchAll(block)) {
    const names = [...m[1].matchAll(/data-theme='([a-z]+)'/g)].map((x) => x[1]);
    if (!names.length) continue;
    const vars: Record<string, string> = {};
    for (const d of m[2].matchAll(/--([a-z0-9-]+)\s*:\s*([^;]+);/g)) vars[d[1]] = d[2].trim();
    for (const n of names) out[n] = { ...(out[n] ?? {}), ...vars };
  }
  // Each theme over the defaults (the light theme's :root).
  const base = out.light ?? {};
  for (const n of Object.keys(out)) out[n] = { ...base, ...out[n] };
  return out;
}
