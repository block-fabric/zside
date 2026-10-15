// Pictures of the pages the user decisions of 2026-10-15 changed, in Midnight and Light: Receive →
// Addresses, Send with "Choose coins" on, Tools (its tabs), Advanced transactions (its tabs); and Home,
// in the workspace (its first area) and the dashboard (the compact sidebar is gone, Home in every layout).
(async () => {
  const { rpc, sleep, T, waitFor, check, snapshot, finish } = E2E;
  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    await rpc('createwallet', ['demo']);
    await waitFor('the wallet', () => T().wallets.current === 'demo');
    const mining = await rpc('getnewaddress', ['Mining'], 'demo');
    await rpc('generatetoaddress', [110, mining]);
    for (const [label, amount] of [['Coffee shop', 0.42], ['Rent', 12.5], ['Savings', 3]]) {
      const to = await rpc('getnewaddress', [label], 'demo');
      await rpc('sendtoaddress', [to, amount], 'demo');
    }
    await rpc('getnewaddress', ['Not used yet', 'bech32m'], 'demo');
    await rpc('getnewaddress', ['Old style', 'legacy'], 'demo');
    await rpc('generatetoaddress', [2, mining]);
    await waitFor('the balance', () => T().wallets.balances.demo && T().wallets.balances.demo.trusted > 0n);
    const payee = await rpc('getnewaddress', ['Payee'], 'demo');
    // One coin locked.
    const coins = await rpc('listunspent', [], 'demo');
    await rpc('lockunspent', [false, [{ txid: coins[0].txid, vout: coins[0].vout }]], 'demo');

    for (const theme of ['midnight', 'light']) {
      for (const layout of ['workspace', 'dashboard']) {
        T().savePrefs({ theme, layout });
        T().go('home');
        await waitFor(`${layout}: Home`, () => { const h = $('.home'); return h && h.textContent.includes('CHN') && h.querySelector('table tbody tr'); }, 30000);
        if (layout === 'workspace') {
          const first = $('.workspace .areas .area');
          await check(`workspace ${theme}: Home, the first area, is the one shown`, first && first.textContent.includes('Home') && first.getAttribute('aria-current') === 'true', first ? first.outerHTML.slice(0, 160) : 'none');
        }
        await sleep(800);
        await snapshot(`${layout}-home-${theme}`);
      }

      T().savePrefs({ theme, layout: 'dashboard' });
      T().go('receive', 'addresses');
      await waitFor('the addresses', () => { const g = $('[role="grid"][aria-label="Receiving addresses"]'); return g && g.textContent.includes('Coffee shop'); }, 30000);
      await sleep(600);
      await snapshot(`receive-addresses-${theme}`);

      T().savePrefs({ coinControl: true });
      T().go('send');
      const grid = await waitFor('the coins', () => { const g = $('[role="grid"][aria-label="Coins of the wallet"]'); return g && $$('input[type="checkbox"]', g).length > 2 ? g : null; }, 30000);
      set(await waitFor('the send form', () => $('input[id^="pay-to-"]')), payee);
      set($('input[id^="amount-"]'), '60');
      const boxes = $$('input[type="checkbox"]', grid);
      if (!boxes[0].checked) boxes[0].click();
      await waitFor('not enough', () => $('.coins [role="alert"]'), 20000);
      await sleep(600);
      $('.coins').scrollIntoView({ block: 'start' });
      await snapshot(`send-coins-short-${theme}`);
      if (!boxes[1].checked) boxes[1].click();
      await waitFor('enough', () => !$('.coins [role="alert"]') && $('.coins').textContent.includes('Quantity'), 20000);
      await sleep(600);
      $('.coins').scrollIntoView({ block: 'start' });
      await snapshot(`send-coins-${theme}`);
      T().savePrefs({ coinControl: false });

      for (const tab of ['timestamp', 'proof', 'crypto']) {
        T().go(tab === 'crypto' ? 'crypto' : tab, tab === 'crypto' ? 'merkle' : '');
        await sleep(700);
        await snapshot(`tools-${tab}-${theme}`);
      }
      for (const tab of ['messages', 'psbt', 'multisig']) {
        T().go(tab);
        await sleep(700);
        await snapshot(`advanced-${tab}-${theme}`);
      }
    }
    await check('the pictures', true);
    T().savePrefs({ theme: 'midnight' });
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
