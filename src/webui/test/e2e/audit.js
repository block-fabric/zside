// The fixes of the audit of chains-app (doc/webui-audit.md), checked in the app itself: the sync
// overlay, chain text that is not UTF-8, long calls, a long history, keys unlocked for one call, the
// contrast of every theme, aligned tables, settings that wait for Apply, the frame, the language.
// Run by run_audit.py, which also submits a block by hand and starts the app again.
(async () => {
  const { host, rpc, sleep, log, T, waitFor, check, snapshot, text, buttonByText, finish } = E2E;
  const $ = (s, root = document) => root.querySelector(s);
  const $$ = (s, root = document) => [...root.querySelectorAll(s)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const go = async (page, tab = '') => {
    T().go(page, tab);
    await sleep(250);
  };
  const PASS = 'audit pass phrase';

  // Every call of the page and its answer, as the host sees them (sizes, methods).
  const calls = [];
  const send = window.__chains;
  window.__chains = (req) => {
    const entry = { method: req.method, kind: req.kind, params: req.params, size: 0, t: Date.now() };
    calls.push(entry);
    const p = send(req);
    p.then((r) => (entry.size = JSON.stringify(r ?? null).length), () => undefined);
    return p;
  };

  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);

    // L3: a fresh regtest chain is far behind: the sync overlay opens by itself (a layer, not modal).
    await waitFor('the sync overlay', () => T().nav.dialog === 'sync', 10000);
    await check('L3 sync overlay opens by itself while behind', text('dialog[open]').includes('Number of blocks left') && text('dialog[open]').includes('Attempting to spend coins'));
    await snapshot('audit-sync-overlay-light');

    // C16: the window is framed by the page, its buttons in the title bar.
    const state = await host('window.state');
    await check('C16 the page frames the window', state.frameless === true && !!$('.titlestrip, .top.titlebar') && !!$('button[aria-label="Close"]') && !!$('button[aria-label="Minimize"]'), JSON.stringify(state));

    // H1: a block with a sidechain proposal whose title is not UTF-8 (submitted by the runner).
    await log('READY_FOR_BLOCK');
    await waitFor('the block of the runner', () => T().node.tip.height >= 1, 60000);
    const proposals = await Promise.race([rpc('listsidechainproposals'), sleep(5000).then(() => 'timeout')]);
    const pending = proposals && proposals.pending ? proposals.pending : [];
    await check('H1 an answer with chain text that is not UTF-8 reaches the page', pending.some((p) => p.title === 'Bad �� title'), JSON.stringify(proposals).slice(0, 200));
    await go('sidechains', 'proposals');
    await waitFor('the proposal in the page', () => text('.page').includes('Bad �� title'), 20000);
    await check('H1 the Sidechains page shows it', !text('.page').includes('not available yet'));

    // A wallet with a history, and the chain up to date.
    await rpc('createwallet', ['big']);
    await waitFor('the wallet', () => T().wallets.current === 'big');
    const bigAddress = await rpc('getnewaddress', [], 'big');
    await rpc('generatetoaddress', [400, bigAddress]);
    await waitFor('the tip', () => T().node.tip.height >= 401, 60000);
    await waitFor('the overlay to go', () => T().nav.dialog !== 'sync', 20000);
    await check('L3 the overlay goes once the node caught up', T().nav.dialog !== 'sync');

    // M1: calls that wait for long do not hold up the quick ones.
    const waits = [];
    for (let i = 0; i < 8; i++) waits.push(window.__chains({ rid: `wait-${i}`, kind: 'testrpc', method: 'waitfornewblock', params: [30000] }).catch((e) => e));
    await sleep(500);
    const t0 = Date.now();
    await rpc('getblockcount');
    await host('wallet.info', { wallet: 'big' });
    const quick = Date.now() - t0;
    await check('M1 quick calls answer while eight long ones wait', quick < 1500, `${quick} ms`);
    for (let i = 0; i < 8; i++) await window.__chains({ kind: 'cancel', rid: `wait-${i}` });
    await Promise.all(waits);

    // H2: the history is loaded once; a new block and a new transaction ask for little.
    await go('transactions');
    await waitFor('the history', () => T().walletData.records.length >= 400 && T().walletData.recordsLoaded, 60000);
    const full = calls.filter((c) => c.method === 'wallet.transactions' && !(c.params && (c.params.txids || c.params.latest)));
    await check('H2 the history loaded whole once', full.length === 1, String(full.length));
    const before = calls.length;
    await rpc('generatetoaddress', [1, bigAddress]);
    await waitFor('the new record', () => T().walletData.records.length >= 401, 30000);
    await sleep(800);
    const after = calls.slice(before).filter((c) => c.method === 'wallet.transactions');
    const asked = after.map((c) => (c.params && c.params.txids ? c.params.txids.length : 'all'));
    await check('H2 a new block asks for the records that changed only', after.length >= 1 && after.every((c) => c.params && (c.params.txids || c.params.latest)), JSON.stringify(asked));
    await check('H2 and the answers are small', after.every((c) => c.size < 20000), JSON.stringify(after.map((c) => c.size)));
    const depthOf = (r) => r && r.depth;
    const oldest = T().walletData.records.find((r) => r.blockHeight === 2);
    await check('H2 the status of the others follows the height', depthOf(oldest) === T().node.tip.height - 2 + 1, `${depthOf(oldest)} at ${T().node.tip.height}`);
    // L2: the headers line up with their columns.
    const head = (label) => $$('[role="columnheader"]').find((h) => h.textContent.includes(label));
    const firstRow = $$('[role="grid"] [role="row"]')[1];
    const cells = firstRow ? $$('[role="gridcell"]', firstRow) : [];
    const dateHead = head('Date');
    const amountHead = head('Amount');
    const dateText = cells[1] && cells[1].querySelector('.cellwrap');
    const amountText = cells[cells.length - 1] && cells[cells.length - 1].querySelector('.amt');
    const leftGap = dateHead && dateText ? Math.abs(dateHead.querySelector('button').getBoundingClientRect().left - dateText.getBoundingClientRect().left) : 99;
    const rightGap = amountHead && amountText ? Math.abs(amountHead.querySelector('button').getBoundingClientRect().right - amountText.getBoundingClientRect().right) : 99;
    await check('L2 the headers line up with their columns', leftGap <= 2 && rightGap <= 2, `date ${leftGap.toFixed(1)} px, amount ${rightGap.toFixed(1)} px`);
    await snapshot('audit-transactions-light');
    // The overview: the newest records and the chart, never the whole history.
    const beforeOverview = calls.length;
    await go('overview');
    await waitFor('recent transactions', () => $$('.recent .tx').length >= 5, 20000);
    const overviewCalls = calls.slice(beforeOverview).filter((c) => c.method === 'wallet.transactions');
    await check('H2 the overview asks for the newest records only', overviewCalls.length >= 1 && overviewCalls.every((c) => c.params && c.params.latest), JSON.stringify(overviewCalls.map((c) => c.params)));

    // M2: keys unlocked for the call alone, in the host.
    await rpc('createwallet', ['enc', false, false, PASS]);
    await waitFor('the wallet', () => T().wallets.list.some((w) => w.name === 'enc'));
    T().selectWallet('enc');
    const encAddress = await rpc('getnewaddress', ['', 'bech32'], 'enc').catch(async () => null);
    await rpc('sendtoaddress', [encAddress, 2], 'big');
    await rpc('generatetoaddress', [1, bigAddress]);
    await waitFor('the coins of enc', () => T().wallets.balances.enc && T().wallets.balances.enc.trusted > 0n, 30000);
    await go('proof', 'prove');
    $('#proof-statement').closest('form').requestSubmit();
    const dialog = await waitFor('the passphrase', () => $$('dialog[open]').find((d) => d.textContent.includes('Unlock wallet')), 20000);
    set($('#pass1', dialog), PASS);
    await sleep(50);
    $('#passphrase-form').requestSubmit();
    await waitFor('the proof', () => $('#proof-output') && $('#proof-output').value.includes('-----BEGIN'), 30000).catch(() => null);
    const proofText = $('#proof-output') ? $('#proof-output').value : '';
    const info = await rpc('getwalletinfo', [], 'enc');
    await check('M2 a proof of funds signs with a locked wallet', proofText.length > 50, proofText.slice(0, 60));
    await check('M2 and the wallet is locked again at once', info.unlocked_until === 0, JSON.stringify(info.unlocked_until));
    await check('M2 the page never unlocked it for a while', !calls.some((c) => c.method === 'walletpassphrase'));
    T().selectWallet('big');

    // M5: the text of every theme reaches 4.5:1, measured on what the page draws.
    const parse = (c) => {
      const m = c.match(/rgba?\(([^)]+)\)/);
      if (!m) return null;
      const p = m[1].split(/[ ,/]+/).filter(Boolean).map(Number);
      return { r: p[0], g: p[1], b: p[2], a: p.length > 3 ? p[3] : 1 };
    };
    const lum = ({ r, g, b }) => {
      const f = (v) => ((v /= 255) <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4));
      return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b);
    };
    const blend = (fg, bg) => ({ r: fg.r * fg.a + bg.r * (1 - fg.a), g: fg.g * fg.a + bg.g * (1 - fg.a), b: fg.b * fg.a + bg.b * (1 - fg.a), a: 1 });
    const bgOf = (el) => {
      const stack = [];
      for (let e = el; e; e = e.parentElement) {
        const c = parse(getComputedStyle(e).backgroundColor);
        if (c && c.a > 0) {
          stack.push(c);
          if (c.a >= 1) break;
        }
      }
      let bg = { r: 255, g: 255, b: 255, a: 1 };
      for (let i = stack.length - 1; i >= 0; i--) bg = blend(stack[i], bg);
      return bg;
    };
    const lowContrast = () => {
      const out = [];
      const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
      const seen = new Set();
      while (walker.nextNode()) {
        const node = walker.currentNode;
        const el = node.parentElement;
        if (!el || seen.has(el) || !node.textContent.trim()) continue;
        seen.add(el);
        const r = el.getBoundingClientRect();
        if (!r.width || !r.height || r.bottom < 0 || r.top > innerHeight) continue;
        const cs = getComputedStyle(el);
        if (cs.visibility === 'hidden' || Number(cs.opacity) === 0) continue;
        if (el.closest('[disabled],[aria-disabled=true],.edge,.measure,svg,canvas') || el.closest('dialog:not([open])')) continue;
        let fg = parse(cs.color);
        if (!fg) continue;
        let opacity = 1;
        for (let e = el; e; e = e.parentElement) opacity *= Number(getComputedStyle(e).opacity);
        const bg = bgOf(el);
        fg = blend({ ...fg, a: fg.a * opacity }, bg);
        const [a, b] = [lum(fg), lum(bg)];
        const ratio = (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
        const size = parseFloat(cs.fontSize);
        const need = size >= 24 || (size >= 18.66 && Number(cs.fontWeight) >= 700) ? 3 : 4.5;
        if (ratio < need - 0.01) out.push(`${ratio.toFixed(2)} "${node.textContent.trim().slice(0, 24)}"`);
      }
      return out;
    };
    const contrast = {};
    for (const theme of ['light', 'dark', 'ocean', 'matrix', 'qdarkstyle']) {
      T().savePrefs({ theme });
      await sleep(300);
      const failing = [];
      for (const page of ['overview', 'send', 'receive', 'transactions', 'sidechains', 'explorer', 'node', 'settings', 'wallets']) {
        await go(page);
        await sleep(200);
        for (const f of lowContrast()) failing.push(`${page}: ${f}`);
      }
      contrast[theme] = failing;
    }
    T().savePrefs({ theme: 'light' });
    for (const [theme, failing] of Object.entries(contrast)) {
      await check(`M5 contrast of the ${theme} theme`, failing.length === 0, `${failing.length}: ${failing.slice(0, 6).join('; ')}`);
    }

    // C16: the window framed by the page (the integrated title bar of the layouts) follows the option.
    T().go('settings', 'display');
    const useSystem = await waitFor('the title bar option', () => [...document.querySelectorAll('label.check')].find((l) => l.textContent.includes('Use the title bar of the desktop')), 5000);
    const applyButton = () => [...document.querySelectorAll('.apply-bar button')].find((b) => b.textContent.trim() === 'Apply');
    useSystem.querySelector('input').click();
    // It waits for Apply, as the other settings.
    await new Promise((r) => setTimeout(r, 300));
    let frameState = await host('window.state');
    await check('C16 the title bar option waits for Apply', frameState.titleBar === 'integrated' && !!$('.titlestrip, .top.titlebar') && !applyButton().disabled, JSON.stringify(frameState));
    applyButton().click();
    await waitFor('the frame of the desktop', () => !$('.titlestrip, .top.titlebar') && !$('.resize'), 5000);
    frameState = await host('window.state');
    await check('C16 the option gives the frame back to the desktop', frameState.titleBar === 'system' && frameState.frameless === false, JSON.stringify(frameState));
    useSystem.querySelector('input').click();
    await waitFor('Apply, for the way back', () => !applyButton().disabled, 5000);
    applyButton().click();
    await waitFor('the integrated title bar again', () => !!$('.titlestrip, .top.titlebar'), 5000);
    frameState = await host('window.state');
    await check('C16 and takes it again', frameState.titleBar === 'integrated' && frameState.frameless === true, JSON.stringify(frameState));
    T().go('overview');

    // M7: a change of the options left unapplied (the runner checks it is gone at the next start), and an applied one.
    await host('options.set', { name: 'prune', value: true });
    await host('options.set', { name: 'dbcache', value: 777 });
    let options = await host('options.get');
    await check('M7 the changes wait for Apply', options.pending === 3 && options.prune === true, JSON.stringify({ pending: options.pending, prune: options.prune }));
    await host('options.cancel');
    options = await host('options.get');
    await check('M7 Cancel takes them back', options.pending === 0 && options.prune === false && options.dbcache !== 777, JSON.stringify({ pending: options.pending, prune: options.prune, dbcache: options.dbcache }));
    await host('options.set', { name: 'par', value: 2 });
    await host('options.apply');
    await host('options.set', { name: 'prune', value: true });
    await log('UNAPPLIED prune');

    await finish();
  } catch (e) {
    await finish(e);
  }
})();
