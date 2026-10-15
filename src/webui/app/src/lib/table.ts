// Sorting and filtering of the rows of a table (DataTable.svelte), apart from the component so
// that it can be tested on its own.

export type CellValue = string | number | bigint | boolean | null | undefined;

export interface Column<T> {
  key: string;
  label: string;
  /** The value of the cell: what is sorted, filtered, and shown unless the table renders it itself. */
  value: (row: T) => CellValue;
  /** Width as a grid track: "8rem", "minmax(10rem, 1fr)". Default "1fr". */
  width?: string;
  align?: 'start' | 'end' | 'center';
  sortable?: boolean;
  mono?: boolean;
  /** Not searched by the filter. */
  noFilter?: boolean;
  /** The values are words of the app (a type, a state), already in the language shown (made with t()); otherwise they are data, shown as they are (SafeText). */
  words?: boolean;
}

export type SortDir = 'asc' | 'desc';

const collator = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });

export function compareValues(a: CellValue, b: CellValue): number {
  if (a === b) return 0;
  if (a === null || a === undefined) return -1;
  if (b === null || b === undefined) return 1;
  if (typeof a === 'bigint' || typeof b === 'bigint') {
    const x = BigInt(a as bigint | number);
    const y = BigInt(b as bigint | number);
    return x < y ? -1 : x > y ? 1 : 0;
  }
  if (typeof a === 'number' && typeof b === 'number') return a - b;
  if (typeof a === 'boolean' && typeof b === 'boolean') return Number(a) - Number(b);
  return collator.compare(String(a), String(b));
}

/** The rows that match the filter (every word in some column), sorted. A new array; the input stays. */
export function filterAndSort<T>(rows: readonly T[], columns: readonly Column<T>[], filter: string, sortKey: string | null, sortDir: SortDir): T[] {
  const words = filter.trim().toLowerCase().split(/\s+/).filter(Boolean);
  const searched = columns.filter((c) => !c.noFilter);
  let out = words.length
    ? rows.filter((row) => {
        const text = searched.map((c) => String(c.value(row) ?? '')).join('\u0000').toLowerCase();
        return words.every((w) => text.includes(w));
      })
    : rows.slice();
  const column = sortKey ? columns.find((c) => c.key === sortKey) : undefined;
  if (column) {
    const factor = sortDir === 'asc' ? 1 : -1;
    // Values are taken once per row: sorting 100 000 rows stays fast.
    const keyed = out.map((row, i) => ({ row, i, v: column.value(row) }));
    keyed.sort((x, y) => compareValues(x.v, y.v) * factor || x.i - y.i);
    out = keyed.map((k) => k.row);
  }
  return out;
}

/** The rows to draw for a scroll position: first index and count, with some rows above and below. */
export function visibleRange(scrollTop: number, viewport: number, rowHeight: number, total: number, overscan = 8): { start: number; end: number } {
  const first = Math.floor(Math.max(0, scrollTop) / rowHeight);
  const count = Math.ceil(Math.max(0, viewport) / rowHeight) + 1;
  const start = Math.max(0, first - overscan);
  const end = Math.min(total, first + count + overscan);
  return { start, end: Math.max(start, end) };
}
