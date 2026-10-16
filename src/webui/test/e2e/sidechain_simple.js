// Simple mode on a sidechain (run_sidechain.py --script sidechain_simple.js --js-var SIMPLE=true): the
// default of a first start. A wallet, then in the Midnight and Light themes: Home with its first steps and
// merged mining in a line, the sidebar without the pages of Advanced, Drivechains in two tabs (a deposit
// address to bring coins in, the form that sends them back), Send, the history, and every page of the
// sidebar (the chain's own among them), with a picture of each; then the switch to Advanced and back.
(async () => {
  const { rpc, host, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  const SLOT = window.__E2E_SLOT;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  const stem = (window.__E2E_STEM || 'sc');
  async function shot(name) {
    // Without the sync overlay a new sidechain shows until its first blocks.
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = button('Hide', overlay);
      if (hide) hide.click();
    }
    await sleep(500);
    await snapshot(`${stem}-tour-${name.replace(/^simple-/, '')}`);
  }
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await check('Simple mode is the default of a first start', T().prefs.uiMode === 'simple', T().prefs.uiMode);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    await rpc('createwallet', ['side']);
    await waitFor('the wallet in the page', () => T().wallets.current === 'side');
    T().savePrefs({ layout: 'dashboard', tipsDone: false });
    // Blocks of this chain, merged mined with every block of the mainchain (as a miner of the mainchain would),
    // so that the pictures show a chain in sync.
    const fees = await rpc('getnewaddress', [], 'side');
    await host('test.confirmAnswer', { yes: true });
    await host('bmm.set', { mine: true, address: fees, always: true, amount: '0.0001' });
    await waitFor('the first blocks of this chain', () => T().node.tip.height >= 2 && !T().node.tip.ibd, 120000);

    for (const theme of ['midnight', 'light']) {
      T().savePrefs({ theme });
      T().go('home');
      await waitFor('the first steps', () => $('.tips'), 20000);
      await check(`${theme}: three first steps on Home`, $$('.tips li').length === 3);
      await waitFor('merged mining in a line', () => $('[data-testid="bmm-line"]'), 20000);
      const sidebar = text('nav.sidebar .scroll');
      await check(`${theme}: the sidebar without the pages of Advanced`, !sidebar.includes('Advanced transactions') && !sidebar.includes('Node window') && sidebar.includes('Drivechains'), sidebar);
      await check(`${theme}: the switch in the sidebar's foot`, !!$('nav.sidebar .foot [role="radiogroup"]'));
      await shot(`simple-home-${theme}`);

      T().go('peg', 'deposit');
      const tabs = await waitFor('the two tabs of Drivechains', () => {
        const t = $$('.tabs [role="tab"]').map((x) => x.textContent.trim().replace(/\s*\d+$/, ''));
        return t.length ? t : null;
      });
      await check(`${theme}: Drivechains has two tabs in plain words`, JSON.stringify(tabs) === JSON.stringify(['Bring coins in', 'Send coins back']), JSON.stringify(tabs));
      if (theme === 'midnight') {
        const b = await waitFor('New deposit address', () => button('New deposit address'));
        b.click();
        await waitFor('the deposit address', () => text('#deposit-address').trim().startsWith(`s${SLOT}_`));
      }
      await shot(`simple-peg-in-${theme}`);
      T().go('peg', 'withdraw');
      await waitFor('the form that sends coins back', () => $('#withdraw-address'));
      await shot(`simple-peg-back-${theme}`);

      T().go('send');
      await waitFor('the send page', () => $('#content .page'));
      await shot(`simple-send-${theme}`);

      // Every other page of the sidebar: the kit's and the chain's own, as Simple mode shows them.
      const seen = new Set(['Home', 'Drivechains', 'Send', 'Settings']);
      const entries = () => $$('nav.sidebar .scroll .entry').filter((e) => !e.disabled);
      for (const name of entries().map((e) => e.textContent.trim())) {
        if (seen.has(name)) continue;
        seen.add(name);
        const entry = entries().find((e) => e.textContent.trim() === name);
        if (!entry) continue;
        entry.click();
        await sleep(1200);
        await shot(`simple-${name.toLowerCase().replace(/[^a-z0-9]+/g, '-')}-${theme}`);
      }
    }

    // The switch: Advanced by the sidebar, Simple again by the palette.
    $$('nav.sidebar [role="radio"]').find((b) => b.textContent.trim() === 'Advanced').click();
    await waitFor('Advanced mode', () => T().prefs.uiMode === 'advanced');
    T().go('peg', 'deposit');
    await waitFor('the four tabs of Drivechains', () => $$('.tabs [role="tab"]').length === 4);
    await check('Advanced: the bundle and merged mining are back', !!button('Bundle') && text('nav.sidebar .scroll').includes('Advanced transactions'));
    const saved = await host('prefs.get');
    await check('the mode is saved in the preferences', saved.uiMode === 'advanced', JSON.stringify(saved.uiMode));
    T().buildCommands().find((c) => c.id === 'display.uiMode').run();
    await waitFor('Simple mode again', () => T().prefs.uiMode === 'simple');
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
