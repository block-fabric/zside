// Our own dropdowns and scroll bars (doc/webui-layouts.md, "The looks"), in the engine of the app: every
// select drawn has no native face and our chevron; the scroll bars of the page, of a long table and of
// the console are ours (10 px, whatever the default of the engine: overlay bars in GTK, 15–17 px in
// WebView2). Pictures in Midnight and Light: the selects of the settings, the status bar and the
// history, and the lists scrolled.
(async () => {
  const { rpc, sleep, T, waitFor, check, snapshot, finish } = E2E;
  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
  const barWidth = (el) => {
    const cs = getComputedStyle(el);
    return el.offsetWidth - el.clientWidth - parseFloat(cs.borderLeftWidth) - parseFloat(cs.borderRightWidth);
  };
  const ownSelects = (where) => {
    const selects = $$('select').filter((s) => s.offsetParent !== null);
    const bad = selects.filter((s) => {
      const cs = getComputedStyle(s);
      return (cs.appearance || cs.webkitAppearance) !== 'none' || !cs.backgroundImage.includes('data:image/svg+xml');
    });
    return check(`${where}: ${selects.length} selects, all ours`, selects.length > 0 && bad.length === 0, bad.map((s) => s.outerHTML.slice(0, 80)).join(' | ') || 'none drawn');
  };
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    await rpc('createwallet', ['demo']);
    await waitFor('the wallet', () => T().wallets.current === 'demo');
    const mining = await rpc('getnewaddress', ['Mining'], 'demo');
    await rpc('generatetoaddress', [130, mining]);
    for (let i = 0; i < 40; i++) {
      const to = await rpc('getnewaddress', [`Payee ${i + 1}`], 'demo');
      await rpc('sendtoaddress', [to, Number((0.1 + i / 100).toFixed(2))], 'demo');
      // Not too long a chain of unconfirmed transactions.
      if (i % 10 === 9) await rpc('generatetoaddress', [1, mining]);
    }
    await rpc('generatetoaddress', [1, mining]);
    await waitFor('the balance', () => T().wallets.balances.demo && T().wallets.balances.demo.trusted > 0n);

    for (const theme of ['midnight', 'light']) {
      T().savePrefs({ theme, layout: 'dashboard' });
      T().go('settings', 'display');
      await waitFor('the settings', () => $('#content') && $('#content').textContent.includes('Layout'));
      await sleep(400);
      await ownSelects(`settings ${theme}`);
      if ($('#opt-font')) { $('#opt-font').scrollIntoView({ block: 'center' }); $('#opt-font').focus(); }
      await snapshot(`controls-settings-${theme}`);

      T().go('home');
      const content = await waitFor('the home', () => $('.home') && $('#content'));
      await sleep(600);
      content.scrollTop = 400;
      const page = barWidth(content);
      await check(`${theme}: the page's scroll bar is ours (10 px)`, page === 10, String(page));
      await snapshot(`controls-home-scrolled-${theme}`);

      T().go('transactions');
      const grid = await waitFor('the history', () => {
        const g = $('.transactions [role="grid"]') || $('[role="grid"]');
        return g && g.scrollHeight > g.clientHeight + 50 ? g : null;
      }, 30000);
      await ownSelects(`transactions ${theme}`);
      grid.scrollTop = grid.scrollHeight / 3;
      await sleep(400);
      const table = barWidth(grid);
      await check(`${theme}: the scroll bar of a long table is ours (10 px)`, table === 10, String(table));
      await snapshot(`controls-transactions-scrolled-${theme}`);

      T().go('node', 'console');
      const input = await waitFor('the console', () => $('#console-input'));
      for (let i = 0; i < 4; i++) {
        input.value = 'help';
        input.dispatchEvent(new Event('input', { bubbles: true }));
        input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
        await sleep(900);
      }
      const log = await waitFor('a long console', () => $$('#content *').find((el) => el.scrollHeight > el.clientHeight + 100 && /auto|scroll/.test(getComputedStyle(el).overflowY) && el.id !== 'content'), 20000);
      log.scrollTop = log.scrollHeight;
      await sleep(300);
      const consoleBar = barWidth(log);
      await check(`${theme}: the scroll bar of the console is ours (10 px)`, consoleBar === 10, String(consoleBar));
      await ownSelects(`console ${theme}`);
      await snapshot(`controls-console-${theme}`);
    }
    T().savePrefs({ theme: 'midnight', layout: 'dashboard' });
    T().go('home');
    await finish();
  } catch (e) {
    await snapshot('controls-failure').catch(() => undefined);
    await finish(e instanceof Error ? e : JSON.stringify(e));
  }
})();
