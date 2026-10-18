// "Confirm before quit": a page that keeps work a quit would interrupt (Hivemind: a market still being created) says so
// to the host, which then asks in its own dialog before the app quits (the window closed, Quit, Ctrl+Q): "Keep … open"
// the default, "Quit anyway". The host forgets what was said after a minute (a page that hangs holds nothing), so the
// text is said again while it holds; a second quit while the question shows quits, as does the end of the session.
import { host } from './rpc';

/** How often the text is said again while it holds (the host forgets it after 60 s). */
export const QUIT_GUARD_RENEW_MS = 20_000;

let current: string | null = null;
let timer: ReturnType<typeof setInterval> | null = null;

function tell(message: string | null): void {
  void host.quitGuard(message).catch(() => undefined);
}

/**
 * The text the host shows before a quit, or null for none. Said to the host when it changes, then again every
 * QUIT_GUARD_RENEW_MS while it holds.
 */
export function setQuitGuard(message: string | null): void {
  const next = message && message.trim() ? message : null;
  if (next === current) return;
  current = next;
  if (timer) {
    clearInterval(timer);
    timer = null;
  }
  tell(next);
  if (next) timer = setInterval(() => tell(current), QUIT_GUARD_RENEW_MS);
}

/** What the host is told now (the tests). */
export function quitGuard(): string | null {
  return current;
}
