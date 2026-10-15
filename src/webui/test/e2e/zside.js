// zSide against a live mainchain (run_sidechain.py --script zside.js): the shielded wallet driven through
// its pages as a user would. The wallet gets coins (a deposit by the helper of the mainchain wallet, merged
// mining on); then, through the pages: coins shielded with a memo, a private payment with a memo to a second
// wallet (whose inbox shows the memo), an unshielding, the viewing key exported after its warning and
// watched in a wallet without keys (which sees the account, watch-only), a cast planned and cancelled, and
// the pool. Each proof is shown while it runs, and the page answers meanwhile. Pictures of every page of
// zSide on the way, in the layouts (Dashboard, Workspace) and looks (Midnight, Light).
(async () => {
  const { host, rpc, sleep, log, T, waitFor, check, snapshot, text, finish } = E2E;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const openDialog = () => $$('dialog[open]').pop();
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function click(label, root) {
    const b = await waitFor(`the button "${label}"`, () => {
      const el = button(label, typeof root === 'function' ? root() : root);
      return el && !el.disabled ? el : null;
    });
    b.click();
    await sleep(120);
  }
  async function clickId(id, ms = 30000) {
    const b = await waitFor(`#${id} enabled`, () => {
      const el = document.getElementById(id);
      return el && !el.disabled ? el : null;
    }, ms);
    b.click();
    await sleep(150);
  }
  async function answerDialog(title, label, before) {
    const d = await waitFor(`the dialog "${title}"`, () => {
      const el = openDialog();
      return el && $('h2', el) && $('h2', el).textContent.trim() === title ? el : null;
    });
    if (before) await before(d);
    await click(label, d);
    await waitFor(`the dialog "${title}" to go`, () => openDialog() !== d || !d.open);
  }
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(300);
  }
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = button('Hide', overlay);
      if (hide) hide.click();
      await sleep(300);
    }
    await snapshot(name);
  }
  const shielded = async (wallet) => rpc('getshieldedbalance', [], wallet);

  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    await check('zSide, the dashboard and Midnight by default', T().app.info.name === 'zSide' && T().prefs.layout === 'dashboard' && (T().prefs.theme ?? 'midnight') === 'midnight', `${T().app.info.name} ${T().prefs.layout}`);

    // --- Two wallets, coins in the first ---
    await rpc('createwallet', ['zs2']);
    await rpc('createwallet', ['zs']);
    T().selectWallet('zs');
    await waitFor('the wallet in the page', () => T().wallets.current === 'zs');
    const deposit = await rpc('getdepositaddress', [], 'zs');
    // The wallet of the mainchain node pays: on the desktop's word (audit port-1 #4), accepted here.
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], 'zs'), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], 'zs')).mine.trusted >= 20, 240000);

    await go('home');
    await waitFor('the cards of zSide on the home', () => text('.home').includes('Memos') && /Private/.test(text('.home')), 30000);
    await sleep(1200);
    await shot('zs-home-empty');

    // --- Shield 8, with a note to self ---
    await go('private', 'move');
    await waitFor('the shield form', () => $('#zs-shield-amount'));
    set($('#zs-shield-amount'), '8');
    set($('#zs-shield-memo'), 'e2e savings');
    await sleep(300);
    await clickId('zs-shield');
    await answerDialog('Shield', 'Shield');
    // The proof runs in the node: the page shows it, and answers meanwhile.
    const seen = await waitFor('the proof shown', () => $('.proving') || T().wallets.txVersion > 0, 20000);
    if (seen instanceof Element) {
      await shot('zs-proving');
      const t0 = performance.now();
      await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
      await check('the page answers while the node proves', performance.now() - t0 < 500, `${performance.now() - t0} ms`);
    }
    await waitFor('the coins shielded', async () => (await shielded('zs')).available >= 8, 240000);
    await check('8 shielded through the page', (await shielded('zs')).available === 8, JSON.stringify(await shielded('zs')));

    // --- A private payment with a memo to the second wallet ---
    const to = await rpc('getshieldedaddress', [], 'zs2');
    await go('private', 'pay');
    await waitFor('the pay form', () => $('#zs-pay-to-0'));
    set($('#zs-pay-to-0'), to);
    set($('#zs-pay-amount-0'), '1.25');
    set($('#zs-pay-memo'), 'hello from the e2e test ✓');
    await waitFor('the address taken as shielded', () => text('.privatepage').includes('A shielded address'), 10000);
    await sleep(300);
    await shot('zs-pay');
    await clickId('zs-pay');
    await answerDialog('Pay privately', 'Pay');
    await waitFor('the payment received', async () => (await shielded('zs2')).available === 1.25, 240000);
    const received = await rpc('listshieldedtransactions', [], 'zs2');
    await check('paid privately through the page, with the memo', received.some((tx) => tx.memos.includes('hello from the e2e test ✓')), JSON.stringify(received));

    // --- The memo in the inbox of the second wallet ---
    T().selectWallet('zs2');
    await waitFor('the second wallet in the page', () => T().wallets.current === 'zs2');
    await go('memos');
    await waitFor('the memo in the inbox', () => text('.memos').includes('hello from the e2e test ✓'), 30000);
    $$('.memos [role="option"]')[0].click();
    await sleep(500);
    await check('the memo read in its pane', text('.reader').includes('hello from the e2e test ✓'));
    await shot('zs-memos');
    T().selectWallet('zs');
    await waitFor('the first wallet again', () => T().wallets.current === 'zs');

    // --- Unshield 1 to a new address of the wallet ---
    await go('private', 'move');
    await waitFor('the unshield form', () => $('#zs-unshield-amount'));
    set($('#zs-unshield-amount'), '1');
    await sleep(300);
    const before = (await rpc('getbalances', [], 'zs')).mine;
    await clickId('zs-unshield');
    await answerDialog('Unshield', 'Unshield');
    await waitFor('the coins unshielded', async () => {
      const m = (await rpc('getbalances', [], 'zs')).mine;
      return m.trusted + m.untrusted_pending >= before.trusted + before.untrusted_pending + 0.99;
    }, 240000);
    await check('unshielded through the page', true);

    // --- The viewing key: exported after its warning, watched in a wallet without keys ---
    await go('private', 'keys');
    await clickId('zs-export');
    await answerDialog('Export the viewing key', 'Show the key', async (d) => {
      const box = $('input[type="checkbox"]', d);
      box.click();
      await sleep(100);
    });
    const key = await waitFor('the key shown', () => {
      const el = $('.zs-code');
      return el && /^rzview1/.test(el.textContent.trim()) ? el.textContent.trim() : null;
    });
    await check('the key blurred until shown', $('.zs-code').classList.contains('hidden'));
    await shot('zs-keys');
    await rpc('createwallet', ['watch', true, true]);
    T().selectWallet('watch');
    await waitFor('the watch-only wallet in the page', () => T().wallets.current === 'watch');
    await go('private', 'keys');
    await waitFor('the import form', () => $('#zs-import-key'), 20000);
    set($('#zs-import-key'), key);
    set($('#zs-import-height'), '0');
    await sleep(300);
    await clickId('zs-import');
    await answerDialog('Watch a viewing key', 'Watch');
    const mine = await shielded('zs');
    await waitFor('the account seen, watch-only', async () => (await shielded('watch')).watchonly === mine.available + mine.pending, 120000);
    await check('the viewing key watched: the account seen, watch-only', true);
    await go('zhistory');
    await sleep(1200);
    await shot('zs-watch-history');
    T().selectWallet('zs');
    await waitFor('the first wallet again', () => T().wallets.current === 'zs');

    // --- Cast: planned, then cancelled ---
    await go('meltcast', 'cast');
    await waitFor('the cast form', () => $('#zs-cast-amount'));
    set($('#zs-cast-amount'), '0.07');
    await waitFor('the bills', () => text('.meltcast').includes('Comes out as 3 bill'), 10000);
    await shot('zs-cast');
    await clickId('zs-cast');
    await answerDialog('Cast', 'Cast');
    await waitFor('the plan', async () => (await rpc('listmeltcast', [], 'zs')).filter((s) => s.kind === 'cast').length === 3, 30000);
    await go('meltcast', 'plan');
    await sleep(900);
    await shot('zs-plan');
    await clickId('zs-cancel-all');
    await answerDialog('Cancel all', 'Cancel all');
    await waitFor('the plan cancelled', async () => (await rpc('listmeltcast', [], 'zs')).every((s) => s.status === 'sent'), 30000);
    await check('a cast planned and cancelled through the page', true);

    // --- Every page of zSide, in the layouts and looks ---
    for (const [page, tab] of [['private', ''], ['zhistory', ''], ['zhistory', 'notes'], ['pool', ''], ['home', '']]) {
      await go(page, tab);
      await sleep(1100);
      await shot(`zs-${page}${tab ? '-' + tab : ''}-midnight`);
    }
    T().savePrefs({ theme: 'light' });
    for (const page of ['home', 'private', 'memos', 'zhistory', 'meltcast', 'pool']) {
      await go(page);
      await sleep(900);
      await shot(`zs-${page}-light`);
    }
    T().savePrefs({ theme: 'midnight', layout: 'workspace' });
    for (const page of ['home', 'private', 'zhistory', 'pool']) {
      await go(page);
      await sleep(1100);
      await shot(`zs-workspace-${page}-midnight`);
    }
    T().savePrefs({ theme: 'light' });
    for (const page of ['home', 'private']) {
      await go(page);
      await sleep(1100);
      await shot(`zs-workspace-${page}-light`);
    }
    T().savePrefs({ theme: 'midnight', layout: 'dashboard' });

    await rpc('setbmm', [false]);
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
