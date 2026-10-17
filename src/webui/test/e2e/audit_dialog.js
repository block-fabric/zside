// The confirmations of the host in its dialog window (audit port-1 #4, src/webui/dialog.h): a window of its own with a
// web view of its own, in the look of the app (Midnight, Light, Dracula, a theme of the user), which the page can neither
// draw, nor reach, nor answer. The test answers it as a user would, in that window (test.dialog: keys and clicks in its
// page), never through the page; the dialog of the system when the window cannot be made (the fallback); one at a time.
// Audit 7: the spends with what the host decoded (#1), the arming of the button that accepts (#7), the limits (#8).
(async () => {
  const { host, rpc, sleep, T, waitFor, check, snapshot, finish } = E2E;
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
  const key = (k, repeat = false) => dialog('eval', { script: `document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, repeat: ${repeat}, bubbles: true, cancelable: true })); true` });
  // The button that accepts takes a click a second after the window shows with the focus (audit 7 #7): waited for.
  const ARMED = 2600;
  const refusedCode = (p) => p.then(() => 0, (e) => (e && e.code) || -1);
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
      // Each answered on purpose, declines among them: the limits of the questions start again (they have a check of their own).
      await host('test.questionLimits');
      await host('prefs.set', look.prefs);
      await host('test.confirmAnswer', { show: true });
      const pending = run('setnetworkactive true');
      const s = await shown(look.name);
      await check(`#4 ${look.name}: the question of the host in its window, as an alert dialog`,
        s.title === 'Run a command of the console' && s.message[0] === 'Command: setnetworkactive' && s.accept === 'Run' && s.cancel === 'Cancel' && s.role === 'alertdialog', JSON.stringify(s));
      if (look.name === 'midnight') {
        // Audit 7 #3: the parameters it runs with, one a row, and the whole line.
        const rows = await dialog('eval', { script: `[...document.querySelectorAll('#dialog-rows .row')].map((r) => r.textContent)` });
        const details = await dialog('eval', { script: `document.getElementById('dialog-details') && document.getElementById('dialog-details').textContent` });
        await check('audit 7 #3 the console question shows each parameter and the whole line', Array.isArray(rows) && rows.some((r) => r.includes('state') && r.includes('true')) && details === 'setnetworkactive true', JSON.stringify({ rows, details }));
        // Audit 7 #7: while a question of the host shows, the page neither hides nor moves the window.
        await check('audit 7 #7 the page cannot minimize, maximize, close or move the window under a question',
          (await Promise.all(['window.minimize', 'window.toggleMaximize', 'window.close', 'window.startMove'].map((m) => refusedCode(host(m))))).every((c) => c === -32800));
      }
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
        // Audit 7 #7: a press that began before, released after the delay, does not accept; nor does a key held down.
        await dialog('eval', { script: `document.getElementById('dialog-accept').dispatchEvent(new MouseEvent('mousedown', { bubbles: true })); true` });
        await sleep(ARMED);
        await dialog('eval', { script: `document.getElementById('dialog-accept').dispatchEvent(new MouseEvent('click', { bubbles: true, detail: 1 })); true` });
        await sleep(200);
        await check('audit 7 #7 a press that began before the delay does not accept', (await kind()) === 'themed');
        await dialog('eval', { script: `document.getElementById('dialog-accept').focus(); true` });
        await key('Enter', true);
        await sleep(200);
        await check('audit 7 #7 a key repeated by the keyboard does not accept', (await kind()) === 'themed');
        await click('dialog-accept');
      }
      const result = await pending;
      if (look.answer === 'accept') await check(`#4 ${look.name}: accepted in the window, the command runs`, result.ok, JSON.stringify(result));
      else await check(`#4 ${look.name}: declined in the window (${look.answer}), the command does not run`, !result.ok && /setnetworkactive was not run: it was declined/.test(result.e), JSON.stringify(result));
      await waitFor(`${look.name}: the window gone`, async () => (await kind()) === 'none', 10000);
    }
    await host('prefs.set', { theme: 'midnight', fontAdjust: 0 });
    await host('test.questionLimits');

    // One at a time: two questions at once, the second shows when the first is answered. (Two of one kind are not: the
    // second is refused at once, audit 7 #8.)
    await host('test.confirmAnswer', { show: true });
    await host('test.confirmAnswer', { show: true });
    const first = run('setnetworkactive true');
    await shown('the first of two');
    const sameKind = await run('setnetworkactive false');
    await check('audit 7 #8 a second question of the same kind while one waits is refused at once', !sameKind.ok && /declined/.test(sameKind.e), JSON.stringify(sameKind));
    const second = host('app.open', { what: 'url', url: 'https://second.example/' }).then((r) => ({ ok: r === true }), (e) => ({ ok: false, e: e && e.message }));
    await sleep(1500);
    await check('#4 two questions: one window at a time', (await kind()) === 'themed');
    await key('Escape');
    const firstResult = await first;
    await shown('the second of two');
    await check('#4 two questions: the second shows once the first is answered', !firstResult.ok, JSON.stringify(firstResult));
    await click('dialog-cancel');
    const secondResult = await second;
    await check('#4 two questions: both answered, in order', !secondResult.ok, JSON.stringify(secondResult));
    await host('test.questionLimits');

    // Audit 7 #8: a question whose request the page cancelled is withdrawn while it waits its turn.
    await host('test.confirmAnswer', { show: true });
    await host('test.confirmAnswer', { show: true });
    const third = run('setnetworkactive true');
    await shown('the first, before one withdrawn');
    const withdrawn = window.__chains({ rid: 'withdrawn-1', kind: 'host', method: 'app.open', params: { what: 'url', url: 'https://withdrawn.example/' } }).then(() => 'answered', (e) => e && e.code);
    await sleep(500);
    await window.__chains({ kind: 'cancel', rid: 'withdrawn-1' });
    await sleep(500);
    await key('Escape');
    await third;
    await sleep(1500);
    await check('audit 7 #8 a question the page cancelled is never shown', (await kind()) === 'none' && (await withdrawn) === -32800);
    await host('test.confirmAnswer', { clear: true });
    await host('test.questionLimits');

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
    await check('#4 every question kept for the tests', asked.length >= looks.length + 5 && asked.some((d) => d.title === 'Open a web address'), JSON.stringify(asked.map((d) => d.title)));

    // --- Audit 7 #8: a question declined again and again is no longer asked ---
    await host('test.questionLimits');
    for (let i = 0; i < 3; i++) {
      await host('test.confirmAnswer', { show: true });
      const pending = run('setnetworkactive true');
      const s = await shown(`the question, declined ${i + 1}`);
      if (i === 2) await check('audit 7 #8 the third time, the dialog says it will stop asking', s.message[0].startsWith('This was asked and declined 2 times in the last minute'), JSON.stringify(s.message));
      await key('Escape');
      await pending;
      await waitFor('the window gone', async () => (await kind()) === 'none', 10000);
    }
    await host('test.confirmAnswer', { show: true });
    const fourth = await run('setnetworkactive true');
    await check('audit 7 #8 after three noes in a minute, the kind is refused without a window', !fourth.ok && (await kind()) === 'none', JSON.stringify(fourth));
    const refusedKinds = await host('test.questionLimits');
    await check('audit 7 #8 the kind is refused for the session', refusedKinds.console === 'refused', JSON.stringify(refusedKinds));
    await host('test.confirmAnswer', { clear: true });

    // --- Audit 7 #1: spends, with what the host decoded; pictures in Midnight and Light ---
    await rpc('createwallet', ['spender', false, false, 'spend pass']);
    await waitFor('the wallet', () => T().wallets.list.some((w) => w.name === 'spender'), 30000);
    const own = await rpc('getnewaddress', ['', 'bech32'], 'spender');
    await rpc('generatetoaddress', [101, own]);
    await waitFor('the coins', () => T().wallets.balances.spender && T().wallets.balances.spender.trusted > 0n, 60000);
    await rpc('createwallet', ['payee']);
    const exchange = await rpc('getnewaddress', [], 'payee');
    const rent = await rpc('getnewaddress', [], 'payee');
    const readRows = `[...document.querySelectorAll('#dialog-rows > *')].map((r) => [r.querySelector('dt') ? r.querySelector('dt').textContent : r.textContent, r.querySelector('dd') ? r.querySelector('dd').textContent : '', r.className])`;
    for (const theme of ['midnight', 'light']) {
      await host('prefs.set', { theme });
      // A send to two recipients, from a locked wallet: the passphrase in the same window.
      const prepared = await host('wallet.send.prepare', { wallet: 'spender', recipients: [{ address: exchange, amount: 125000000, label: 'Exchange\n\n\n(fake)' }, { address: rent, amount: 40000000, label: 'Rent' }], feeRate: 2000 });
      await host('test.confirmAnswer', { show: true });
      const commit = host('wallet.send.commit', { wallet: 'spender', token: prepared.token }).then((r) => r, (e) => ({ error: e && e.code }));
      const s = await shown(`the send, ${theme}`);
      const rows = await dialog('eval', { script: readRows });
      const labels = rows.map((r) => r[0]);
      await check(`audit 7 #1 send (${theme}): each address whole before its amount and its label, the fee, the total`,
        s.title === 'Send coins' && labels.indexOf('Pay to') < labels.indexOf('Amount') && labels.indexOf('Amount') < labels.indexOf('Label') &&
          rows.some((r) => r[0] === 'Pay to' && r[1] === exchange) && rows.some((r) => r[0] === 'Pay to' && r[1] === rent) &&
          rows.some((r) => r[0] === 'Label' && r[1] === '\u201cExchange (fake)\u201d') && labels.includes('Network fee') && labels.includes('Total') && labels.includes('Recipient 2 of 2'),
        JSON.stringify(rows));
      const field = await dialog('eval', { script: `!!document.getElementById('dialog-passphrase')` });
      await check(`audit 7 #1 send (${theme}): the passphrase of the locked wallet in the window of the host`, field === true);
      await dialog('snapshot', { name: `spend-send-2-recipients-${theme}` });
      if (theme === 'midnight') {
        await click('dialog-cancel');
        await check('audit 7 #1 send declined in the window: nothing sent', (await commit).error === -32800 && (await rpc('getrawmempool', [])).length === 0);
      } else {
        // Typed in the window, accepted there: sent, and the wallet locked again.
        await dialog('eval', { script: `const f = document.getElementById('dialog-passphrase'); f.value = 'spend pass'; true` });
        await sleep(ARMED);
        await click('dialog-accept');
        const sent = await commit;
        await check('audit 7 #1 send accepted in the window with the passphrase: sent, the wallet locked again', sent && sent.complete && /^[0-9a-f]{64}$/.test(sent.txid) && (await rpc('getwalletinfo', [], 'spender')).unlocked_until === 0, JSON.stringify(sent));
      }
      await waitFor('the window gone', async () => (await kind()) === 'none', 10000);

      // A PSBT to sign: what it spends of the wallet and what it pays.
      const draft = await host('wallet.send.prepare', { wallet: 'spender', recipients: [{ address: exchange, amount: 30000000 }], feeRate: 2000 });
      const psbt = (await host('wallet.send.psbt', { wallet: 'spender', token: draft.token })).psbt;
      await host('test.confirmAnswer', { show: true });
      const signing = host('wallet.psbt.sign', { wallet: 'spender', psbt }).then((r) => r, (e) => ({ error: e && e.code }));
      const p = await shown(`the PSBT, ${theme}`);
      const prows = await dialog('eval', { script: readRows });
      await check(`audit 7 #1 PSBT (${theme}): what it spends of the wallet, what it pays, the fee`,
        p.title === 'Sign a partially signed transaction' && prows.some((r) => r[0] === 'Spends of this wallet') && prows.some((r) => r[0] === 'Pay to' && r[1] === exchange) && prows.some((r) => r[0] === 'Network fee'),
        JSON.stringify(prows));
      await dialog('snapshot', { name: `spend-psbt-${theme}` });
      await click('dialog-cancel');
      await check(`audit 7 #1 PSBT (${theme}): declined, not signed`, (await signing).error === -32800);
      await waitFor('the window gone', async () => (await kind()) === 'none', 10000);
    }
    await host('prefs.set', { theme: 'midnight' });
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
