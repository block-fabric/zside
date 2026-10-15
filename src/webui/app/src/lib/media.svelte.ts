// The size of the window, for layouts that change with it (a narrow window: an icon sidebar, menus
// folded into one). Kept in one place, from matchMedia.

function query(q: string): { readonly matches: boolean } {
  const state = $state({ matches: false });
  if (typeof window !== 'undefined' && typeof window.matchMedia === 'function') {
    const m = window.matchMedia(q);
    state.matches = m.matches;
    m.addEventListener?.('change', (e) => (state.matches = e.matches));
  }
  return state;
}

/** Narrower than a sidebar with names and a page side by side: the sidebar shows icons only. */
export const narrowWindow = query('(max-width: 900px)');
