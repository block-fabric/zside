import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

// The tabs of a page (Sidechains, Node window...): the tab shown has no shadow (user decision: the
// inset underline looked broken in the rounded button).
describe('page tabs', () => {
  it('the selected tab has no shadow', () => {
    const css = readFileSync(resolve(__dirname, '../styles/base.css'), 'utf8');
    const rule = css.match(/\.tabs button\[aria-selected='true'\]\s*\{([^}]*)\}/);
    expect(rule).not.toBeNull();
    expect(rule![1]).not.toMatch(/box-shadow/);
    expect(rule![1]).toMatch(/background:\s*var\(--raised\)/);
  });
});
