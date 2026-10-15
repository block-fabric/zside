// A node without wallets (-disablewallet, audit C1): no wallet menus, and the wallet pages say why
// instead of aborting the app.
(async () => {
  const { host, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    await check('C1 the app knows there are no wallets', T().app.info.walletEnabled === false);
    await check('L11 a debug option set in chains.conf is ignored', T().app.info.devtools === false);
    // The menus of chains-qt: the sections of the workspace's Menus button (the dashboard's sidebar has
    // none, user decision); back to the dashboard after.
    T().savePrefs({ layout: 'workspace' });
    await sleep(400);
    const menusButton = document.querySelector('button[aria-label^="Menus of chains-qt"], button[title^="Menus of chains-qt"]');
    menusButton.click();
    await sleep(200);
    const menus = [...document.querySelectorAll('[role="menu"] > [role="group"]')].map((g) => g.getAttribute('aria-label'));
    menusButton.click();
    T().savePrefs({ layout: 'dashboard' });
    await sleep(400);
    await check('C1 the other menus are there', menus.includes('Chains') && menus.includes('Tools'), menus.join(', '));
    await check('C1 no wallet menus', !menus.includes('Your wallet') && !menus.includes('Banking'), menus.join(', '));
    // Every layout starts on Home: its balance card says why; then the overview.
    await check('C1 the home says why', T().nav.page !== 'home' || text('.content').includes('-disablewallet'), text('.content').slice(0, 120));
    T().go('overview');
    await sleep(400);
    await check('C1 the overview says why', text('.balances').includes('-disablewallet') && !text('.balances').includes('Create or open'));
    for (const page of ['wallets', 'send', 'receive', 'transactions', 'addresses', 'psbt', 'proof', 'multisig']) {
      T().go(page);
      await sleep(400);
      await check(`C1 the ${page} page says why`, text('.content').includes('-disablewallet'), text('.content').slice(0, 80));
    }
    T().go('wallets', 'create');
    await sleep(400);
    await snapshot('audit-nowallet-wallets-light');
    // The host answers its wallet functions with an error, never an abort.
    const error = await host('wallets.list').then(() => null, (e) => e);
    await check('C1 the wallet functions answer an error', error && error.code === -18, JSON.stringify(error));
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
