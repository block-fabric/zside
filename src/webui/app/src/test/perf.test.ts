import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { every } from '../lib/poll';
import { balanceHistory, newest, sortNewestFirst } from '../lib/wallet';

// What keeps the UI fast on a long history and a hidden window (doc/webui-layouts.md, "Performance").

const record = (time: number, key: string) => ({ time, sortKey: key });

describe('the newest records without sorting the history', () => {
  it('are those of the full sort, in its order', () => {
    const records = Array.from({ length: 3000 }, (_, i) => record(1000 + ((i * 7919) % 50), `${String((i * 104729) % 9973).padStart(10, '0')}-0-${i}`));
    for (const count of [1, 8, 25]) expect(newest(records, count)).toEqual(sortNewestFirst(records).slice(0, count));
    expect(newest(records.slice(0, 3), 8)).toEqual(sortNewestFirst(records.slice(0, 3)));
    expect(newest([], 8)).toEqual([]);
  });

  it('the sort key compared as it is: the order of the locale for the keys of the host', () => {
    const keys = ['0000000120-0-0001700000-002-1', '0000000120-0-0001700000-010-1', '0000000099-1-0001699999-000-0', '-000000001-0-0001700001-000-1'];
    const byLocale = [...keys].sort((a, b) => b.localeCompare(a));
    expect(sortNewestFirst(keys.map((k) => record(5, k))).map((r) => r.sortKey)).toEqual(byLocale);
  });

  it('the balance over time is unchanged', () => {
    const rows = [
      { time: 2, sortKey: 'b', amount: 5n, status: 'confirmed' },
      { time: 1, sortKey: 'a', amount: 3n, status: 'confirmed' },
      { time: 2, sortKey: 'a', amount: -1n, status: 'abandoned' },
    ];
    expect(balanceHistory(rows as never)).toEqual([
      { time: 1, balance: 3n },
      { time: 2, balance: 8n },
    ]);
  });
});

describe('timers of the pages', () => {
  const setHidden = (hidden: boolean) => {
    Object.defineProperty(document, 'hidden', { configurable: true, get: () => hidden });
    document.dispatchEvent(new Event('visibilitychange'));
  };
  afterEach(() => {
    setHidden(false);
    vi.useRealTimers();
  });

  it('rest while the window is hidden, and catch up when it is shown', () => {
    vi.useFakeTimers();
    const fn = vi.fn();
    const stop = every(1000, fn, { now: true });
    expect(fn).toHaveBeenCalledTimes(1);
    vi.advanceTimersByTime(3000);
    expect(fn).toHaveBeenCalledTimes(4);
    setHidden(true);
    vi.advanceTimersByTime(10000);
    expect(fn).toHaveBeenCalledTimes(4);
    setHidden(false);
    expect(fn).toHaveBeenCalledTimes(5);
    vi.advanceTimersByTime(1000);
    expect(fn).toHaveBeenCalledTimes(6);
    stop();
    vi.advanceTimersByTime(5000);
    setHidden(true);
    setHidden(false);
    expect(fn).toHaveBeenCalledTimes(6);
  });
});

describe('the styles stay cheap to paint (software GL, as under WSLg)', () => {
  const files: { file: string; css: string }[] = [];
  const walk = (dir: string) => {
    for (const name of readdirSync(dir)) {
      const path = join(dir, name);
      if (statSync(path).isDirectory()) {
        if (name !== 'test') walk(path);
      } else if (name.endsWith('.css') || name.endsWith('.svelte')) files.push({ file: path, css: readFileSync(path, 'utf8') });
    }
  };
  walk(resolve(__dirname, '..'));

  it('no shadow, filter or blur animated; no backdrop blur; no fixed background', () => {
    for (const { file, css } of files) {
      for (const m of css.matchAll(/transition:([^;]*);/g)) expect(m[1], file).not.toMatch(/box-shadow|filter|all\b/);
      expect(css, file).not.toMatch(/backdrop-filter/);
      expect(css, file).not.toMatch(/background-attachment:\s*fixed/);
    }
  });

  it('the ambient light is a layer of its own, painted once', () => {
    const base = readFileSync(resolve(__dirname, '../styles/base.css'), 'utf8');
    const layer = /body::before \{([^}]*)\}/.exec(base)?.[1] ?? '';
    expect(layer).toMatch(/position:\s*fixed/);
    expect(layer).toMatch(/var\(--ambient\)/);
    expect(layer).toMatch(/will-change:\s*transform/);
  });
});
