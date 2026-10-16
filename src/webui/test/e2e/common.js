// Helpers of the page scripts of the end-to-end tests of chains-app. The test runner joins this
// file with a script (smoke.js, intro.js) and gives it to the app with -webuitestscript; it runs in
// the page, with the test methods of the bridge (test.log, test.snapshot, test.exit).
const E2E = (() => {
  const failures = [];
  const passed = [];
  const shots = [];
  const host = (method, params = {}) => window.__chains({ kind: 'host', method, params });
  // The script sets the node up past the RPC policy of the pages ("testrpc", only with -webuitestscript in a
  // build with WEBUI_TEST_HOOKS); the pages keep to the policy that ships, and report() fails on a method it
  // refused them (audit port-1 #5). asPage() calls as a page does, for the checks of the policy itself.
  const rpc = (method, params = [], wallet) => window.__chains({ kind: 'testrpc', method, params, ...(wallet == null ? {} : { wallet }) });
  const expectedRefusals = new Set();
  const asPage = (method, params = [], refused = false) => {
    if (refused) expectedRefusals.add(method);
    return window.__chains({ kind: 'rpc', method, params });
  };
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const log = (message) => host('test.log', { message: String(message) });
  const T = () => window.__chainsTest;

  // The mode of the interface (Simple, the default since 2026-10-16, or Advanced): the scripts were
  // written for the full pages, Advanced; a script of Simple mode sets window.__E2E_SIMPLE in its prelude.
  // Set as soon as the page has its preferences, before the first wait of the script ends.
  const wantedMode = window.__E2E_SIMPLE ? 'simple' : 'advanced';
  const modeSet = (async () => {
    for (let i = 0; i < 1200; i++) {
      const t = window.__chainsTest;
      if (t && t.prefs && t.prefs.loaded) {
        if (t.prefs.uiMode !== wantedMode) t.savePrefs({ uiMode: wantedMode });
        return;
      }
      await new Promise((resolve) => setTimeout(resolve, 25));
    }
  })();

  async function waitFor(what, fn, ms = 30000) {
    await modeSet;
    const until = Date.now() + ms;
    let last;
    let noted = Date.now();
    while (Date.now() < until) {
      // A long wait says what it waits for: the runner hears the page is alive, and where it is.
      if (Date.now() - noted > 20000) {
        noted = Date.now();
        await log(`WAITING for ${what}`);
      }
      try {
        last = await fn();
        if (last) return last;
      } catch (e) {
        last = e;
      }
      await sleep(100);
    }
    throw new Error(`timed out waiting for ${what} (${last && last.message ? last.message : JSON.stringify(last)})`);
  }

  function check(name, ok, detail = '') {
    if (ok) passed.push(name);
    else failures.push(`${name}${detail ? ': ' + detail : ''}`);
    return log(`${ok ? 'PASS' : 'FAIL'} ${name}${!ok && detail ? ' (' + detail + ')' : ''}`);
  }

  async function snapshot(name) {
    // Let the page settle: fonts, the transition of a theme.
    await sleep(350);
    await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    await host('test.snapshot', { name });
    shots.push(name);
  }

  function text(selector) {
    const el = document.querySelector(selector);
    return el ? el.textContent : '';
  }

  function buttonByText(label, root = document) {
    return [...root.querySelectorAll('button')].find((b) => b.textContent.trim() === label);
  }

  /** Say how the test went (the runner reads it), without ending the app. */
  async function report() {
    const refused = (await host('test.refused').catch(() => [])).filter((m) => !expectedRefusals.has(m));
    if (refused.length) failures.push(`the pages called RPC methods their policy refuses (rpcpolicy.cpp): ${refused.join(', ')}`);
    else passed.push('the pages called no RPC method outside their policy');
    await log(`RESULT ${JSON.stringify({ passed: passed.length, failures, shots })}`);
  }

  async function finish(error) {
    if (error) failures.push(`script error: ${error}${error && error.stack ? ' at ' + error.stack.split('\n')[0] : ''}`);
    await report();
    await host('test.exit', { code: failures.length ? 1 : 0 });
  }

  // The runner hears from the page from the start: a page that says nothing for long is stuck.
  log('SCRIPT started');
  return { host, rpc, asPage, sleep, log, T, waitFor, modeSet, check, snapshot, text, buttonByText, report, finish };
})();
