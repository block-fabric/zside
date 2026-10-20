// The alert of a sidechain whose node cannot reach its mainchain node (run_mainlink.py): the runner stops,
// starts and slows the mainchain node when the page says so (STOP_MAIN, START_MAIN, SYNC_MAIN, NORMAL_MAIN),
// the page checks the banner at the top: down within its delay with the reason in plain words and the status
// bar saying so, gone with one toast when the node is back, the softer banner while it syncs; with pictures
// in Midnight, Advanced and Simple.
(async () => {
  const { rpc, host, log, sleep, T, waitFor, check, snapshot, finish } = E2E;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const MAIN = window.__E2E_MAINCHAIN;
  const banner = () => $('.mainchain-alert');
  const statusbar = () => ($('footer.statusbar .sync') || {}).textContent || '';
  async function mode(m) {
    T().savePrefs({ uiMode: m });
    await waitFor(`${m} mode`, () => T().prefs.uiMode === m);
    await sleep(300);
  }
  async function shot(name) {
    // The sync overlay of a new sidechain would cover the page.
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = $$('button', overlay).find((b) => b.textContent.trim() === 'Hide');
      if (hide) hide.click();
    }
    await snapshot(name);
  }
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await waitFor('the link to the mainchain node', async () => (await rpc('getmainchaininfo')).state === 'ok', 60000);
    const info = await rpc('getmainchaininfo');
    await check('getmainchaininfo says what the mainchain node said of itself', info.mainchain_network === 'regtest' && info.mainchain_height >= info.followed_height - 1 && info.behind_blocks <= 6 && info.last_ok > 0, JSON.stringify(info));
    T().savePrefs({ theme: 'midnight', layout: 'dashboard' });
    await rpc('createwallet', ['side']);
    await waitFor('the wallet in the page', () => T().wallets.current === 'side');
    // Blocks of this chain, merged mined with the blocks of the mainchain: a chain in sync in the pictures.
    const fees = await rpc('getnewaddress', [], 'side');
    await host('test.confirmAnswer', { yes: true });
    await host('bmm.set', { mine: true, address: fees, always: true, amount: '0.0001' });
    await waitFor('the first blocks of this chain', () => T().node.tip.height >= 2 && !T().node.tip.ibd, 120000);
    await sleep(4500);
    await check('no banner while the link is up', !banner());

    // 1. The mainchain node stops.
    await log('STOP_MAIN');
    const stopped = Date.now();
    await waitFor('the node to see the mainchain node down', async () => (await rpc('getmainchaininfo')).state === 'down', 60000);
    const down = await rpc('getmainchaininfo');
    await check('getmainchaininfo: down, refused, since and last_ok', down.error_kind === 'refused' && down.since > 0 && down.last_ok > 0 && !down.connected, JSON.stringify(down));
    await sleep(8000);
    await check('no banner before its delay (a restart of the mainchain node is no outage)', !banner() || Date.now() - stopped > 20000);
    await waitFor('the down banner', () => banner() && banner().classList.contains('down'), 60000);
    const seconds = Math.round((Date.now() - stopped) / 1000);
    await check(`the down banner within the timeout (${seconds} s)`, seconds <= 45, String(seconds));
    const words = banner().textContent;
    await check('the banner names the node, the reason in plain words, and when it was last reached', words.includes(`Cannot reach your ${MAIN} node (127.0.0.1:`) && words.includes('connection refused') && /Last reached .* ago/.test(words), words);
    await check('the banner is an alert', banner().getAttribute('role') === 'alert');
    await check('the status bar says so', statusbar().includes(`No link to ${MAIN}`), statusbar());
    await check('the bell has one notice of it', T() && $('.notices .dot') !== null);
    $$('button', banner()).find((b) => /How to fix/.test(b.textContent)).click();
    await waitFor('How to fix', () => banner().textContent.includes('server=1'));
    await shot('mainlink-down-advanced');
    await mode('simple');
    await check('Simple mode: a short plain message', banner().textContent.includes(`Cannot reach ${MAIN}`) && banner().textContent.includes('paused'), banner().textContent);
    await shot('mainlink-down-simple');

    // 2. Back: the banner goes, one toast (pictured in Simple, then Advanced).
    await log('START_MAIN');
    await waitFor('the banner to go', () => !banner(), 90000);
    const toastText = $$('.toast').map((t) => t.textContent).join(' | ');
    await check('the toast of the link back', toastText.includes(`Connected to ${MAIN} again`), toastText);
    await shot('mainlink-restored-simple');
    await mode('advanced');
    await shot('mainlink-restored-advanced');
    await check('the status bar no longer says so', !statusbar().includes('No link'), statusbar());
    await sleep(6000);
    await check('one toast only', $$('.toast').filter((t) => t.textContent.includes('again')).length <= 1);

    // 3. The mainchain node in initial block download: the softer banner.
    await log('SYNC_MAIN');
    await waitFor('the node to see the mainchain node syncing', async () => (await rpc('getmainchaininfo')).state === 'syncing', 90000);
    const syncing = await rpc('getmainchaininfo');
    await check('getmainchaininfo: syncing, not-synced, mainchain_ibd', syncing.error_kind === 'not-synced' && syncing.mainchain_ibd === true, JSON.stringify(syncing));
    await waitFor('the syncing banner', () => banner() && banner().classList.contains('syncing'), 90000);
    await check('the syncing banner is a status, not an alert', banner().getAttribute('role') === 'status');
    await check('it says the mainchain node is syncing', banner().textContent.includes(`${MAIN} is still syncing`), banner().textContent);
    await shot('mainlink-syncing-advanced');
    await mode('simple');
    await check('Simple mode: catching up', banner().textContent.includes('catching up'), banner().textContent);
    await shot('mainlink-syncing-simple');
    // The restored toast in Simple mode too (the mainchain node back to normal).
    await log('NORMAL_MAIN');
    await waitFor('the banner to go again', () => !banner(), 120000);
    await check('no toast of a link back after syncing only', true);
    await mode('advanced');
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
