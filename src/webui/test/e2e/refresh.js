// Pictures of the visual refresh (doc/webui-layouts.md, "The looks"): each layout in Midnight, the
// look of a first start — Home and the overview, Send (the side panel of the dashboard, the page
// otherwise), the transactions, the sidechains, the settings — then the dashboard in every other
// look, and a few states: the notifications, Receive.
(async () => {
  const { rpc, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  const $ = (selector, root = document) => root.querySelector(selector);
  const typeInto = (input, value) => {
    input.value = value;
    input.dispatchEvent(new Event('input', { bubbles: true }));
  };
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    await check('Midnight is the look of a first start', T().prefs.theme === 'midnight', T().prefs.theme);
    await check('the dashboard is the layout of a first start', T().prefs.layout === 'dashboard', T().prefs.layout);

    // A wallet with some history, two sidechains.
    await rpc('createwallet', ['demo']);
    await waitFor('the wallet', () => T().wallets.current === 'demo');
    const mining = await rpc('getnewaddress', ['Mining'], 'demo');
    await rpc('createsidechainproposal', [2, 'Thunder', 'Fast payments with a lightning-style sidechain']);
    await rpc('createsidechainproposal', [5, 'BitNames', 'Names and their records, on a sidechain']);
    await rpc('generatetoaddress', [130, mining]);
    for (const [label, amount] of [['Coffee shop', 0.42], ['Rent', 12.5], ['Savings', 3]]) {
      const to = await rpc('getnewaddress', [label], 'demo');
      await rpc('sendtoaddress', [to, amount], 'demo');
    }
    await rpc('generatetoaddress', [2, mining]);
    const later = await rpc('getnewaddress', ['Bookshop'], 'demo');
    await rpc('sendtoaddress', [later, 0.15], 'demo');
    await waitFor('the balance', () => T().wallets.balances.demo && T().wallets.balances.demo.trusted > 0n);
    const payee = await rpc('getnewaddress', ['Payee'], 'demo');

    const shot = async (name) => {
      await sleep(500);
      await snapshot(name);
    };
    const home = async () => {
      T().go('home');
      await waitFor('the home', () => text('.home').includes('Thunder') && text('.home').includes('Bookshop'), 30000);
    };

    for (const layout of ['dashboard', 'workspace']) {
      T().savePrefs({ theme: 'midnight', layout });
      await sleep(300);
      // Home, in every layout.
      await home();
      await shot(`${layout}-home-midnight`);
      T().go('overview');
      await waitFor('the overview', () => text('.balances').includes('CHN'), 20000);
      await shot(`${layout}-overview-midnight`);

      if (layout === 'dashboard') {
        // Send from the home: the side panel.
        await home();
        const send = [...document.querySelectorAll('.home .stats button')].find((b) => b.textContent.trim() === 'Send');
        send.click();
        const panel = await waitFor('the side panel', () => $('.quick-panel'));
        await waitFor('the big Send button', () => [...panel.querySelectorAll('.btn.cta')].some((b) => b.textContent.includes('Send')));
        typeInto(await waitFor('the address field', () => $('#quick-pay-to')), payee);
        typeInto($('#quick-amount'), '1.25');
        await waitFor('the address checked', () => !$('#quick-pay-to[aria-invalid="true"]'));
        await check('dashboard: Send in a side panel', true);
        await shot(`${layout}-send-midnight`);
        [...panel.querySelectorAll('[role="tab"]')].find((b) => b.textContent.includes('Receive')).click();
        await waitFor('the receive side', () => $('.quick-panel') && text('.quick-panel').includes('Create address'));
        [...document.querySelectorAll('.quick-panel button')].find((b) => b.textContent.includes('Create address')).click();
        await waitFor('the new address', () => $('.quick-panel svg.qr'));
        await shot(`${layout}-receive-midnight`);
        $('.quick-panel button[aria-label="Close"]').click();
      } else {
        T().go('send');
        await waitFor('the send page', () => $('#content .page'));
        await shot(`${layout}-send-midnight`);
      }

      T().go('transactions');
      const row = await waitFor('a transaction row', () => $('.transactions [role="row"][aria-rowindex="2"]'), 20000);
      row.dispatchEvent(new MouseEvent('click', { bubbles: true }));
      if (layout === 'workspace') {
        await waitFor('the detail panel', () => $('.detail-panel') && text('.detail-panel').includes('Transaction ID'));
      }
      await shot(`${layout}-transactions-midnight`);

      T().go('sidechains');
      await waitFor('the sidechains page', () => $('#content .page'));
      await shot(`${layout}-sidechains-midnight`);

      T().go('settings', 'display');
      await waitFor('the settings', () => text('#content').includes('Layout'));
      await shot(`${layout}-settings-midnight`);
    }

    // The dashboard: its tables, a row in the detail panel, the notifications.
    T().savePrefs({ theme: 'midnight', layout: 'dashboard' });
    await home();
    $('#content').scrollTop = $('.home .tables').offsetTop - 16;
    await shot('dashboard-home-tables-midnight');
    const tab = (name) => [...document.querySelectorAll('.home [role="tab"]')].find((b) => b.textContent.includes(name));
    tab('Sidechains').click();
    const thunder = await waitFor('the Thunder row', () => [...document.querySelectorAll('.home .row-name')].find((b) => b.textContent.includes('Thunder')));
    thunder.click();
    await waitFor('the sidechain panel', () => $('.detail-panel') && text('.detail-panel').includes('Release hash'));
    await shot('dashboard-sidechain-panel-midnight');
    T().go('home');
    await sleep(300);
    $('.notices .bell').click();
    await waitFor('the notifications', () => $('.notices [role="dialog"]'));
    await shot('dashboard-notifications-midnight');
    $('.notices .bell').click();
    // Back to the transactions at the top of the home (it stays drawn: no other layout in between).
    tab('Transactions').click();
    $('#content').scrollTop = 0;

    // Every other look, on the dashboard; and the light one's Send panel.
    for (const theme of ['light', 'dark', 'ocean', 'matrix', 'qdarkstyle', 'mocha', 'dracula']) {
      T().savePrefs({ theme, layout: 'dashboard' });
      await home();
      await shot(`dashboard-home-${theme}`);
    }
    T().savePrefs({ theme: 'light', layout: 'dashboard' });
    await home();
    [...document.querySelectorAll('.home .stats button')].find((b) => b.textContent.trim() === 'Send').click();
    await waitFor('the side panel', () => $('.quick-panel'));
    await shot('dashboard-send-light');
    // The sun and moon of the top bar.
    $('.theme-toggle').click();
    await sleep(200);
    await check('the moon goes back to Midnight', T().prefs.theme === 'midnight', T().prefs.theme);
    await finish();
  } catch (e) {
    await snapshot('refresh-failure').catch(() => undefined);
    await finish(e);
  }
})();
