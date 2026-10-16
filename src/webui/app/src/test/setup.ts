// Before every test: the app in Advanced mode, as it was before Simple mode came (2026-10-16), since the
// tests of the pages are written for their full form; the tests of Simple mode switch to it themselves.
// The state is imported when the first test starts, not here: a test file's vi.mock of a module the state
// leads to (chain/local) must come first.
import { beforeEach } from 'vitest';

beforeEach(async () => {
  const { prefs } = await import('../lib/state.svelte');
  prefs.uiMode = 'advanced';
});
