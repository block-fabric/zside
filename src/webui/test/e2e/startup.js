// The start of chains-app: the page leaves the splash and shows the app, whatever is loaded at the
// start (no wallet: the node is ready in a moment, before the page has its language; a wallet). The
// page must follow the phase of the host: the "ready" event that comes while the page gets ready is
// not overwritten by the older answer of app.hello (the page stayed on the splash for ever).
// window.__E2E_STEP: 'first' (a new data directory), 'nowallet' (again, no wallet to load),
// 'wallet' (again, a wallet loaded at the start). 'nowallet' makes the wallet the next start loads.
(async () => {
  const { host, sleep, T, waitFor, check, snapshot, finish } = E2E;
  const STEP = window.__E2E_STEP;
  const LAST = window.__E2E_LAST;
  try {
    const started = Date.now();
    // The host says ready; the page must show it within a few seconds.
    await waitFor('the host ready', async () => (await host('app.hello')).phase === 'ready', 120000);
    const page = await waitFor('the page ready', () => T() && T().app.phase === 'ready', 10000).catch(() => false);
    await check(`${STEP}: the page leaves the splash when the host is ready`, !!page, T() ? `page phase ${T().app.phase}` : 'no test hooks');
    await sleep(500);
    const splash = document.querySelector('.phase');
    const shell = document.querySelector('nav[aria-label="Pages"], .sidebar, .workspace');
    await check(`${STEP}: the app is drawn, not the splash`, !splash && !!shell, splash ? splash.textContent.slice(0, 120) : 'no shell');
    // Every layout starts on Home.
    await waitFor('the home', () => T().nav.page === 'home' && document.querySelector('.cards, .home'), 20000).catch(() => null);
    await check(`${STEP}: the ${T().prefs.layout} shows Home`, T().nav.page === 'home', T().nav.page);
    if (STEP === 'wallet') {
      await waitFor('the wallet', () => T().wallets.current === 'w', 30000).catch(() => null);
      await check(`${STEP}: the wallet loaded at the start is shown`, T().wallets.current === 'w', String(T().wallets.current));
    }
    await snapshot(`startup-${STEP}`);
    if (STEP === 'first') {
      // A chain to load at the next start (as a data directory in use has). Not on a sidechain: its blocks are
      // merged mined by the mainchain, which this test does not run (and its addresses are not those of Chains).
      const sidechain = String(await E2E.rpc('help', ['getmainchaininfo'])).startsWith('getmainchaininfo');
      if (!sidechain) await E2E.rpc('generatetoaddress', [150, 'rchn1qyvyz4rr5nhdhfyuek4ef8jcvjqy33vrels9sxr']);
    }
    if (STEP === 'nowallet' && LAST) {
      // For the next start: a wallet loaded with the node.
      await host('wallets.create', { name: 'w' });
      await waitFor('the wallet', () => T().wallets.list.some((x) => x.name === 'w'), 60000);
    }
    await sleep(Math.max(0, 1000 - (Date.now() - started)));
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
