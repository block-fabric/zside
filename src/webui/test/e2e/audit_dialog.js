// The confirmations of the host in its dialog window (audit port-1 #4, src/webui/dialog.h): a window of its own with a
// web view of its own, in the look of the app (Midnight, Light, Dracula, a theme of the user), which the page can neither
// draw, nor reach, nor answer. The test answers it as a user would, in that window (test.dialog: keys and clicks in its
// page), never through the page; the dialog of the system when the window cannot be made (the fallback); one at a time.
(async () => {
  const { host, sleep, T, waitFor, check, snapshot, finish } = E2E;
  const dialog = (action, more = {}) => host('test.dialog', { action, ...more });
  const kind = () => dialog('kind');
  // A command of the console that does more than read (it waits for the user's word): whether it ran, or its error.
  const run = (line) => host('console.run', { line }).then((r) => (r && r.ok ? { ok: true } : { ok: false, e: r && r.error }), (e) => ({ ok: false, e: e && e.message }));
  // What the dialog window shows, read in its page.
  const STATE = `(() => {
    const q = (s) => document.querySelector(s);
    const root = document.documentElement;
    const css = getComputedStyle(root);
    return {
      title: q('#dialog-title') && q('#dialog-title').textContent,
      message: [...document.querySelectorAll('.message p')].map((p) => p.textContent),
      accept: q('#dialog-accept') && q('#dialog-accept').textContent,
      cancel: q('#dialog-cancel') && q('#dialog-cancel').textContent,
      focused: document.activeElement && document.activeElement.id,
      role: q('#dialog') && q('#dialog').getAttribute('role'),
      theme: root.dataset.theme,
      choice: root.dataset.themeChoice,
      scheme: root.style.colorScheme,
      bg: css.getPropertyValue('--surface').trim(),
      accent: css.getPropertyValue('--accent').trim(),
      fontSize: root.style.fontSize,
      loading: document.body.classList.contains('loading'),
      bridge: typeof window.__chains,
      width: innerWidth,
      height: innerHeight,
    };
  })()`;
  const key = (k) => dialog('eval', { script: `document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true, cancelable: true })); true` });
  const click = (id) => dialog('eval', { script: `document.getElementById(${JSON.stringify(id)}).click(); true` });

  async function shown(what) {
    await waitFor(`${what}: the dialog window`, async () => (await kind()) === 'themed', 30000);
    return waitFor(`${what}: its page`, async () => {
      const s = await dialog('eval', { script: STATE });
      return s && !s.loading && s.title ? s : null;
    }, 30000);
  }

  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 300000);

    // The main page has no way to the dialog: no bridge of it, no method of the host that answers it.
    await check('#4 the main page has no bridge of the dialog window', typeof window.__chainsDialog === 'undefined');
    await check('#4 nothing shows before a question', (await kind()) === 'none');

    const custom = { id: 'custom-e2e', name: 'Rose', base: 'dracula', colors: { accent: '#b3125a', 'accent-fg': '#ff79c6', bg: '#120d12', surface: '#1d1420', 'surface-2': '#251a29' } };
    const looks = [
      { name: 'midnight', prefs: { theme: 'midnight' }, theme: 'midnight', scheme: 'dark', answer: 'escape' },
      { name: 'light', prefs: { theme: 'light', fontAdjust: 2 }, theme: 'light', scheme: 'light', answer: 'accept' },
      { name: 'dracula', prefs: { theme: 'dracula', fontAdjust: 0 }, theme: 'dracula', scheme: 'dark', answer: 'enter' },
      { name: 'custom', prefs: { theme: 'custom-e2e', customThemes: [custom] }, theme: 'dracula', scheme: 'dark', answer: 'cancel', accent: '#b3125a' },
    ];
    for (const look of looks) {
      await host('prefs.set', look.prefs);
      await host('test.confirmAnswer', { show: true });
      const pending = run('setnetworkactive true');
      const s = await shown(look.name);
      await check(`#4 ${look.name}: the question of the host in its window, as an alert dialog`,
        s.title === 'Run a command of the console' && s.message[0] === 'Command: setnetworkactive' && s.accept === 'Run' && s.cancel === 'Cancel' && s.role === 'alertdialog', JSON.stringify(s));
      await check(`#4 ${look.name}: drawn in the look of the app`,
        s.theme === look.theme && s.scheme === look.scheme && (!look.accent || s.accent === look.accent) && (look.name !== 'custom' || s.choice === 'custom'), JSON.stringify(s));
      await check(`#4 ${look.name}: the focus on Cancel, the safe default; no bridge of the main page in it`, s.focused === 'dialog-cancel' && s.bridge === 'undefined', JSON.stringify(s));
      if (look.name === 'light') await check('#4 light: the text size of the app', s.fontSize === '16px', s.fontSize);
      if (look.name === 'midnight') {
        // The answers of the page's own dialogs (dialog.answer, for the messages of the node) never answer it.
        const tries = await Promise.all([...Array(20).keys()].map((id) => host('dialog.answer', { id, answer: true }).catch(() => false)));
        await sleep(300);
        await check('#4 the page cannot answer the dialog window', tries.every((x) => x === false) && (await kind()) === 'themed', JSON.stringify(tries));
      }
      const shot = await dialog('snapshot', { name: `dialog-${look.name}` });
      await check(`#4 ${look.name}: a picture of the dialog window`, typeof shot === 'string' && shot.endsWith(`dialog-${look.name}.png`), JSON.stringify(shot));
      // Answered in the window, as a user would.
      if (look.answer === 'escape') await key('Escape');
      else if (look.answer === 'enter') await key('Enter');
      else if (look.answer === 'cancel') await click('dialog-cancel');
      else {
        // The button that accepts takes clicks only a moment after the window shows (a click meant for the main window).
        await click('dialog-accept');
        await sleep(100);
        await check('#4 light: a click right after the window shows does not accept', (await kind()) === 'themed');
        await sleep(600);
        await click('dialog-accept');
      }
      const result = await pending;
      if (look.answer === 'accept') await check(`#4 ${look.name}: accepted in the window, the command runs`, result.ok, JSON.stringify(result));
      else await check(`#4 ${look.name}: declined in the window (${look.answer}), the command does not run`, !result.ok && /setnetworkactive was not run: it was declined/.test(result.e), JSON.stringify(result));
      await waitFor(`${look.name}: the window gone`, async () => (await kind()) === 'none', 10000);
    }
    await host('prefs.set', { theme: 'midnight', fontAdjust: 0 });

    // One at a time: two questions at once, the second shows when the first is answered.
    await host('test.confirmAnswer', { show: true });
    await host('test.confirmAnswer', { show: true });
    const first = run('setnetworkactive true');
    await shown('the first of two');
    const second = run('setnetworkactive true');
    await sleep(1500);
    await check('#4 two questions: one window at a time', (await kind()) === 'themed');
    await key('Escape');
    const firstResult = await first;
    await shown('the second of two');
    await check('#4 two questions: the second shows once the first is answered', !firstResult.ok, JSON.stringify(firstResult));
    await click('dialog-cancel');
    const secondResult = await second;
    await check('#4 two questions: both answered, in order', !secondResult.ok && /declined/.test(secondResult.e), JSON.stringify(secondResult));

    // Without its window (its web view cannot be made): the dialog of the system, never an answer not given.
    await host('test.confirmAnswer', { fallback: true });
    const fallback = run('setnetworkactive true');
    await waitFor('the dialog of the system', async () => (await kind()) === 'native', 30000);
    const native = await dialog('native', { arg: 'state' });
    await check('#4 fallback: the same question in the dialog of the system', native && native.title === 'Run a command of the console' && native.message.startsWith('Command: setnetworkactive'), JSON.stringify(native));
    await dialog('native', { arg: 'cancel' });
    const fallbackResult = await fallback;
    await check('#4 fallback: declined there, the command does not run', !fallbackResult.ok && /declined/.test(fallbackResult.e), JSON.stringify(fallbackResult));
    await host('test.confirmAnswer', { fallback: true });
    const fallbackYes = run('setnetworkactive true');
    await waitFor('the dialog of the system again', async () => (await kind()) === 'native', 30000);
    await dialog('native', { arg: 'accept' });
    await check('#4 fallback: accepted there, it runs', (await fallbackYes).ok);

    // A question nobody expected is still declined at once in the tests (never a window on the tester's desktop).
    const unexpected = await run('setnetworkactive true');
    await check('#4 an unexpected question is declined', !unexpected.ok && (await kind()) === 'none', JSON.stringify(unexpected));
    const asked = await host('test.confirmed');
    await check('#4 every question kept for the tests', asked.length === looks.length + 5 && asked.every((d) => d.title === 'Run a command of the console'), JSON.stringify(asked.map((d) => d.title)));
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
