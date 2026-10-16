// Simple mode (user decision 2026-10-16), chosen by the user (a first start is in Advanced). A wallet with coins, two
// sidechains, then in the Midnight and Light themes: Home with its first steps, the sidebar without the
// pages of Advanced, Send, the history in plain words, and Move coins (the Sidechains page) through its
// four steps up to a real deposit; then the switch to Advanced and back, by the sidebar and the palette.
// The runner (run_simple.py) sets window.__E2E_SIMPLE, so that common.js keeps Simple mode.
(async () => {
  const { rpc, host, sleep, T, waitFor, check, snapshot, text, buttonByText, finish } = E2E;
  const $ = (selector, root = document) => root.querySelector(selector);
  const typeInto = (input, value) => {
    input.value = value;
    input.dispatchEvent(new Event('input', { bubbles: true }));
  };
  const shot = async (name) => {
    await sleep(400);
    await snapshot(name);
  };
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    await check('Advanced on a first start (user decision), Simple once chosen', window.__E2E_FIRST_MODE === 'advanced' && T().prefs.uiMode === 'simple', `${window.__E2E_FIRST_MODE} ${T().prefs.uiMode}`);

    await rpc('createwallet', ['demo']);
    await waitFor('the wallet', () => T().wallets.current === 'demo');
    const mining = await rpc('getnewaddress', ['Mining'], 'demo');
    await rpc('createsidechainproposal', [2, 'Thunder', 'Fast payments with a lightning-style sidechain']);
    await rpc('createsidechainproposal', [5, 'BitNames', 'Names and their records, on a sidechain']);
    await rpc('generatetoaddress', [130, mining]);
    for (const [label, amount] of [['Coffee shop', 0.42], ['Rent', 12.5]]) {
      const to = await rpc('getnewaddress', [label], 'demo');
      await rpc('sendtoaddress', [to, amount], 'demo');
    }
    await rpc('generatetoaddress', [1, mining]);
    const later = await rpc('getnewaddress', ['Bookshop'], 'demo');
    await rpc('sendtoaddress', [later, 0.15], 'demo');
    await waitFor('the balance', () => T().wallets.balances.demo && T().wallets.balances.demo.trusted > 0n);
    const active = await rpc('listactivesidechains');
    await check('two sidechains active', active.length === 2, JSON.stringify(active.map((s) => s.title)));

    // A deposit address of Thunder, as its app would give it: s<slot>_<address>_<checksum>.
    const prefix = 's2_tTHUNDERdemoAddress_';
    const checksum = (await host('tools.hashes', { text: prefix, hex: false })).sha256.slice(0, 6);
    const depositAddress = prefix + checksum;

    T().savePrefs({ layout: 'dashboard', tipsDone: false });
    for (const theme of ['midnight', 'light']) {
      T().savePrefs({ theme });
      T().go('home');
      await waitFor('Home, merged mining in a line', () => text('[data-testid="bmm-line"]').includes('of 2 active'), 20000);
      await waitFor('the first steps', () => $('.tips'), 10000);
      await check(`${theme}: the first steps on Home`, document.querySelectorAll('.tips li').length === 3);
      await check(`${theme}: no mining card in Simple mode`, !$('[data-testid="mining-rate"]'));
      const sidebar = text('nav.sidebar .scroll');
      await check(`${theme}: the sidebar without the pages of Advanced`, !sidebar.includes('Advanced transactions') && !sidebar.includes('Node window') && sidebar.includes('Send'), sidebar);
      await check(`${theme}: the switch in the sidebar's foot`, !!$('nav.sidebar .foot [role="radiogroup"]'));
      await shot(`simple-home-${theme}`);

      T().go('send');
      await waitFor('the send page', () => $('#content .page'));
      await shot(`simple-send-${theme}`);

      T().go('transactions');
      await waitFor('the history', () => $('.transactions [role="row"][aria-rowindex="2"]'), 20000);
      await shot(`simple-history-${theme}`);

      // Move coins: four steps.
      T().go('sidechains');
      const thunder = await waitFor('the sidechains to choose from', () => [...document.querySelectorAll('.move .choice')].find((b) => b.textContent.includes('Thunder')), 20000);
      await shot(`simple-move-1-${theme}`);
      thunder.click();
      const amount = await waitFor('the amount', () => $('#move-amount'));
      typeInto(amount, '1.25');
      await sleep(150);
      buttonByText('Next').click();
      const dest = await waitFor('the destination', () => $('#move-destination'));
      typeInto(dest, depositAddress);
      await waitFor('the address checked', () => text('#move-destination-check').includes('A deposit address of Thunder'), 10000);
      await shot(`simple-move-3-${theme}`);
      buttonByText('Next').click();
      await waitFor('the confirmation', () => text('.move').includes('Check, then move the coins.'));
      await shot(`simple-move-4-${theme}`);
      if (theme === 'midnight') {
        const before = (await rpc('listsidechaindeposits', [2])).length;
        const move = await waitFor('the Move button', () => [...document.querySelectorAll('.move button')].find((b) => b.textContent.trim().startsWith('Move 1.25')));
        move.click();
        await waitFor('the coins on their way', () => text('.move').includes('are on their way to'), 20000);
        await rpc('generatetoaddress', [1, mining]);
        const after = await rpc('listsidechaindeposits', [2]);
        await check('the deposit of Move coins is on the chain', after.length === before + 1 && after.some((d) => d.destination === depositAddress), JSON.stringify(after.slice(-1)));
        await shot(`simple-move-done-${theme}`);
        buttonByText('Move more coins').click();
      } else {
        buttonByText('Back').click();
      }

      // A word of the glossary, its explanation shown.
      const term = await waitFor('a word of the glossary', () => $('.move .term'));
      term.dispatchEvent(new PointerEvent('pointerenter', { bubbles: true }));
      await waitFor('its explanation', () => $('[role="tooltip"]'));
      await shot(`simple-term-${theme}`);
      term.dispatchEvent(new PointerEvent('pointerleave', { bubbles: true }));
    }

    // The switch: Advanced by the sidebar, Simple again by the palette.
    const advanced = [...document.querySelectorAll('nav.sidebar [role="radio"]')].find((b) => b.textContent.trim() === 'Advanced');
    advanced.click();
    await waitFor('Advanced mode', () => T().prefs.uiMode === 'advanced');
    T().go('sidechains');
    await waitFor('the full Sidechains page', () => document.querySelectorAll('.slot').length > 0, 20000);
    await check('Advanced: the slots and the proposals are back', text('nav.sidebar .scroll').includes('Advanced transactions') && !!buttonByText('Proposals'));
    await shot('advanced-sidechains-light');
    const saved = await host('prefs.get');
    await check('the mode is saved in the preferences', saved.uiMode === 'advanced', JSON.stringify(saved.uiMode));
    T().buildCommands().find((c) => c.id === 'display.uiMode').run();
    await waitFor('Simple mode again', () => T().prefs.uiMode === 'simple');
    await check('the palette switches back to Simple', true);
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
