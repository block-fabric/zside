import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

// A highlighted button (the accent) under the pointer glows; it does not go dark (user decision): the
// hover of every .btn sets the raised background, and wins over .btn.primary unless its own hover keeps
// the accent.
const css = (file: string) => readFileSync(resolve(__dirname, file), 'utf8');
/** The body of the rule whose selector list ends with this selector (exactly), or null. */
const rule = (text: string, selector: string) => {
  const at = text.indexOf(`${selector} {`);
  if (at < 0) return null;
  const open = text.indexOf('{', at);
  return text.slice(open + 1, text.indexOf('}', open));
};

describe('highlighted buttons glow under the pointer', () => {
  const base = css('../styles/base.css');
  it('the glow is defined from the accent', () => {
    expect(base).toMatch(/--glow:[^;]*var\(--accent\)/);
  });
  it('no look redefines it (the ambient light of a look is --ambient: a colour, not a shadow)', () => {
    expect(css('../styles/themes.css')).not.toMatch(/--glow(-strong)?\s*:/);
  });
  for (const selector of ['.btn.primary:hover:not(:disabled)', '.btn.cta:hover:not(:disabled)']) {
    it(`${selector}: keeps the accent and glows`, () => {
      const body = rule(base, selector);
      expect(body, selector).not.toBeNull();
      expect(body!).toMatch(/background:[^;]*var\(--accent\)/);
      expect(body!).toMatch(/box-shadow:\s*var\(--glow(-strong)?\)/);
    });
  }
  it('the page shown in the sidebar and the area shown in the workspace glow too', () => {
    expect(rule(css('../components/Sidebar.svelte'), '.entry.current:hover')!).toMatch(/box-shadow:\s*var\(--glow\)/);
    expect(rule(css('../views/layouts/Workspace.svelte'), ".area[aria-current='true']:hover")!).toMatch(/background:\s*var\(--accent\)/);
  });
});
