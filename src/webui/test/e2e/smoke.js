// The main smoke test: chains-app on regtest, driven through its page.
(async () => {
  const { host, rpc, sleep, log, T, waitFor, check, snapshot, text, finish } = E2E;
  const set = (input, value) => {
    input.value = value;
    input.dispatchEvent(new Event('input', { bubbles: true }));
  };
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    await check('info: regtest', T().app.info.network === 'regtest' && T().app.info.test === true);
    // Every layout starts on Home; the checks below are on the overview.
    await waitFor('Home at the start', () => T().nav.page === 'home', 20000);
    await check('the app starts on Home', true);
    T().go('overview');
    await snapshot('overview-nowallet-light');

    // The bridge: RPC calls, errors, cancellation.
    const chain = await rpc('getblockchaininfo');
    await check('rpc getblockchaininfo', chain.chain === 'regtest', JSON.stringify(chain).slice(0, 80));
    const unknown = await rpc('no_such_method').then(() => null, (e) => e);
    await check('rpc error code', unknown && unknown.code === -32601, JSON.stringify(unknown));
    // A page may not call a method outside its policy (rpcpolicy.cpp): the console is the way to any command.
    const outside = await E2E.asPage('dumptxoutset', ['x'], true).then(() => null, (e) => e);
    await check('rpc outside the policy of the pages is refused', outside && outside.code === -32601 && /does not call dumptxoutset/.test(outside.message), JSON.stringify(outside));
    const unknownHost = await host('no.such.method').then(() => null, (e) => e);
    await check('host error code', unknownHost && unknownHost.code === -32601, JSON.stringify(unknownHost));
    const started = Date.now();
    const waiting = window.__chains({ rid: 'e2e-wait', kind: 'testrpc', method: 'waitfornewblock', params: [60000] }).then(() => 'resolved', (e) => e);
    await sleep(300);
    const cancelled = await window.__chains({ kind: 'cancel', rid: 'e2e-wait' });
    const outcome = await waiting;
    await check('cancel a waiting call', cancelled === true && outcome.code === -32800 && Date.now() - started < 5000, JSON.stringify(outcome));

    // A wallet, coins, and what the page shows of them.
    await rpc('createwallet', ['smoke']);
    await waitFor('the wallet in the page', () => T().wallets.list.some((w) => w.name === 'smoke') && T().wallets.current === 'smoke');
    await check('wallet.loaded event', true);
    const address = await rpc('getnewaddress', [], 'smoke');
    await rpc('generatetoaddress', [101, address]);
    await waitFor('the tip event', () => T().node.tip.height >= 101);
    await check('tip event', T().node.tip.height === 101, String(T().node.tip.height));
    await waitFor('the balance event', () => T().wallets.balances.smoke && T().wallets.balances.smoke.trusted === 5000000000n);
    await check('balance event', true);
    // The dashboard (the default layout) opens its home: the balances checked here are the overview's.
    T().go('overview');
    await waitFor('the balance on the overview', () => text('.balances').includes('50.00000000'));
    await check('overview shows the balance', true);
    await check('the chain in the status bar', text('.chain .newest').includes('101'), text('.chain'));

    // Units, privacy mode, preferences kept by the app.
    T().savePrefs({ unit: 'sat' });
    await waitFor('amounts in sat', () => text('.balances').includes('5 000 000 000'));
    await check('unit sat', true);
    T().savePrefs({ unit: 'CHN', mask: true });
    await waitFor('masked amounts', () => !text('.balances').includes('50.0') && text('.balances').includes('••••'));
    await check('mask values', true);
    await snapshot('overview-masked-light');
    T().savePrefs({ mask: false });
    const prefs = await host('prefs.get');
    await check('preferences kept by the app', prefs.unit === 'CHN' && prefs.mask === false, JSON.stringify(prefs));

    // Text from the chain is text: a wallet whose name is markup.
    // (Windows has no < or > in file names: there, entities that must not be decoded.)
    const windows = (await host('app.info')).platform === 'windows';
    const evil = windows ? '&lt;img src=x onerror=alert(1)&gt;w' : '<img src=x onerror=alert(1)>w';
    await rpc('createwallet', [evil]);
    await waitFor('the second wallet', () => T().wallets.list.length === 2);
    const select = await waitFor('the wallet selector', () => document.querySelector('.sidebar .wallet select'));
    await check('markup in a name stays text', select && !select.querySelector('img') && [...select.options].some((o) => o.textContent === evil), select ? select.innerHTML.slice(0, 120) : 'no selector');
    T().selectWallet('smoke');
    await rpc('unloadwallet', [evil]);
    await waitFor('the wallet to go', () => T().wallets.list.length === 1);
    await check('wallet.unloaded event', T().wallets.current === 'smoke');

    // Keyboard shortcuts.
    window.dispatchEvent(new KeyboardEvent('keydown', { key: 'D', ctrlKey: true, shiftKey: true, bubbles: true }));
    await waitFor('the node window', () => T().nav.page === 'node');
    window.dispatchEvent(new KeyboardEvent('keydown', { key: '1', altKey: true, bubbles: true }));
    await waitFor('the overview', () => T().nav.page === 'overview');
    await check('keyboard shortcuts', true);

    // A payment URI from a second start of the app.
    await log(`READY_FOR_URI ${address}`);
    await waitFor('the payment URI', () => T().nav.pendingUri && T().nav.page === 'send', 90000);
    await check('URI handed over by a second start', T().nav.pendingUri.startsWith('chains:' + address), T().nav.pendingUri);

    // The kit: inputs check what is typed.
    T().go('kit');
    await waitFor('the kit', () => document.querySelector('#kit-amount'));
    set(document.querySelector('#kit-amount'), '1.5');
    set(document.querySelector('#kit-address'), address);
    await waitFor('a valid address', () => text('.kit').includes('Valid address'));
    await check('address input checks with the node', true);
    await check('amount input', text('.kit').includes('150000000 sat'), text('.kit').slice(0, 200));
    const rows = document.querySelectorAll('.kit [role="row"]').length;
    await check('100 000 rows, a few drawn', rows > 2 && rows < 120, String(rows));

    // Pictures of every page, light and dark.
    const pages = ['overview', 'send', 'receive', 'transactions', 'sidechains', 'sidechain-nodes', 'mine', 'explorer', 'node', 'settings', 'addresses', 'messages', 'psbt', 'proof', 'multisig', 'timestamp', 'crypto', 'wallets', 'kit', 'home', 'tools', 'advanced'];
    for (const theme of ['light', 'dark']) {
      T().savePrefs({ theme });
      for (const page of pages) {
        T().go(page);
        await snapshot(`${page}-${theme}`);
      }
      T().go('overview');
      // The menus of chains-qt: in the sidebar's Menus, and the command palette.
      document.querySelector('.sidebar button[aria-label^="Menus of chains-qt"]').click();
      await waitFor('the menus', () => document.querySelector('[role="menu"]'));
      await snapshot(`menu-${theme}`);
      document.body.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
      window.dispatchEvent(new KeyboardEvent('keydown', { key: 'k', ctrlKey: true, bubbles: true }));
      await waitFor('the palette', () => document.querySelector('dialog.palette[open]'));
      await snapshot(`palette-${theme}`);
      document.querySelector('dialog.palette').dispatchEvent(new Event('cancel', { cancelable: true }));
      T().nav.dialog = 'about';
      await snapshot(`about-${theme}`);
      T().nav.dialog = 'help';
      await snapshot(`help-${theme}`);
      T().nav.dialog = null;
      T().app.dialogs.push({ kind: 'message', level: 'error', title: 'Error', message: 'An error of the node, as the node shows it.' });
      await snapshot(`node-dialog-${theme}`);
      T().app.dialogs.shift();
    }
    for (const theme of ['ocean', 'matrix', 'qdarkstyle']) {
      T().savePrefs({ theme });
      for (const page of ['overview', 'settings', 'kit']) {
        T().go(page);
        await snapshot(`${page}-${theme}`);
      }
    }
    T().savePrefs({ theme: 'light', fontAdjust: 0 });
    T().go('overview');
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
