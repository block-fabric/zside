import { describe, expect, it } from 'vitest';
import { compareValues, filterAndSort, visibleRange, type Column } from './table';

interface Row {
  id: number;
  name: string;
  amount: bigint;
}
const columns: Column<Row>[] = [
  { key: 'id', label: 'Id', value: (r) => r.id },
  { key: 'name', label: 'Name', value: (r) => r.name },
  { key: 'amount', label: 'Amount', value: (r) => r.amount, noFilter: true },
];
const rows: Row[] = [
  { id: 3, name: 'coffee 10', amount: -5n },
  { id: 1, name: 'Rent', amount: 100n },
  { id: 2, name: 'coffee 9', amount: 7n },
];

describe('table', () => {
  it('compares numbers, bigints, text naturally', () => {
    expect(compareValues(2, 10)).toBeLessThan(0);
    expect(compareValues(10n, 9n)).toBeGreaterThan(0);
    expect(compareValues('item 9', 'item 10')).toBeLessThan(0);
    expect(compareValues(null, 'a')).toBeLessThan(0);
  });
  it('filters by every word, case blind, and sorts stably', () => {
    expect(filterAndSort(rows, columns, 'COFFEE', 'name', 'asc').map((r) => r.id)).toEqual([2, 3]);
    expect(filterAndSort(rows, columns, 'coffee 10', null, 'asc').map((r) => r.id)).toEqual([3]);
    expect(filterAndSort(rows, columns, '100', null, 'asc')).toEqual([]);
    expect(filterAndSort(rows, columns, '', 'amount', 'desc').map((r) => r.id)).toEqual([1, 2, 3]);
    expect(rows.map((r) => r.id)).toEqual([3, 1, 2]);
  });
  it('sorts 100 000 rows quickly', () => {
    const many = Array.from({ length: 100_000 }, (_, i) => ({ id: i, name: `n${(i * 7919) % 100_000}`, amount: BigInt(i % 1000) }));
    const started = performance.now();
    const sorted = filterAndSort(many, columns, '', 'name', 'asc');
    expect(sorted).toHaveLength(100_000);
    expect(performance.now() - started).toBeLessThan(3000);
  });
  it('draws only the rows in view', () => {
    expect(visibleRange(0, 340, 34, 100_000)).toEqual({ start: 0, end: 19 });
    expect(visibleRange(34_000, 340, 34, 100_000)).toEqual({ start: 992, end: 1019 });
    expect(visibleRange(3_399_000, 340, 34, 100_000)).toEqual({ start: 99_962, end: 99_989 });
    expect(visibleRange(3_399_800, 340, 34, 100_000)).toEqual({ start: 99_986, end: 100_000 });
    expect(visibleRange(0, 340, 34, 0)).toEqual({ start: 0, end: 0 });
  });
});
