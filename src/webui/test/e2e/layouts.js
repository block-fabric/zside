// The two layouts of the main window (doc/webui-layouts.md), driven through the page: a wallet
// with a history, two sidechains, then each layout in the light and dark themes with pictures of
// its home, the send page, the history with a transaction in the detail panel (or its dialog),
// the sidechains, the settings and the command palette.
(async () => {
  const { rpc, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  const $ = (selector, root = document) => root.querySelector(selector);
  const typeInto = (input, value) => {
    input.value = value;
    input.dispatchEvent(new Event('input', { bubbles: true }));
  };
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);

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
    const active = await rpc('listactivesidechains');
    await check('two sidechains active', active.length === 2, JSON.stringify(active.map((s) => s.title)));
    await waitFor('the balance', () => T().wallets.balances.demo && T().wallets.balances.demo.trusted > 0n);

    const shot = async (name) => {
      await sleep(400);
      await snapshot(name);
    };

    for (const layout of ['workspace', 'dashboard']) {
      for (const theme of ['light', 'dark']) {
        T().savePrefs({ theme, layout });
        await sleep(200);

        // Home, in every layout: its live cards.
        T().go('home');
        await waitFor('the home', () => text('.home').includes('Thunder'), 20000);
        if (layout === 'workspace') {
          const current = $('.workspace .areas [aria-current="true"]');
          await check(`${layout} ${theme}: Home is the first area, and the current one`, $('.workspace .areas .area') === current && current.textContent.includes('Home'), current ? current.textContent : 'none');
        }
        await shot(`${layout}-home-${theme}`);

        T().go('send');
        await waitFor('the send page', () => $('#content .page'));
        await shot(`${layout}-send-${theme}`);

        // The history, a transaction selected: the panel in the workspace, the dialog otherwise.
        T().go('transactions');
        const row = await waitFor('a transaction row', () => $('.transactions [role="row"][aria-rowindex="2"]'), 20000);
        row.dispatchEvent(new MouseEvent('click', { bubbles: true }));
        if (layout === 'workspace') {
          const panel = await waitFor('the detail panel', () => $('.detail-panel'));
          await waitFor('the details in it', () => panel.textContent.includes('Transaction ID'));
          await check(`${layout} ${theme}: a transaction in the panel`, true);
        } else {
          row.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
          await waitFor('the details dialog', () => $('dialog[open]') && $('dialog[open]').textContent.includes('Transaction ID'));
        }
        await shot(`${layout}-transactions-${theme}`);
        if (layout !== 'workspace') $('dialog[open] button[aria-label="Close"]').click();

        // A sidechain: the page, and on the dashboard its card's row in the panel.
        T().go('sidechains');
        await waitFor('the sidechains page', () => $('#content .page'));
        await shot(`${layout}-sidechains-${theme}`);
        if (layout === 'dashboard') {
          T().go('home');
          const tab = (name) => [...document.querySelectorAll('.home [role="tab"]')].find((b) => b.textContent.includes(name));
          (await waitFor('the Sidechains tab', () => tab('Sidechains'))).click();
          const thunder = await waitFor('the Thunder row', () => [...document.querySelectorAll('.home .row-name')].find((b) => b.textContent.includes('Thunder')));
          thunder.click();
          await waitFor('the sidechain panel', () => $('.detail-panel') && text('.detail-panel').includes('Release hash'));
          await shot(`${layout}-sidechain-panel-${theme}`);
          tab('Blocks').click();
          const block = await waitFor('a block row', () => $('.home table[aria-label="Latest blocks"] .row-name'));
          block.click();
          await waitFor('the block panel', () => text('.detail-panel').includes('Confirmations'));
          await shot(`${layout}-block-panel-${theme}`);
        }

        T().go('settings', 'display');
        await waitFor('the settings', () => text('#content').includes('Layout'));
        await shot(`${layout}-settings-${theme}`);

        // The command palette, with something typed.
        window.dispatchEvent(new KeyboardEvent('keydown', { key: 'k', ctrlKey: true, bubbles: true }));
        const input = await waitFor('the palette', () => $('dialog.palette[open] input'));
        await shot(`${layout}-palette-${theme}`);
        typeInto(input, 'psbt');
        await waitFor('results', () => text('#palette-list').includes('Load PSBT from clipboard'));
        await shot(`${layout}-palette-search-${theme}`);
        input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
        $('dialog.palette').dispatchEvent(new Event('cancel', { cancelable: true }));
        await waitFor('the palette closed', () => !$('dialog.palette[open]'));
      }
    }

    // The menus of chains-qt: in the workspace's More menu, in the dashboard sidebar's Menus.
    T().savePrefs({ theme: 'light', layout: 'workspace' });
    await sleep(300);
    T().go('overview');
    await sleep(300);
    $('.workspace .top button[aria-label^="Menus of chains-qt"]').click();
    await waitFor('the more menu', () => $('[role="menu"]'));
    await shot('workspace-more-menu-light');
    document.body.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    // The dashboard's sidebar has no Menus button (user decision): the palette reaches every item.
    T().savePrefs({ layout: 'dashboard', theme: 'dark' });
    await sleep(300);
    if ($('.sidebar button[aria-label^="Menus of chains-qt"]')) throw new Error('the dashboard sidebar still has a Menus button');
    // Every other theme, on the dashboard.
    for (const theme of ['midnight', 'ocean', 'matrix', 'qdarkstyle', 'mocha', 'dracula']) {
      T().savePrefs({ theme, layout: 'dashboard' });
      T().go('home');
      await sleep(300);
      await shot(`dashboard-home-${theme}`);
    }
    T().savePrefs({ theme: 'light', layout: 'dashboard' });
    T().go('home');
    await finish();
  } catch (e) {
    await snapshot('layouts-failure').catch(() => undefined);
    await finish(e);
  }
})();
