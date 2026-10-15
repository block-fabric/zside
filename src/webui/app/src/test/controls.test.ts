import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { describe, expect, it } from 'vitest';
import { THEMES } from '../lib/themes';

// Our own dropdowns and scroll bars (user decision): the same on WebKitGTK and WebView2, in every look.
// The selects lose their native face for our chevron; the scroll bars are drawn by ::-webkit-scrollbar
// (Chromium ignores those as soon as scrollbar-width or scrollbar-color is set on the element).
const root = resolve(__dirname, '..');
const read = (file: string) => readFileSync(resolve(root, file), 'utf8');
const base = read('styles/base.css');
const themes = read('styles/themes.css');

/** Every file with styles: the stylesheets, and the <style> of each component. */
function sources(): { file: string; css: string }[] {
  const out: { file: string; css: string }[] = [];
  const walk = (dir: string) => {
    for (const name of readdirSync(dir)) {
      const path = join(dir, name);
      if (statSync(path).isDirectory()) {
        if (name !== 'test') walk(path);
      } else if (name.endsWith('.css')) out.push({ file: path, css: readFileSync(path, 'utf8') });
      else if (name.endsWith('.svelte')) {
        const style = /<style[^>]*>([\s\S]*?)<\/style>/.exec(readFileSync(path, 'utf8'));
        if (style) out.push({ file: path, css: style[1] });
      }
    }
  };
  walk(root);
  return out;
}

/** The innermost rules of a stylesheet: selector and body (comments dropped). */
function rules(css: string): { selector: string; body: string }[] {
  const text = css.replace(/\/\*[\s\S]*?\*\//g, '');
  return [...text.matchAll(/([^{}]+)\{([^{}]*)\}/g)].map((m) => ({ selector: m[1].trim(), body: m[2] }));
}

/** The declarations of the block of a look in themes.css. */
function themeBlock(id: string): string {
  const at = themes.indexOf(`:root[data-theme='${id}'] {`);
  expect(at, id).toBeGreaterThanOrEqual(0);
  return themes.slice(at, themes.indexOf('}', at));
}

describe('dropdowns', () => {
  it('no select keeps the native face: appearance none and our chevron', () => {
    const rule = rules(base).find((r) => r.selector === 'select:not([multiple]):not([size])');
    expect(rule).toBeTruthy();
    expect(rule!.body).toMatch(/(^|[\s;])appearance:\s*none/);
    expect(rule!.body).toMatch(/-webkit-appearance:\s*none/);
    expect(rule!.body).toMatch(/background-image:\s*var\(--chevron\)/);
    expect(rule!.body).toMatch(/padding-right:\s*2rem/);
    // A focus ring and a disabled state.
    expect(rules(base).some((r) => r.selector.split(',').map((x) => x.trim()).includes('select:focus-visible') && /box-shadow/.test(r.body))).toBe(true);
    expect(rules(base).some((r) => r.selector === 'select:disabled')).toBe(true);
  });

  it('no component gives a select back its native face, or drops the chevron', () => {
    for (const { file, css } of sources()) {
      for (const r of rules(css)) {
        if (!/(^|[\s,>+~(])select\b/.test(r.selector) || r.selector.startsWith('@')) continue;
        expect(r.body, `${file}: ${r.selector}`).not.toMatch(/appearance:\s*(auto|menulist)/);
        // The shorthand would reset the chevron (background-image): the colour alone.
        expect(r.body, `${file}: ${r.selector}`).not.toMatch(/(^|[\s;])background:/);
      }
    }
  });

  for (const theme of THEMES.filter((x) => x.id !== 'system')) {
    it(`${theme.id}: the chevron in the muted colour, the native parts in its scheme`, () => {
      const block = themeBlock(theme.id);
      const muted = /--text-muted:\s*(#[0-9a-f]{6})/i.exec(block)?.[1];
      expect(muted, theme.id).toBeTruthy();
      const chevron = /--chevron:\s*url\("([^"]+)"\)/.exec(block)?.[1];
      expect(chevron, theme.id).toBeTruthy();
      expect(decodeURIComponent(chevron!.replace('data:image/svg+xml,', ''))).toContain(`stroke='${muted}'`);
      expect(chevron).toContain('M6 9l6 6 6-6');
      expect(block).toMatch(new RegExp(`color-scheme:\\s*${theme.id === 'light' ? 'light' : 'dark'}`));
      expect(block).toMatch(/--line-strong:\s*#/);
    });
  }
});

describe('scroll bars', () => {
  it('thin, round, in the colours of the look, in both engines', () => {
    const all = rules(base);
    const body = (selector: string) => all.find((r) => r.selector.split(',').map((x) => x.trim()).includes(selector))?.body ?? '';
    expect(body('::-webkit-scrollbar')).toMatch(/width:\s*10px/);
    expect(body('::-webkit-scrollbar-track')).toMatch(/background:\s*transparent/);
    const thumb = body('::-webkit-scrollbar-thumb');
    expect(thumb).toMatch(/border-radius:\s*999px/);
    expect(thumb).toMatch(/background-color:\s*var\(--line-strong\)/);
    expect(body('::-webkit-scrollbar-thumb:hover')).toMatch(/background-color:\s*var\(--text-muted\)/);
  });

  it('the standard properties only where ::-webkit-scrollbar is not known (Chromium would ignore ours)', () => {
    for (const { file, css } of sources()) {
      const text = css.replace(/\/\*[\s\S]*?\*\//g, '');
      const outside = text.replace(/@supports not selector\(::-webkit-scrollbar\)\s*\{[^{}]*\{[^{}]*\}\s*\}/g, '');
      expect(outside, file).not.toMatch(/scrollbar-(width|color)\s*:/);
    }
  });
});
