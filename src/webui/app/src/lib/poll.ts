// Timers of the pages that rest while the window is hidden (minimized, on another desktop): a page
// shown does not ask the node every two seconds for nobody, and catches up at once when shown again.

/** Run fn every ms while the page is visible (and once when it becomes visible again). Returns the stop. */
export function every(ms: number, fn: () => void, options: { now?: boolean } = {}): () => void {
  let timer: ReturnType<typeof setInterval> | null = null;
  const start = () => {
    if (timer === null) timer = setInterval(fn, ms);
  };
  const stop = () => {
    if (timer !== null) clearInterval(timer);
    timer = null;
  };
  const onVisibility = () => {
    if (document.hidden) stop();
    else if (timer === null) {
      fn();
      start();
    }
  };
  if (options.now) fn();
  if (!document.hidden) start();
  document.addEventListener('visibilitychange', onVisibility);
  return () => {
    stop();
    document.removeEventListener('visibilitychange', onVisibility);
  };
}
