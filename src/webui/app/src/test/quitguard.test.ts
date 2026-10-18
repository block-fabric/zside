// "Confirm before quit" (lib/quitguard.ts): the page says to the host what a quit would interrupt, again while it holds
// (the host forgets it after a minute), and clears it when the work is done.
import { afterEach, describe, expect, it, vi } from 'vitest';

const calls: unknown[] = [];
vi.mock('../lib/rpc', () => ({
  host: {
    quitGuard: (message: string | null) => {
      calls.push(message);
      return Promise.resolve(true);
    },
  },
}));

describe('the quit guard of a page', () => {
  afterEach(() => vi.useRealTimers());

  it('says a text once when it changes, again while it holds, and null when cleared', async () => {
    vi.useFakeTimers();
    const { setQuitGuard, quitGuard, QUIT_GUARD_RENEW_MS } = await import('../lib/quitguard');
    setQuitGuard('A market is still being created (Rain).');
    setQuitGuard('A market is still being created (Rain).');
    expect(calls).toEqual(['A market is still being created (Rain).']);
    expect(quitGuard()).toBe('A market is still being created (Rain).');
    // Renewed well within the minute the host keeps it.
    expect(QUIT_GUARD_RENEW_MS).toBeLessThan(60_000);
    vi.advanceTimersByTime(QUIT_GUARD_RENEW_MS * 2);
    expect(calls.length).toBe(3);
    setQuitGuard(null);
    expect(calls.at(-1)).toBeNull();
    vi.advanceTimersByTime(QUIT_GUARD_RENEW_MS * 3);
    expect(calls.length).toBe(4);
    // Blank text is none.
    setQuitGuard('   ');
    expect(calls.length).toBe(4);
    expect(quitGuard()).toBeNull();
  });

  it('every way to quit goes through the host, which asks (app.quit, not a stop of the page)', async () => {
    const { readFileSync } = await import('node:fs');
    const { join } = await import('node:path');
    const app = readFileSync(join(__dirname, '..', '..', '..', 'app.cpp'), 'utf8');
    // The window closed, Quit of the tray, app.quit (the menu, Ctrl+Q): AskQuit; the end of the session stops at once.
    expect(app).toMatch(/RegisterHost\("app\.quit"[^}]*QuitRequested\(\)/);
    expect(app).toMatch(/if \(id == "quit"\) \{\s*AskQuit\(\);/);
    expect(app).toMatch(/callbacks\.session_ending = \[this\] \{ RequestShutdown\(\); \}/);
    expect(app).toMatch(/\.cancel = strprintf\("Keep %s open", CLIENT_NAME\)/);
    expect(app).toMatch(/\.accept = "Quit anyway"/);
  });
});
