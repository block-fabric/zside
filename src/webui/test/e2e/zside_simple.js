// zSide in Simple mode (run_sidechain.py --script zside_simple.js --js-var SIMPLE=true): as a user chooses it;
// (a first start is in Advanced). The wallet gets coins (a deposit by the helper of the mainchain wallet, merged mining on); then through
// the pages of Simple mode: the private balance, coins made private (Make private), a payment with a message
// to a second wallet from the Send page's "Send privately", and coins made public again; pictures of each in
// Midnight and Light.
(async () => {
  const { host, rpc, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const openDialog = () => $$('dialog[open]').pop();
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function clickId(id, ms = 30000) {
    const b = await waitFor(`#${id} enabled`, () => {
      const el = document.getElementById(id);
      return el && !el.disabled ? el : null;
    }, ms);
    b.click();
    await sleep(150);
  }
  async function answerDialog(title, label) {
    const d = await waitFor(`the dialog "${title}"`, () => {
      const el = openDialog();
      return el && $('h2', el) && $('h2', el).textContent.trim() === title ? el : null;
    });
    const b = await waitFor(`the button "${label}"`, () => {
      const el = button(label, d);
      return el && !el.disabled ? el : null;
    });
    b.click();
    await waitFor(`the dialog "${title}" to go`, () => openDialog() !== d || !d.open);
  }
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(400);
  }
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = button('Hide', overlay);
      if (hide) hide.click();
      await sleep(300);
    }
    await sleep(500);
    await snapshot(name);
  }
  const shielded = async (wallet) => rpc('getshieldedbalance', [], wallet);

  try {
    // What pays or signs is confirmed in the dialog of the host (audit 7 #1): every question accepted here, checked at the end.
    await host('test.confirmAnswer', { always: true });
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await check('Advanced on a first start (user decision), Simple once chosen', window.__E2E_FIRST_MODE === 'advanced' && T().prefs.uiMode === 'simple', `${window.__E2E_FIRST_MODE} ${T().prefs.uiMode}`);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);

    await rpc('createwallet', ['zs2']);
    await rpc('createwallet', ['zs']);
    T().selectWallet('zs');
    await waitFor('the wallet in the page', () => T().wallets.current === 'zs');
    const deposit = await rpc('getdepositaddress', [], 'zs');
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], 'zs'), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], 'zs')).mine.trusted >= 20, 240000);

    const sidebar = text('nav.sidebar .scroll');
    await check('the sidebar: Private, without memos, melt & cast and the pool', sidebar.includes('Private') && !sidebar.includes('Memos') && !sidebar.includes('Melt') && !sidebar.includes('Shielded pool'), sidebar);

    // --- Make private: 8 through the page ---
    await go('private', 'move');
    await waitFor('the Make private form', () => $('#zs-shield-amount'));
    await check('no memo nor other address in Simple mode', !$('#zs-shield-memo') && !$('#zs-unshield-to'));
    await shot('zs-simple-make-private-midnight');
    set($('#zs-shield-amount'), '8');
    await sleep(300);
    await clickId('zs-shield');
    await answerDialog('Make private', 'Make private');
    await waitFor('the coins made private', async () => (await shielded('zs')).available >= 8, 240000);
    await check('8 made private through the page', (await shielded('zs')).available === 8, JSON.stringify(await shielded('zs')));

    // --- Send privately, from the Send page ---
    const to = await rpc('getshieldedaddress', [], 'zs2');
    await go('send');
    const toggle = await waitFor('the Send privately switch', () => $('input[role="switch"]'));
    toggle.click();
    await waitFor('the private form', () => $('#zs-sp-to'));
    set($('#zs-sp-to'), to);
    set($('#zs-sp-amount'), '1.25');
    set($('#zs-sp-memo'), 'hello in Simple mode ✓');
    await waitFor('the address taken as private', () => text('.send-privately').includes('A shielded address'), 10000);
    await sleep(300);
    await shot('zs-simple-send-privately-midnight');
    await clickId('zs-sp-send');
    await answerDialog('Send privately', 'Pay');
    await waitFor('the payment received', async () => (await shielded('zs2')).available === 1.25, 240000);
    const received = await rpc('listshieldedtransactions', [], 'zs2');
    await check('sent privately from the Send page, with the message', received.some((tx) => tx.memos.includes('hello in Simple mode ✓')), JSON.stringify(received));

    // --- Make public: 1, back to the wallet's own public address ---
    await go('private', 'move');
    set(await waitFor('the Make public form', () => $('#zs-unshield-amount')), '1');
    await sleep(300);
    await clickId('zs-unshield');
    await answerDialog('Make public', 'Make public');
    await waitFor('the coins made public', async () => (await shielded('zs')).available < 5.75, 240000);
    await check('made public through the page', true);

    for (const theme of ['midnight', 'light']) {
      T().savePrefs({ theme });
      await go('home');
      await waitFor('the private card', () => button('Send privately'), 30000);
      await shot(`zs-simple-home-${theme}`);
      await go('private', 'overview');
      await waitFor('the private balance', () => text('.privatepage').includes('Private balance'));
      await shot(`zs-simple-private-balance-${theme}`);
      await go('private', 'receive');
      await shot(`zs-simple-private-address-${theme}`);
      if (theme === 'light') {
        await go('private', 'move');
        await shot('zs-simple-make-private-light');
        await go('send');
        const sw = await waitFor('the switch', () => $('input[role="switch"]'));
        if (!sw.checked) sw.click();
        await waitFor('the private form', () => $('#zs-sp-to'));
        await shot('zs-simple-send-privately-light');
      }
    }
    {
      // The confirmations of the desktop: what the chain's pages paid or signed, each with its facts.
      const titles = ["Cast coins", "Increase the fee", "Melt coins", "Send privately", "Shield coins", "Unshield coins"];
      const asked = (await host('test.confirmed', {})).filter((q) => titles.includes(q.title));
      await check(`the desktop confirmed what the pages paid or signed (${asked.length})`, asked.length > 0 && asked.every((q) => q.rows.length > 1), JSON.stringify(asked.map((q) => q.title)));
      // A private payment and an unshielding say their fee and what is paid in all (audit 7 E-ZS3).
      const paying = asked.filter((q) => q.title === 'Send privately' || q.title === 'Unshield coins');
      await check('the payments confirmed with their fee', paying.length >= 2 && paying.every((q) => q.rows.some((r) => r.label === 'Fee' && /^0\.\d+ /.test(r.value))), JSON.stringify(paying.map((q) => q.rows)));
    }
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
