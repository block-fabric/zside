import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import { go, nav } from '../lib/nav.svelte';
import { SUBTABS } from '../lib/subtabs';
import Tools from '../pages/Tools.svelte';

// A tab whose page has tabs of its own: those follow on the same row, after an arrow (user decision).
describe('tabs with sub-tabs', () => {
  it('Tools ▸ Crypto tools: the sub-tabs on the row of the tabs, after an arrow', async () => {
    go('crypto');
    expect([nav.page, nav.tab]).toEqual(['tools', 'crypto']);
    const { container } = render(Tools);
    const rows = container.querySelectorAll('.tab-row');
    expect(rows.length).toBe(1);
    const row = rows[0] as HTMLElement;
    const lists = within(row).getAllByRole('tablist');
    expect(lists.map((l) => l.getAttribute('aria-label'))).toEqual(['Tools', 'Crypto tools']);
    expect(row.querySelector('.arrow')).not.toBeNull();
    // The arrow is between the tabs and the sub-tabs.
    const children = Array.from(row.children);
    expect(children.indexOf(row.querySelector('.arrow')!)).toBe(1);
    // The page inside has no row of its own.
    expect(container.querySelectorAll('[role="tablist"]').length).toBe(2);
    const sub = lists[1];
    expect(within(sub).getByRole('tab', { name: 'Hash Calculator' }).getAttribute('aria-selected')).toBe('true');
    await fireEvent.click(within(sub).getByRole('tab', { name: 'Merkle Tree' }));
    expect(nav.tab).toBe('crypto/merkle');
    expect(within(sub).getByRole('tab', { name: 'Merkle Tree' }).getAttribute('aria-selected')).toBe('true');
  });

  it('a tab without sub-tabs shows no arrow', () => {
    go('timestamp');
    const { container } = render(Tools);
    expect(container.querySelector('.tab-row .arrow')).toBeNull();
    expect(container.querySelectorAll('[role="tablist"]').length).toBe(1);
  });

  it('every page shown inside another has its sub-tabs listed once', () => {
    for (const [page, sub] of Object.entries(SUBTABS)) {
      expect(sub!.tabs.some((tb) => tb.id === sub!.first), page).toBe(true);
      expect(new Set(sub!.tabs.map((tb) => tb.id)).size, page).toBe(sub!.tabs.length);
    }
  });
});
