// The app of a sidechain against a live mainchain (run_sidechain.py), driven through its page as a
// user would: a wallet, a deposit address, a deposit made by the helper of the mainchain wallet,
// merged mining turned on, the deposit credited by a block the mainchain committed to, a withdrawal
// to an address of the mainchain wallet, its bundle voted on and paid. Pictures of each step, of every
// page of the sidechain, in the layouts and looks.
(async () => {
  const { host, rpc, sleep, log, T, waitFor, check, snapshot, text, finish } = E2E;
  /** The confirmations of the desktop answered so far (test.confirmAnswer queues the answers). */
  const confirmed = () => host('test.confirmed');
  const SLOT = window.__E2E_SLOT;
  const MAIN = window.__E2E_MAINCHAIN;
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
    await sleep(80);
  }
  async function answerDialog(title, label) {
    const d = await waitFor(`the dialog "${title}"`, () => {
      const el = openDialog();
      return el && $('h2', el) && $('h2', el).textContent.trim() === title ? el : null;
    });
    await click(label, d);
    await waitFor(`the dialog "${title}" to go`, () => openDialog() !== d || !d.open);
  }
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(200);
  }
  const sideRpc = (method, params = []) => rpc(method, params, 'side');
  /** A picture of the page, without the sync overlay a new sidechain shows until its first blocks. */
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = button('Hide', overlay);
      if (hide) hide.click();
      await sleep(300);
    }
    await snapshot(name);
  }

  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    const info = await waitFor('the node to reach the mainchain', async () => {
      const i = await rpc('getmainchaininfo');
      return i.connected ? i : null;
    }, 60000);
    await check('the node follows the mainchain, in its slot', info.slot === SLOT && info.connected === true, JSON.stringify(info));
    await check('the dashboard and Midnight by default', T().prefs.layout === 'dashboard' && (T().prefs.theme ?? 'midnight') === 'midnight', `${T().prefs.layout} ${T().prefs.theme}`);

    // A wallet.
    await rpc('createwallet', ['side']);
    await waitFor('the wallet in the page', () => T().wallets.current === 'side');
    await go('home');
    await sleep(1500);
    await shot('sc-home-empty');

    // --- Deposit: a deposit address, then the helper of the mainchain wallet ---
    await go('peg', 'deposit');
    await waitFor('the connection to the mainchain', () => text('.side-bridge').includes('Connected'), 60000);
    await click('New deposit address');
    const deposit = await waitFor('the deposit address', () => {
      const t = text('#deposit-address').trim();
      return t.startsWith(`s${SLOT}_`) ? t : null;
    });
    await check('a deposit address of the slot', deposit.startsWith(`s${SLOT}_`), deposit);
    await sleep(1500);
    await shot('sc-peg-deposit-address');
    set($('#deposit-amount'), '5');
    await sleep(200);
    // The wallet of the mainchain node pays: the host asks the desktop, the amount first (audit port-1 #4).
    // Declined first: nothing is sent.
    let asked = (await confirmed()).length;
    await host('test.confirmAnswer', { yes: false });
    $('#deposit-helper').click();
    await waitFor('the deposit asked on the desktop', async () => (await confirmed()).length > asked, 30000);
    await sleep(1500);
    await check('a deposit declined on the desktop sends nothing, and no error shows', !text('.peg').includes('On its way') && !$$('dialog[open]').some((d) => /Deposit from/.test(d.textContent)));
    asked = (await confirmed()).length;
    await host('test.confirmAnswer', { yes: true });
    $('#deposit-helper').click();
    await waitFor('the deposit asked on the desktop', async () => (await confirmed()).length > asked, 30000);
    const depositAsked = (await confirmed()).at(-1);
    await check('the deposit is confirmed on the desktop, its amount first', depositAsked.title === `Deposit from the wallet of ${MAIN}` && depositAsked.message.startsWith(`Amount: 5.00 coins of ${MAIN}`) && depositAsked.message.includes(deposit.slice(deposit.indexOf('_') + 1, deposit.lastIndexOf('_'))), JSON.stringify(depositAsked));
    await waitFor('the deposit on its way', () => text('.peg').includes('On its way'), 60000);
    await check('the deposit is on its way', true);
    await shot('sc-peg-deposit-on-its-way');

    // --- Merged mining: a block with every block of the mainchain ---
    await go('bmm');
    await waitFor('the merged mining page', () => $('#bmm-start'));
    $('#bmm-always').click();
    await sleep(150);
    asked = (await confirmed()).length;
    await host('test.confirmAnswer', { yes: true });
    await click('Start mining');
    await waitFor('merged mining asked on the desktop', async () => (await confirmed()).length > asked, 30000);
    const bmmAsked = (await confirmed()).at(-1);
    await check('merged mining is confirmed on the desktop, its offer first', bmmAsked.title === 'Turn merged mining on' && bmmAsked.message.startsWith(`Offer: 0.0001 coins of ${MAIN} for every block of ${MAIN}`), JSON.stringify(bmmAsked));
    await waitFor('mining on', () => text('.bmm').includes('Mining'), 30000);
    const bmm = await rpc('getbmminfo');
    await check('merged mining on, always', bmm.mining === true && bmm.always === true, JSON.stringify(bmm));
    await waitFor('a block of this chain', () => T().node.tip.height >= 1, 120000);
    await check('the mainchain commits to blocks of this chain', true);

    // --- The deposit credited ---
    await waitFor('the deposit credited', async () => (await sideRpc('getbalances')).mine.trusted >= 5, 180000);
    await check('the deposit credited to the wallet', true);
    await go('bmm');
    await waitFor('the blocks committed', () => $$('.side-blocks span.hit').length > 0, 60000);
    await sleep(800);
    await shot('sc-bmm-mining');
    await go('peg', 'deposit');
    await waitFor('the deposit credited in the list', () => text('.peg').includes('Credited'), 60000);
    await shot('sc-peg-deposit-credited');
    // A narrow panel (a window of 480 px): the table is a list of cards, no column behind a scroll (audit port-1 #9).
    const wrap = $('.side-table-wrap');
    wrap.style.width = '400px';
    await sleep(300);
    const cells = $$('td', wrap);
    const hidden = cells.filter((td) => td.getBoundingClientRect().right > wrap.getBoundingClientRect().right + 1);
    await check('at 400 px the deposits are cards, every cell in view under the words of its column',
      wrap.scrollWidth <= wrap.clientWidth + 1 && hidden.length === 0 && getComputedStyle($('thead', wrap)).display === 'none' && cells.some((td) => td.dataset.label === 'Amount'),
      JSON.stringify({ scroll: wrap.scrollWidth, client: wrap.clientWidth, hidden: hidden.length }));
    await shot('sc-peg-deposit-400');
    wrap.style.width = '';
    // Notices leave the form of the deposit helper usable (audit port-1 #11).
    T().toast('success', 'One', 'A notice.');
    T().toast('info', 'Two', 'Another notice.');
    await sleep(1800);
    const helper = $('#deposit-helper').getBoundingClientRect();
    const notes = $('.toasts').getBoundingClientRect();
    const covered = helper.left < notes.right && helper.right > notes.left && helper.top < notes.bottom && helper.bottom > notes.top;
    await check('notices do not cover the button of the deposit helper', !covered, JSON.stringify({ helper, notes }));
    await shot('sc-peg-deposit-notices');
    await go('home');
    await sleep(2000);
    await shot('sc-home-funded');
    // The balance whole, its unit too (audit port-2 #14: "39.70000000 C…"); the tables of Home cards in a narrow
    // panel, every cell in view (they scrolled sideways, columns hidden).
    const balance = $('.value.fit');
    await check('the balance of Home whole, its unit in view', !!balance && balance.scrollWidth <= balance.clientWidth + 1 && /CHN/.test(balance.textContent),
      JSON.stringify({ text: balance?.textContent, scroll: balance?.scrollWidth, client: balance?.clientWidth }));
    const scroll = $('#home-panel');
    await waitFor('the transactions of Home', () => $('table', scroll), 30000);
    scroll.style.width = '440px';
    await sleep(300);
    const homeCells = $$('td', scroll).filter((td) => getComputedStyle(td).display !== 'none');
    const homeHidden = homeCells.filter((td) => td.getBoundingClientRect().right > scroll.getBoundingClientRect().right + 1);
    await check('at 440 px the tables of Home are cards, every cell in view',
      scroll.scrollWidth <= scroll.clientWidth + 1 && homeHidden.length === 0 && getComputedStyle($('thead', scroll)).display === 'none',
      JSON.stringify({ scroll: scroll.scrollWidth, client: scroll.clientWidth, hidden: homeHidden.length }));
    await shot('sc-home-440');
    scroll.style.width = '';

    // --- Withdraw to an address of the mainchain wallet ---
    await go('peg', 'withdraw');
    await click(`An address of the ${MAIN} wallet`);
    const payTo = await waitFor('an address of the mainchain wallet', () => $('#withdraw-address') && $('#withdraw-address').value);
    set($('#withdraw-amount'), '1');
    await waitFor('the address checked', () => !text('.peg').includes('This is an address of'));
    await sleep(500);
    $('#withdraw-button').click();
    await answerDialog('Confirm the withdrawal', 'Withdraw');
    await waitFor('the withdrawal in a block', async () => (await rpc('listwithdrawals')).length === 1, 120000);
    await check('the withdrawal waits for a bundle', true);
    await waitFor('the withdrawal in the list', () => $$('table[aria-label="Withdrawals not paid yet"] tbody tr').length === 1, 30000);
    await shot('sc-peg-withdraw');
    await log(`WITHDRAWN ${payTo} 1`);

    // --- The bundle: voted on by the miners of the mainchain, paid ---
    await go('peg', 'bundle');
    await waitFor('the bundle being voted on', async () => (await rpc('getwithdrawalbundle')).status === 'pending', 180000);
    await waitFor('its votes on the mainchain', () => / of 30/.test(text('.peg .bundle')), 120000);
    await check('the votes of the bundle show', true);
    await shot('sc-peg-bundle-voting');
    await waitFor('the bundle paid', () => $$('table[aria-label="Bundles"] .status.ok').length > 0, 300000);
    await check('the bundle paid out', true);
    await waitFor('no withdrawal left', async () => (await rpc('listwithdrawals')).length === 0, 120000);
    await sleep(800);
    await shot('sc-peg-bundle-paid');

    // --- Every page of the sidechain, in the layouts and the looks ---
    await go('home');
    await sleep(1500);
    await shot('sc-home-midnight');
    for (const [page, tab, name] of [['settings', 'mainchain', 'sc-settings-mainchain'], ['node', 'info', 'sc-node'], ['explorer', '', 'sc-explorer'], ['overview', '', 'sc-overview'], ['tools', '', 'sc-tools']]) {
      await go(page, tab);
      await sleep(800);
      await shot(name);
    }
    T().savePrefs({ theme: 'light' });
    for (const [page, tab] of [['home', ''], ['peg', 'deposit'], ['peg', 'bundle'], ['bmm', '']]) {
      await go(page, tab);
      await sleep(900);
      await shot(`sc-${page}${tab ? '-' + tab : ''}-light`);
    }
    T().savePrefs({ theme: 'midnight', layout: 'workspace' });
    await go('peg', 'withdraw');
    await sleep(900);
    await shot('sc-workspace-peg');
    // Home in every layout (the Compact layout is gone): the workspace's Home area.
    await go('home');
    await sleep(1200);
    await shot('sc-workspace-home');
    T().savePrefs({ layout: 'dashboard', theme: 'ocean' });
    await go('home');
    await sleep(1200);
    await shot('sc-home-ocean');
    T().savePrefs({ theme: 'midnight' });

    // Merged mining off again (no word of the desktop needed to stop paying).
    await go('bmm');
    asked = (await confirmed()).length;
    await click('Stop mining');
    await waitFor('mining off', async () => (await rpc('getbmminfo')).mining === false, 30000);
    await check('merged mining off, without asking', (await confirmed()).length === asked);
    // One block by hand, declined on the desktop: nothing is asked of the mainchain.
    await host('test.confirmAnswer', { yes: false });
    const requestsBefore = (await rpc('getbmminfo')).requests ?? null;
    await click('Mine one block');
    await waitFor('one block asked on the desktop', async () => (await confirmed()).length > asked, 30000);
    await sleep(800);
    const onceAsked = (await confirmed()).at(-1);
    await check('one block by hand is confirmed on the desktop; declined, no error shows', onceAsked.title === 'Mine one block of this chain' && !$$('dialog[open]').some((d) => /Merged mining/.test(d.textContent)) && JSON.stringify((await rpc('getbmminfo')).requests ?? null) === JSON.stringify(requestsBefore), JSON.stringify(onceAsked));
    // The page cannot spend the mainchain wallet with an RPC call of its own.
    const direct = await E2E.asPage('setbmm', [false], true).then(() => null, (e) => e);
    await check('setbmm is not a method the page calls', direct && direct.code === -32601, JSON.stringify(direct));
    // Another machine as the mainchain node: on the desktop's word only (its credentials go there unencrypted).
    await host('test.confirmAnswer', { yes: false });
    const remote = await host('options.set', { name: 'mainchainConnect', value: 'mainchain.example' }).then(() => null, (e) => e);
    const remoteAsked = (await confirmed()).at(-1);
    await check('a mainchain node on another machine is confirmed on the desktop; declined, not set', remote && /declined/.test(remote.message) && remoteAsked.message.startsWith('Host: mainchain.example'), JSON.stringify([remote, remoteAsked]));
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
