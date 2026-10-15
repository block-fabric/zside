// The integrated title bar (doc/webui-layouts.md), driven through the page: in each layout and in
// the light and dark themes (and the other looks on the dashboard), the title bar at the top with
// the buttons of the window, and pictures of it; then the title bar of the system, chosen in the
// settings, and back.
(async () => {
  const { host, rpc, sleep, T, waitFor, check, snapshot, finish } = E2E;
  const $ = (selector, root = document) => root.querySelector(selector);
  const controls = () => $('[role="group"][aria-label="Window"]');
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    await rpc('createwallet', ['demo']);
    await waitFor('the wallet', () => T().wallets.current === 'demo');

    const state = await host('window.state');
    await check('the integrated title bar by default', state.titleBar === 'integrated' && state.frameless === true && state.supported === true, JSON.stringify(state));

    for (const layout of ['workspace', 'dashboard']) {
      for (const theme of ['light', 'dark']) {
        T().savePrefs({ theme, layout });
        T().go(layout === 'dashboard' ? 'home' : 'send');
        await sleep(300);
        const group = await waitFor('the buttons of the window', controls);
        const bar = group.closest('header');
        await check(`${layout} ${theme}: the title bar at the top`, bar && bar.getBoundingClientRect().top === 0, bar ? String(bar.getBoundingClientRect().top) : 'none');
        if (layout !== 'workspace') {
          const h = bar.getBoundingClientRect().height;
          const parts = [...bar.querySelectorAll('*')].map((el) => `${el.tagName}.${el.className && el.className.baseVal === undefined ? el.className : ''}:${Math.round(el.getBoundingClientRect().height)}`);
          await check(`${layout} ${theme}: a slim strip`, h < 40, `${h} ${parts.join(' ')}`);
        }
        const labels = [...group.querySelectorAll('button')].map((b) => b.getAttribute('aria-label'));
        await check(`${layout} ${theme}: minimize, maximize, close`, labels.join() === 'Minimize,Maximize,Close', labels.join());
        await snapshot(`titlebar-${layout}-${theme}`);
      }
    }
    for (const theme of ['ocean', 'matrix', 'qdarkstyle', 'system']) {
      T().savePrefs({ theme, layout: 'dashboard' });
      T().go('home');
      await sleep(300);
      await snapshot(`titlebar-dashboard-${theme}`);
    }

    // The close button under the pointer: red.
    T().savePrefs({ theme: 'light', layout: 'workspace' });
    await sleep(300);
    const close = controls().querySelector('button[aria-label="Close"]');
    close.focus();
    await sleep(200);
    const red = getComputedStyle(close).backgroundColor;
    await check('the close button is red when focused or under the pointer', red === 'rgb(196, 43, 28)', red);
    await snapshot('titlebar-workspace-close-focused-light');
    close.blur();

    // The title bar of the system, chosen in the settings: the page's goes (at once on X11), and back.
    T().savePrefs({ layout: 'dashboard' });
    T().go('settings', 'display');
    const box = await waitFor('the title bar setting', () => [...document.querySelectorAll('label.check')].find((l) => l.textContent.includes('title bar of the desktop')));
    const applyButton = () => [...document.querySelectorAll('.apply-bar button')].find((b) => b.textContent.trim() === 'Apply');
    box.querySelector('input').click();
    // The choice waits for Apply, as the other settings: nothing changes before.
    await new Promise((r) => setTimeout(r, 300));
    const before = await host('window.state');
    await check('the title bar option waits for Apply', before.titleBar === 'integrated' && !!controls() && !applyButton().disabled, JSON.stringify(before));
    applyButton().click();
    const changed = await waitFor('the title bar of the system', async () => {
      const s = await host('window.state');
      return s.titleBar === 'system' && s;
    });
    if (changed.restartNeeded) {
      await waitFor('the notice of a restart', () => box.nextElementSibling.textContent.includes('restart'));
      await check('the title bar of the system, after a restart', true);
    } else {
      await waitFor('no title bar of the page', () => !controls());
      await check('the title bar of the system, at once', !$('.titlestrip') && !$('.resize'));
    }
    await snapshot('titlebar-system-light');
    box.querySelector('input').click();
    await waitFor('Apply, for the way back', () => !applyButton().disabled, 5000);
    applyButton().click();
    const back = await waitFor('the integrated title bar again', async () => {
      const s = await host('window.state');
      return s.titleBar === 'integrated' && s;
    });
    await check('the integrated title bar again', back.frameless === true && !back.restartNeeded, JSON.stringify(back));
    await waitFor('the buttons of the window again', controls);
    await check('a resize edge is refused when unknown', await host('window.startResize', { edge: 'north' }).then(() => false, () => true));
    T().savePrefs({ theme: 'light', layout: 'dashboard' });
    await finish();
  } catch (e) {
    await snapshot('titlebar-failure').catch(() => undefined);
    await finish(e);
  }
})();
