// How fast the UI answers (run_perf.py), with software GL as the demo under WSLg. Two steps:
// window.__E2E_STEP 'setup': a wallet "big" of about 20,000 records (40 transactions of 500 outputs
// to 500 labelled addresses), and a miner; 'measure': the app started again on it, measured: the start
// (first paint, the home drawn with the balance), each page opened (the frame after go()), typing in the
// search of the history, scrolling the history (the frames, as an fps proxy), a new block. Each number is
// a line "PERF <name> <ms>" that the runner reads and checks against its limits.
(async () => {
  const { rpc, sleep, log, T, waitFor, check, finish } = E2E;
  const STEP = window.__E2E_STEP;
  const current = () => (T() && T().walletData ? T().walletData.records : []);
  const $ = (selector, root = document) => root.querySelector(selector);
  const frame = () => new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
  const perf = (name, ms) => log(`PERF ${name} ${Math.round(ms)}`);
  const median = (xs) => [...xs].sort((a, b) => a - b)[Math.floor(xs.length / 2)];
  // Every call of the page to the host, counted (what a page asks for while it is just shown).
  let calls = 0;
  const send = window.__chains;
  window.__chains = (req) => {
    calls++;
    return send(req);
  };
  /** The page hidden (a minimized window) or shown again, as the engine says it. */
  const setHidden = (hidden) => {
    Object.defineProperty(document, 'hidden', { configurable: true, get: () => hidden });
    Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => (hidden ? 'hidden' : 'visible') });
    document.dispatchEvent(new Event('visibilitychange'));
  };
  /** The longest and the median frame while scrolling an element by steps, one per frame. */
  const scrollFrames = async (el, step, count) => {
    const gaps = [];
    let last = performance.now();
    for (let i = 0; i < count; i++) {
      el.scrollTop += step;
      await new Promise((resolve) => requestAnimationFrame(resolve));
      const now = performance.now();
      gaps.push(now - last);
      last = now;
    }
    return { median: median(gaps), max: Math.max(...gaps) };
  };
  try {
    if (STEP === 'setup') {
      await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
      await rpc('createwallet', ['miner']);
      // Loaded at each start of the node (load_on_startup).
      await rpc('createwallet', ['big', false, false, '', false, true, true]);
      const mining = await rpc('getnewaddress', [], 'miner');
      await rpc('generatetoaddress', [160, mining]);
      const addresses = [];
      for (let i = 0; i < 500; i++) addresses.push(await rpc('getnewaddress', [i % 5 === 0 ? `Customer ${i}` : ''], 'big'));
      for (let n = 0; n < 40; n++) {
        const outputs = {};
        for (const a of addresses) outputs[a] = 0.001;
        await rpc('sendmany', ['', outputs], 'miner');
        if (n % 10 === 9) await rpc('generatetoaddress', [1, mining]);
      }
      await rpc('generatetoaddress', [1, mining]);
      const records = await waitFor('the records', async () => {
        const txs = await rpc('listtransactions', ['*', 100000], 'big');
        return txs.length >= 20000 ? txs.length : null;
      }, 120000);
      await check(`a wallet of ${records} records`, records >= 20000, String(records));
      // The wallet shown at the next start: the big one alone (the miner is loaded for the block).
      await rpc('unloadwallet', ['miner', false]);
      T().selectWallet('big');
      await waitFor('the big wallet shown', () => T().wallets.current === 'big' && T().prefs.lastWallet === 'big');
      await sleep(1000);
      await finish();
      return;
    }

    // --- The start: the host started the app at window.__E2E_LAUNCHED (ms since the epoch). ---
    const launched = window.__E2E_LAUNCHED;
    await waitFor('the home with the balance', () => {
      const home = $('.home');
      return T() && T().app.phase === 'ready' && T().wallets.current === 'big' && home && home.textContent.includes('20.00000000') ? home : null;
    }, 180000);
    await frame();
    const interactive = Date.now();
    const paint = performance.getEntriesByType('paint').find((e) => e.name === 'first-contentful-paint' || e.name === 'first-paint');
    if (paint) await perf('start.firstPaint', performance.timeOrigin + paint.startTime - launched);
    await perf('start.homeWithBalance', interactive - launched);
    await waitFor('the history on the home', () => $('.home table tbody tr') && current().length >= 20000, 120000);
    await frame();
    await perf('start.homeWithHistory', Date.now() - launched);
    await check('the big wallet on the home', true);
    await sleep(3000);

    // --- The home scrolled (its cards, shadows and chart over the light of the look). ---
    const content = $('#content');
    const home = await scrollFrames(content, 40, 40);
    await perf('scroll.homeFrameMedian', home.median);
    await perf('scroll.homeFrameMax', home.max);
    content.scrollTop = 0;
    await sleep(500);

    // --- What pages ask for while they are just shown, and while the window is hidden (10 s each). ---
    for (const [page, tab] of [['home', ''], ['mine', ''], ['node', 'peers']]) {
      T().go(page, tab);
      await sleep(1500);
      calls = 0;
      // The runner reads the processor time of the app between the two marks.
      await log(`CPU start idle.cpu10s.${page}`);
      await sleep(10000);
      await log(`CPU end idle.cpu10s.${page}`);
      await perf(`idle.calls10s.${page}`, calls);
      setHidden(true);
      await sleep(500);
      calls = 0;
      await log(`CPU start hidden.cpu10s.${page}`);
      await sleep(10000);
      await log(`CPU end hidden.cpu10s.${page}`);
      await perf(`hidden.calls10s.${page}`, calls);
      setHidden(false);
      await sleep(500);
    }
    T().go('home');
    await sleep(1000);

    // --- Each page opened: from go() to the frame after it is drawn (twice; the second one counted). ---
    const pages = ['overview', 'transactions', 'send', 'receive', 'sidechains', 'explorer', 'mine', 'node', 'settings', 'wallets', 'tools', 'advanced', 'home'];
    const switches = [];
    for (const round of [0, 1]) {
      for (const page of pages) {
        const t0 = performance.now();
        T().go(page);
        await frame();
        const ms = performance.now() - t0;
        if (round === 1) {
          switches.push(ms);
          await perf(`page.${page}`, ms);
        }
        await sleep(page === 'transactions' ? 1500 : 400);
      }
    }
    await perf('page.median', median(switches));
    await perf('page.max', Math.max(...switches));

    // --- A full repaint: another look, from savePrefs to the frame after (the home). ---
    T().go('home');
    await sleep(800);
    const repaint = [];
    for (const theme of ['light', 'midnight', 'light', 'midnight']) {
      const t0 = performance.now();
      T().savePrefs({ theme });
      await frame();
      repaint.push(performance.now() - t0);
      await sleep(500);
    }
    await perf('theme.switchMax', Math.max(...repaint));

    // --- The history: typing in its search, a key at a time; then its rows filtered. ---
    T().go('transactions');
    const grid = await waitFor('the history', () => {
      const g = $('[role="grid"]');
      return g && g.querySelectorAll('[role="row"]').length > 10 ? g : null;
    }, 60000);
    await sleep(1000);
    const search = $('input[type="search"]');
    const keys = [];
    const word = 'Customer';
    const typing = performance.now();
    void typing;
    for (let i = 1; i <= word.length; i++) {
      const t0 = performance.now();
      search.value = word.slice(0, i);
      search.dispatchEvent(new Event('input', { bubbles: true }));
      await frame();
      keys.push(performance.now() - t0);
      await sleep(60);
    }
    await perf('search.keyMax', Math.max(...keys));
    await perf('search.keyMedian', median(keys));
    const count = () => ($('.count') ? $('.count').textContent.split(' of ')[0] : '');
    await waitFor('the rows filtered', () => count() && Number(count().replace(/\D/g, '')) < 20000, 30000);
    await frame();
    await perf('search.filtered', performance.now() - typing);
    search.value = '';
    search.dispatchEvent(new Event('input', { bubbles: true }));
    await waitFor('all the rows again', () => count() && Number(count().replace(/\D/g, '')) >= 20000, 30000);
    await sleep(800);

    // --- Scrolling the history: 60 steps, one per frame; the frames' length. ---
    const history = await scrollFrames(grid, 600, 60);
    await perf('scroll.frameMedian', history.median);
    await perf('scroll.frameMax', history.max);
    await perf('scroll.fps', 1000 / history.median);

    // --- A new block with the history shown: the longest frame while it comes in. ---
    T().go('home');
    await sleep(1500);
    await rpc('loadwallet', ['miner', false]);
    await sleep(1500);
    const mining = await rpc('getnewaddress', [], 'miner');
    let worst = 0;
    let running = true;
    const watch = (async () => {
      let prev = performance.now();
      while (running) {
        await new Promise((resolve) => requestAnimationFrame(resolve));
        const now = performance.now();
        worst = Math.max(worst, now - prev);
        prev = now;
      }
    })();
    const height = T().node.tip.height;
    await rpc('generatetoaddress', [1, mining]);
    await waitFor('the block in the page', () => T().node.tip.height > height, 30000);
    await sleep(2500);
    running = false;
    await watch;
    await perf('block.worstFrame', worst);

    await finish();
  } catch (e) {
    await E2E.snapshot('perf-failure').catch(() => undefined);
    await finish(e instanceof Error ? `${e.message} ${e.stack}` : JSON.stringify(e));
  }
})();
