// Confirm before a quit (app.quitGuard, src/webui/quitguard.h): while the page keeps work a quit would interrupt, the
// window closed, Quit (app.quit: the menu, Ctrl+Q) ask in the dialog of the host, "Keep … open" the default; Keep leaves
// the app running; "Quit anyway" quits; a second close while the question shows quits. The template sets the text
// itself (window.__E2E_MESSAGE); a chain whose page sets it (Hivemind: a market still being created) runs it with
// window.__E2E_FROM_PAGE and the text expected in window.__E2E_EXPECT.
// window.__E2E_STEP: 'accept' (ends with Quit anyway), 'again' (ends with a second close).
(async () => {
  const { host, sleep, T, waitFor, check, report, finish } = E2E;
  const STEP = window.__E2E_STEP;
  const dialog = (action, more = {}) => host('test.dialog', { action, ...more });
  const kind = () => dialog('kind');
  const STATE = `(() => {
    const q = (s) => document.querySelector(s);
    return {
      title: q('#dialog-title') && q('#dialog-title').textContent,
      message: [...document.querySelectorAll('.message p')].map((p) => p.textContent).join('\\n'),
      accept: q('#dialog-accept') && q('#dialog-accept').textContent,
      cancel: q('#dialog-cancel') && q('#dialog-cancel').textContent,
      focused: document.activeElement && document.activeElement.id,
      loading: document.body.classList.contains('loading'),
    };
  })()`;
  const click = (id) => dialog('eval', { script: `document.getElementById(${JSON.stringify(id)}).click(); true` });
  async function shown(what) {
    await waitFor(`${what}: the dialog window`, async () => (await kind()) === 'themed', 30000);
    return waitFor(`${what}: its page`, async () => {
      const s = await dialog('eval', { script: STATE });
      return s && !s.loading && s.title ? s : null;
    }, 30000);
  }
  const alive = async () => (await host('app.hello')).phase === 'ready';
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready', 300000);
    T().savePrefs({ theme: 'midnight' });
    const name = (await host('app.info')).name;
    let expected = window.__E2E_EXPECT;
    const guard = async () => {
      if (window.__E2E_FROM_PAGE) return;
      await host('app.quitGuard', { message: window.__E2E_MESSAGE });
    };
    if (window.__E2E_FROM_PAGE) {
      await waitFor('the page to say a quit interrupts its work', () => T().quitGuard && T().quitGuard(), 180000);
      await check(`${STEP}: the page says what a quit interrupts`, T().quitGuard() === expected, String(T().quitGuard()));
    } else {
      expected = window.__E2E_MESSAGE;
    }
    await sleep(1500);

    // The window closed: asked, Keep the default (focused); Keep leaves the app running.
    await guard();
    await host('test.confirmAnswer', { show: true });
    await host('test.window', { action: 'close' });
    let s = await shown('close');
    await check(`${STEP}: closing the window asks in the dialog of the host`, s.title === `Quit ${name}?`, s.title);
    await check(`${STEP}: the dialog says what a quit interrupts`, s.message === expected, s.message);
    await check(`${STEP}: Quit anyway and Keep ${name} open`, s.accept === 'Quit anyway' && s.cancel === `Keep ${name} open`, `${s.accept} / ${s.cancel}`);
    await check(`${STEP}: Keep open is the default`, s.focused === 'dialog-cancel', s.focused);
    if (STEP === 'accept') await dialog('snapshot', { name: 'quit-dialog' });
    await click('dialog-cancel');
    await waitFor('the dialog to close', async () => (await kind()) === 'none', 20000);
    await sleep(1500);
    await check(`${STEP}: Keep open: the app runs on`, await alive());

    // Quit of the menu, Ctrl+Q (app.quit): asked the same way.
    await guard();
    await host('test.confirmAnswer', { show: true });
    await host('app.quit');
    s = await shown('app.quit');
    await check(`${STEP}: Quit of the menu asks too`, s.message === expected, s.message);
    if (STEP === 'accept') {
      // Quit anyway (armed a moment after the window shows): the app stops.
      await sleep(2600);
      await report();
      await click('dialog-accept');
    } else {
      // A second close while the question shows: the app stops, never held.
      await report();
      await host('test.window', { action: 'close' });
    }
    await sleep(120000);
    await check(`${STEP}: the app stopped`, false, 'still running');
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
