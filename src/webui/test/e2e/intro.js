// The first start: the intro, then the start of the node.
(async () => {
  const { sleep, T, waitFor, check, snapshot, text, buttonByText, finish } = E2E;
  try {
    await waitFor('the intro', () => T() && T().app.phase === 'intro');
    await waitFor('the free space', () => text('.intro').includes('GB of free space'));
    await snapshot('intro-light');
    T().savePrefs({ theme: 'dark' });
    await snapshot('intro-dark');
    T().savePrefs({ theme: 'light' });
    const dir = window.__E2E_DATADIR;
    const radios = document.querySelectorAll('.intro input[type="radio"]');
    radios[1].click();
    const input = document.querySelector('.intro input[aria-label="Custom data directory"]');
    input.value = dir;
    input.dispatchEvent(new Event('input', { bubbles: true }));
    await sleep(100);
    buttonByText('Start').click();
    await waitFor('the start of the node', () => T().app.phase === 'init' || T().app.phase === 'ready');
    if (T().app.phase === 'init') await snapshot('starting-light');
    await waitFor('the node to run', () => T().app.phase === 'ready', 120000);
    const info = await E2E.host('app.info');
    await check('data directory chosen in the intro', info.datadir.startsWith(dir), info.datadir);
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
