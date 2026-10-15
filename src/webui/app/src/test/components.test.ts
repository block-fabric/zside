import { fireEvent, render, screen } from '@testing-library/svelte';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import Amount from '../components/Amount.svelte';
import { setTransport } from '../lib/bridge';
import { applyTheme } from '../lib/themes';
import TableHarness from './TableHarness.svelte';

setTransport(async () => true);

describe('components', () => {
  it('shows amounts in the unit, masked in privacy mode', () => {
    const { container, rerender } = render(Amount, { sats: 123456789n, unit: 'CHN', masked: false });
    expect(container.textContent).toContain('1.23456789');
    expect(container.textContent).toContain('CHN');
    rerender({ sats: 123456789n, unit: 'CHN', masked: true });
    expect(container.textContent).not.toContain('1.23456789');
  });

  it('draws a few rows of 100 000, sorts and filters them', async () => {
    render(TableHarness, { count: 100_000 });
    const grid = screen.getByRole('grid');
    expect(grid.getAttribute('aria-rowcount')).toBe('100001');
    // jsdom has no layout: the viewport is the default one, a few dozen rows at most.
    expect(screen.getAllByRole('row').length).toBeLessThan(100);
    await fireEvent.click(screen.getByRole('button', { name: /Number/ }));
    expect(screen.getAllByRole('columnheader')[0].getAttribute('aria-sort')).toBe('ascending');
    expect(screen.getAllByRole('gridcell')[0].textContent).toBe('0');
    await fireEvent.click(screen.getByRole('button', { name: /Number/ }));
    expect(screen.getAllByRole('gridcell')[0].textContent).toBe('99999');
    await fireEvent.keyDown(grid, { key: 'ArrowDown' });
    expect(screen.getAllByRole('row')[1].getAttribute('aria-selected')).toBe('true');
  });

  it('puts a theme on the document', () => {
    applyTheme(document, 'system', 3, true);
    expect(document.documentElement.dataset.theme).toBe('dark');
    expect(document.documentElement.style.fontSize).toBe('17px');
    applyTheme(document, 'matrix', 100, false);
    expect(document.documentElement.dataset.theme).toBe('matrix');
    expect(document.documentElement.style.fontSize).toBe('26px');
  });
});

describe('the code of the interface', () => {
  const files: string[] = [];
  const walk = (dir: string) => {
    for (const name of readdirSync(dir)) {
      const path = join(dir, name);
      if (statSync(path).isDirectory()) walk(path);
      else if (/\.(svelte|ts)$/.test(name) && !name.endsWith('.test.ts')) files.push(path);
    }
  };
  walk(join(import.meta.dirname, '..'));

  it('never renders text as HTML, never evaluates text as code', () => {
    expect(files.length).toBeGreaterThan(20);
    for (const file of files) {
      const code = readFileSync(file, 'utf8');
      expect(code, file).not.toMatch(/\{@html/);
      expect(code, file).not.toMatch(/\.innerHTML\s*=|outerHTML\s*=|insertAdjacentHTML/);
      expect(code, file).not.toMatch(/\beval\(|new Function\(/);
    }
  });

  it('loads nothing from elsewhere', () => {
    for (const file of files) {
      const code = readFileSync(file, 'utf8');
      expect(code, file).not.toMatch(/\bfetch\(|XMLHttpRequest|WebSocket\(|https?:\/\/(?!(www\.)?w3\.org)/);
    }
  });
});
