// The fixes of the audit of the port, round 1 (AUDIT-port-1.md), in the interface of the kit.
// The host's part (#3: console.run and app.open confirmed by the desktop) is in nodeapi_tests.cpp and node.js;
// the direction of text from outside (#2) in i18n-safety.test.ts.
import { flushSync } from 'svelte';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { render } from '@testing-library/svelte';
import { afterEach, describe, expect, it } from 'vitest';
import Toasts from '../components/Toasts.svelte';
import { toast, toasts } from '../lib/state.svelte';

function rect(left: number, top: number, width: number, height: number): DOMRect {
  return { left, top, width, height, right: left + width, bottom: top + height, x: left, y: top, toJSON: () => ({}) } as DOMRect;
}

afterEach(() => {
  toasts.list.length = 0;
  document.body.replaceChildren();
});

describe('#11: notices leave a form the user works in', () => {
  function formWithFocus(box: DOMRect): HTMLInputElement {
    const form = document.createElement('form');
    const input = document.createElement('input');
    form.append(input);
    document.body.append(form);
    form.getBoundingClientRect = () => box;
    input.focus();
    return input;
  }

  it('go to the top corner while the focus is in a form under the bottom corner, and come back after', async () => {
    Object.defineProperty(window, 'innerWidth', { value: 1200, configurable: true });
    Object.defineProperty(window, 'innerHeight', { value: 800, configurable: true });
    const view = render(Toasts);
    const input = formWithFocus(rect(700, 500, 495, 260));
    toast('error', 'Error', 'The order was refused.');
    flushSync();
    const stack = view.container.querySelector('.toasts')!;
    expect(stack.classList.contains('top')).toBe(true);
    input.blur();
    await new Promise((r) => setTimeout(r, 5));
    flushSync();
    expect(stack.classList.contains('top')).toBe(false);
  });

  it('go to the top corner when they would cover more fields and buttons at the bottom than at the top', () => {
    Object.defineProperty(window, 'innerWidth', { value: 1200, configurable: true });
    Object.defineProperty(window, 'innerHeight', { value: 800, configurable: true });
    const button = document.createElement('button');
    const plain = document.createElement('div');
    document.body.append(button, plain);
    const doc = document as unknown as { elementsFromPoint?: (x: number, y: number) => Element[] };
    const before = doc.elementsFromPoint;
    try {
      // A Deposit button under the bottom corner; nothing to use under the top one.
      doc.elementsFromPoint = (_x: number, y: number) => [y > 400 ? button : plain];
      const view = render(Toasts);
      toast('success', 'Merged mining on', 'A block is asked for with every block of Chains.');
      flushSync();
      expect(view.container.querySelector('.toasts')!.classList.contains('top')).toBe(true);
      // Something under both: they stay at the bottom.
      doc.elementsFromPoint = () => [button];
      toast('info', 'Another');
      flushSync();
      expect(view.container.querySelector('.toasts')!.classList.contains('top')).toBe(false);
    } finally {
      doc.elementsFromPoint = before;
    }
  });

  it('stay in the bottom corner when the form is elsewhere', () => {
    Object.defineProperty(window, 'innerWidth', { value: 1200, configurable: true });
    Object.defineProperty(window, 'innerHeight', { value: 800, configurable: true });
    const view = render(Toasts);
    formWithFocus(rect(240, 80, 400, 300));
    toast('error', 'Error', 'The order was refused.');
    flushSync();
    expect(view.container.querySelector('.toasts')!.classList.contains('top')).toBe(false);
  });
});

describe('#11: words cut short', () => {
  const read = (path: string) => readFileSync(join(__dirname, '..', path), 'utf8');

  it('the theme picker of the sidebar is as wide as its longest theme', () => {
    const css = read('components/Sidebar.svelte');
    const look = css.slice(css.indexOf('  .look {'), css.indexOf('}', css.indexOf('  .look {')));
    expect(look).toMatch(/flex: 1 0 auto;/);
    expect(look).not.toMatch(/min-width: 6\.5rem/);
  });

  it('the labels of the cards of Home wrap instead of being cut', () => {
    for (const file of ['views/home/Home.svelte', 'views/home/SideHome.svelte']) {
      const css = read(file);
      const dt = css.slice(css.indexOf('  .pairs dt {'), css.indexOf('}', css.indexOf('  .pairs dt {')));
      expect(dt).not.toMatch(/text-overflow: ellipsis/);
      expect(dt).not.toMatch(/white-space: nowrap/);
    }
  });
});
