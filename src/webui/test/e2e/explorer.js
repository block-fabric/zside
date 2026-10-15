// The block explorer's ribbon (components/BlockRibbon.svelte): a few blocks on regtest, one with
// transactions, then the explorer in Midnight and Light (dashboard and workspace): the boxes (no box
// for the block being mined), a box chosen and its details, a new block sliding in.
(async () => {
  const { rpc, sleep, T, waitFor, check, snapshot, finish } = E2E;
  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    await rpc('createwallet', ['demo']);
    await waitFor('the wallet', () => T().wallets.current === 'demo');
    const mining = await rpc('getnewaddress', ['Mining'], 'demo');
    await rpc('generatetoaddress', [110, mining]);
    // A block with transactions (and their fees), then blocks at different paces.
    for (let i = 0; i < 12; i++) {
      const to = await rpc('getnewaddress', [`Payee ${i}`], 'demo');
      await rpc('sendtoaddress', [to, 1 + i], 'demo');
    }
    await rpc('generatetoaddress', [1, mining]);
    // A heavier block: transactions of a hundred outputs each.
    const many = [];
    for (let i = 0; i < 100; i++) many.push(await rpc('getnewaddress', [], 'demo'));
    for (let n = 0; n < 10; n++) {
      const outputs = {};
      for (const a of many) outputs[a] = 0.01;
      await rpc('sendmany', ['', outputs], 'demo');
    }
    await sleep(1500);
    await rpc('generatetoaddress', [1, mining]);
    for (const pause of [1200, 200, 2500]) {
      await sleep(pause);
      await rpc('generatetoaddress', [1, mining]);
    }
    // Some waiting for the next block.
    for (let i = 0; i < 3; i++) await rpc('sendtoaddress', [await rpc('getnewaddress', [], 'demo'), 0.5], 'demo');
    const tip = await rpc('getblockcount');

    for (const theme of ['midnight', 'light']) {
      T().savePrefs({ theme, layout: 'dashboard' });
      T().go('explorer');
      await waitFor('the ribbon', () => $$('.ribbon button.blk').length >= 6 && $('.ribbon .blk .reward .amount'), 30000);
      await check(`${theme}: no box for the block being mined (the mempool is its own tab)`, !$('.blk-next'));
      const boxes = $$('.ribbon button.blk');
      const heights = boxes.map((b) => Number($('.height', b).textContent.replace(/\D/g, '')));
      await check(`${theme}: the boxes newest first`, heights[0] === tip && heights.every((h, i) => i === 0 || h === heights[i - 1] - 1), heights.join(','));
      const full = boxes.find((b) => Number($('.height', b).textContent.replace(/\D/g, '')) === tip - 3);
      const fill = full ? parseFloat($('.blk-face', full).style.getPropertyValue('--fill')) : 0;
      await check(`${theme}: the block with transactions is filled, tinted`, fill > 0 && $('.blk-face', full).dataset.tint !== 'none', `${fill} ${full && $('.blk-face', full).dataset.tint}`);
      const strip = $('.ribbon .strip');
      await check(`${theme}: the ribbon scrolls on its own, not the page`, document.documentElement.scrollWidth <= window.innerWidth && getComputedStyle(strip).overflowX === 'auto');
      await sleep(600);
      await snapshot(`explorer-ribbon-${theme}`);
      // A box chosen: its details.
      full.click();
      await waitFor('the block shown', () => $('#explorer-title') && $('#explorer-title').textContent.includes(String(tip - 3)), 20000);
      await check(`${theme}: a box chosen shows its block`, full.getAttribute('aria-pressed') === 'true');
      await sleep(500);
      await snapshot(`explorer-block-${theme}`);
      $('#content').scrollTop = 0;
    }

    // A new block while the page is shown: it slides in.
    await rpc('generatetoaddress', [1, mining]);
    await waitFor('the new block in the ribbon', () => {
      const first = $('.ribbon button.blk');
      return first && Number($('.height', first).textContent.replace(/\D/g, '')) === tip + 1;
    }, 30000);
    await check('a new block slides in', !!$('.ribbon button.blk.is-fresh'));
    await snapshot('explorer-new-block-light');

    // The workspace: the box chosen goes in the detail panel.
    T().savePrefs({ theme: 'midnight', layout: 'workspace' });
    await waitFor('the workspace', () => $('.workspace'), 10000);
    T().go('explorer');
    await waitFor('the ribbon', () => $$('.workspace .ribbon button.blk').length >= 6, 30000);
    await sleep(800);
    $$('.workspace .ribbon button.blk')[1].click();
    await waitFor('the detail panel', () => $('.detail-panel') && $('.detail-panel').textContent.includes('Confirmations'), 20000);
    await sleep(500);
    await snapshot('explorer-workspace-midnight');
    T().savePrefs({ theme: 'midnight', layout: 'dashboard' });
    await finish();
  } catch (e) {
    await snapshot('explorer-failure').catch(() => undefined);
    await finish(e instanceof Error ? `${e.message} ${e.stack}` : JSON.stringify(e));
  }
})();
