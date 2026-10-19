// Pictures of the changes of the user feedback of 2026-10-19 (run_sidechain.py --script feedback_shots.js, or run_smoke.py
// for Chains), in Midnight: the sidebar expanded and folded (Advanced and Simple), its scrollbar at its edge (large
// text, so that it scrolls), the names told apart (Manage wallets), Simple mode's navigation, the workspace folded.
// Pictures only (and a few checks a build without the changes fails: the "before" pictures are taken with the same
// script on the old build, with --js-var BEFORE=true).
(async () => {
  const { rpc, sleep, T, waitFor, check, snapshot, finish } = E2E;
  const $ = (sel, root = document) => root.querySelector(sel);
  const stem = window.__E2E_STEM || 'app';
  const before = window.__E2E_BEFORE === true;
  async function shot(name) {
    const overlay = [...document.querySelectorAll('dialog[open]')].find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = [...overlay.querySelectorAll('button')].find((b) => b.textContent.trim() === 'Hide');
      if (hide) hide.click();
    }
    await sleep(700);
    await snapshot(`${stem}-${before ? 'before' : 'after'}-${name}`);
  }
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await rpc('createwallet', ['demo']);
    await waitFor('the wallet', () => T().wallets.current === 'demo');
    T().savePrefs({ theme: 'midnight', layout: 'dashboard', uiMode: 'advanced', sidebarCollapsed: false, tipsDone: true, fontAdjust: 0 });
    T().go('home');
    await shot('advanced-expanded');
    // Large text: the sidebar scrolls; its bar at its right edge.
    T().savePrefs({ fontAdjust: 4 });
    const scroller = await waitFor('the list of the sidebar', () => $('nav.sidebar .scroll'));
    await sleep(500);
    const nav = $('nav.sidebar');
    const gap = Math.round(nav.getBoundingClientRect().right - scroller.getBoundingClientRect().right);
    if (!before) await check('the scrollbar of the sidebar at its right edge', scroller.scrollHeight > scroller.clientHeight && gap <= 1, `gap ${gap} px, ${scroller.scrollHeight}/${scroller.clientHeight}`);
    await shot('sidebar-scroll');
    T().savePrefs({ fontAdjust: 0 });
    T().go('wallets');
    await shot('manage-wallets');
    T().go('home');
    T().savePrefs({ sidebarCollapsed: true });
    await sleep(400);
    if (!before) await check('the sidebar folds into icons', nav.classList.contains('collapsed') && nav.getBoundingClientRect().width < 80, `${nav.getBoundingClientRect().width}`);
    await shot('advanced-collapsed');
    T().savePrefs({ uiMode: 'simple', sidebarCollapsed: false });
    await shot('simple-expanded');
    T().savePrefs({ sidebarCollapsed: true });
    await shot('simple-collapsed');
    T().savePrefs({ uiMode: 'advanced', sidebarCollapsed: false, layout: 'workspace' });
    await shot('workspace');
    T().savePrefs({ sidebarCollapsed: true });
    await shot('workspace-folded');
    T().savePrefs({ sidebarCollapsed: false, layout: 'dashboard', theme: 'light' });
    T().go('home');
    await shot('advanced-light');
    T().savePrefs({ theme: 'midnight' });
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
