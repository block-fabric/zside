// The block explorer of a sidechain against a live mainchain (run_sidechain.py --script explorer_side.js): a deposit
// credited by a merged mined block and a payment within the wallet; then the block that paid the deposit with its peg
// (the deposit marked yours, the transaction and the block of the mainchain copied, never linked), the payment with the
// coins it spent and its block linked, an address of the wallet from what the wallet knows, Back, and a foreign address.
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
  /** The view shown at the top of the window: what the pictures are of. */
  const toView = () => {
    const title = $('#explorer-title');
    if (title) title.closest('section').scrollIntoView({ block: 'start' });
  };
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
    set($('#deposit-amount'), '5');
    await sleep(200);
    // The wallet of the mainchain node pays: the host asks in its dialog window, the amount first (audit port-1 #4).
    // Declined first, there (Escape), as a user would: nothing is sent.
    let asked = (await confirmed()).length;
    await host('test.confirmAnswer', { show: true });
    $('#deposit-helper').click();
    await waitFor('the deposit asked in the dialog window', async () => (await host('test.dialog', { action: 'kind' })) === 'themed', 30000);
    const shown = await waitFor('its page', async () => {
      const s = await host('test.dialog', { action: 'eval', script: "(() => ({ title: document.getElementById('dialog-title') && document.getElementById('dialog-title').textContent, theme: document.documentElement.dataset.theme, focused: document.activeElement && document.activeElement.id, loading: document.body.classList.contains('loading') }))()" });
      return s && !s.loading && s.title ? s : null;
    }, 30000);
    await check('the deposit is asked in the dialog window of the host, in the look of the app, Cancel first', shown.title === `Deposit from the wallet of ${MAIN}` && shown.theme === document.documentElement.dataset.theme && shown.focused === 'dialog-cancel', JSON.stringify(shown));
    await host('test.dialog', { action: 'snapshot', name: 'sc-dialog-deposit' });
    await host('test.dialog', { action: 'eval', script: "document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })); true" });
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

    // --- A payment within the wallet: a transaction whose inputs and outputs are the wallet's ---
    const to = await sideRpc('getnewaddress', ['Savings']);
    const paid = await sideRpc('sendtoaddress', [to, 1]);
    await waitFor('the payment in a block', async () => ((await sideRpc('gettransaction', [paid])).confirmations ?? 0) >= 1, 180000);
    const payTx = await sideRpc('gettransaction', [paid]);
    const credit = (await sideRpc('listtransactions', ['*', 100])).find((e) => e.amount === 5 && e.blockhash);
    await check('the deposit was paid by a coinbase in a block', !!credit, JSON.stringify(credit));

    // --- The explorer: the block that paid the deposit, its peg, the block of the mainchain that merged mined it ---
    T().savePrefs({ theme: 'midnight' });
    await go('explorer', `search:${credit.blockhash}`);
    const pegView = await waitFor('the peg of the block', () => $('section[aria-label="The peg in this block"]'), 60000);
    const payouts = await waitFor('the payouts of the coinbase', () => $('ul[aria-label="Paid by the coinbase"]', pegView));
    await waitFor('the deposit marked yours', () => $('[data-ours]', payouts), 30000);
    await check('the deposit paid to an address of the wallet is marked yours', /yours/.test(payouts.textContent));
    await check('the deposit names the transaction of the mainchain, copied, never linked', /deposit/.test(payouts.textContent) && !$$('button', payouts).some((b) => /^[0-9a-f]{8}/.test(b.textContent) && b.title.includes(MAIN)));
    const main = $$('dd').find((d) => (d.getAttribute('title') || '').startsWith(`A block of ${MAIN}`));
    await check('the block of the mainchain that merged mined it, with where to see it', !!main && !$('button.linkish', main), main ? main.textContent : 'none');
    await sleep(800);
    toView();
    await sleep(300);
    await shot('explorer-side-block-peg');

    // --- The payment: inputs (the coins spent, their addresses) and outputs, linked, the wallet's marked ---
    await go('explorer', `tx:${paid}:${payTx.blockhash}`);
    const tx = await waitFor('the transaction', () => {
      const s = $('section[aria-label="Transaction"]');
      return s && s.textContent.includes(paid.slice(0, 12)) ? s : null;
    }, 60000);
    await waitFor('the addresses of the wallet marked', () => $$('[data-ours]', tx).length >= 2, 30000);
    await check('the coins spent are read with the transaction, their addresses linked', $$('.io-line button.linkish', tx).length >= 1);
    await check('the block of the transaction is a link', $$('button.linkish', tx).some((b) => b.textContent.includes(payTx.blockhash)));
    await sleep(800);
    toView();
    await sleep(300);
    await shot('explorer-side-tx');
    // An address of the wallet: its view, from what the wallet knows.
    const ours = $('[data-ours] button.linkish', tx);
    ours.click();
    const view = await waitFor('the address view', () => {
      const s = $('section[aria-label="Address"]');
      return s && /Holds now/.test(s.textContent) ? s : null;
    }, 30000);
    await check('the address view: what the wallet knows, its transactions', !!$('table[aria-label="Transactions of the address"]', view));
    await sleep(800);
    toView();
    await sleep(300);
    await shot('explorer-side-address');
    // Back: the transaction again.
    button('Back', view).click();
    await waitFor('back to the transaction', () => $('section[aria-label="Transaction"]'), 30000);
    await check('Back returns to the transaction', true);
    // An address of another wallet: its kind and script only.
    await rpc('createwallet', ['other']);
    const foreign = await rpc('getnewaddress', [], 'other');
    await rpc('unloadwallet', ['other']);
    await waitFor('the wallet again', () => T().wallets.current === 'side', 30000).catch(() => null);
    set($('#explorer-search'), foreign);
    $('#explorer-search').closest('form').requestSubmit();
    const other = await waitFor('the view of a foreign address', () => {
      const s = $('section[aria-label="Address"]');
      return s && /Not an address of this wallet/.test(s.textContent) ? s : null;
    }, 30000);
    await check('a foreign address: no record of it, no list', !$('table', other) && /The explorer keeps no record of other addresses/.test(other.textContent));
    toView();
    await sleep(300);
    await shot('explorer-side-foreign');
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
