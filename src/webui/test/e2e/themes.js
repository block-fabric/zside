// The Catppuccin Mocha and Dracula themes on the main pages, and Settings → Advanced theme: a theme
// made from Mocha (drawn while it is edited, gone on Cancel), saved, exported to a file and imported back
// through the file chooser (answered by test.fileAnswer), a pasted theme that is refused, the contrast
// warning, and a theme deleted after the confirmation of the app. Pictures of each step.
(async () => {
  const { host, rpc, sleep, T, waitFor, check, snapshot, finish } = E2E;
  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim() === label || b.getAttribute('aria-label') === label);
  const token = (name) => getComputedStyle(document.documentElement).getPropertyValue(`--${name}`).trim();
  const bodyBg = () => getComputedStyle(document.body).backgroundColor;
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    await rpc('createwallet', ['demo']);
    await waitFor('the wallet', () => T().wallets.current === 'demo');
    const mining = await rpc('getnewaddress', ['Mining'], 'demo');
    await rpc('generatetoaddress', [110, mining]);
    for (const [label, amount] of [['Coffee shop', 0.42], ['Rent', 12.5]]) {
      await rpc('sendtoaddress', [await rpc('getnewaddress', [label], 'demo'), amount], 'demo');
    }
    await rpc('generatetoaddress', [1, mining]);
    await waitFor('the balance', () => T().wallets.balances.demo && T().wallets.balances.demo.trusted > 0n);

    // The two new looks, everywhere a look can be chosen.
    // The theme list of the sidebar (components/ThemePicker.svelte): it opens upwards, inside the window.
    const look = $('.foot button[aria-haspopup="listbox"]');
    look.click();
    const listbox = await waitFor('the theme list', () => $('[role="listbox"][aria-label="Theme"]'));
    const names = $$('[role="option"]', listbox).map((o) => o.textContent.trim());
    const box = listbox.getBoundingClientRect();
    await check('the sidebar lists Catppuccin Mocha and Dracula', ['Catppuccin Mocha', 'Dracula'].every((n) => names.includes(n)), names.join(','));
    await check('the theme list opens above its button, inside the window', box.top >= 0 && box.bottom <= look.getBoundingClientRect().top && box.bottom <= window.innerHeight, `${Math.round(box.top)}..${Math.round(box.bottom)} button ${Math.round(look.getBoundingClientRect().top)} window ${window.innerHeight}`);
    await snapshot('theme-list-open');
    look.click();
    const palette = T().buildCommands().map((c) => c.label);
    await check('the palette lists them', palette.includes('Theme: Catppuccin Mocha') && palette.includes('Theme: Dracula') && palette.includes('Advanced theme…'), palette.filter((l) => l.startsWith('Theme')).join(', '));

    for (const [theme, base, accent] of [['mocha', '#181825', '#cba6f7'], ['dracula', '#070708', '#d7263d']]) {
      T().savePrefs({ theme, layout: 'dashboard' });
      T().go('home');
      await waitFor(`${theme}: Home`, () => { const h = $('.home'); return h && h.textContent.includes('CHN') && h.querySelector('table tbody tr'); }, 30000);
      await sleep(800);
      await check(`${theme}: drawn in its colours`, token('bg') === base && token('accent') === accent && document.documentElement.style.colorScheme === 'dark', `${token('bg')} ${token('accent')} ${bodyBg()}`);
      await snapshot(`theme-${theme}-home`);
      for (const [page, tab] of [['send', ''], ['transactions', ''], ['sidechains', ''], ['node', 'peers'], ['settings', 'display']]) {
        T().go(page, tab);
        await sleep(900);
        await snapshot(`theme-${theme}-${page}`);
      }
      T().savePrefs({ layout: 'workspace' });
      T().go('home');
      await sleep(900);
      await snapshot(`theme-${theme}-workspace`);
      T().savePrefs({ layout: 'dashboard' });
      T().nav.dialog = 'about';
      await sleep(400);
      await snapshot(`theme-${theme}-about`);
      T().nav.dialog = null;
    }

    // Settings → Advanced theme, from Mocha.
    T().savePrefs({ theme: 'mocha', layout: 'dashboard' });
    T().go('settings', 'theme');
    await waitFor('the Advanced theme tab', () => $('#theme-start'));
    await sleep(500);
    await snapshot('advanced-theme-empty');
    set($('#theme-start'), 'mocha');
    button('New theme').click();
    const editor = await waitFor('the editor', () => $('section[aria-label="Theme editor"]'));
    set($('#tok-accent'), '#fab387');
    await waitFor('the accent drawn while editing', () => token('accent') === '#fab387');
    await check('the theme is drawn while it is edited', token('accent') === '#fab387', token('accent'));
    await sleep(400);
    await snapshot('advanced-theme-editor');
    editor.scrollIntoView({ block: 'end' });
    await sleep(300);
    await snapshot('advanced-theme-editor-end');
    button('Cancel', editor).click();
    await waitFor('the editor closed', () => !$('section[aria-label="Theme editor"]'));
    await check('Cancel puts the look back', token('accent') === '#cba6f7', token('accent'));

    // Made again, named, saved, applied.
    button('New theme').click();
    await waitFor('the editor', () => $('section[aria-label="Theme editor"]'));
    set($('#theme-name'), 'Peach Mocha');
    set($('#tok-accent'), '#fab387');
    set($('#tok-accent-fg'), '#fab387');
    set($('#tok-accent-soft'), '#3e3238');
    set($('#tok-ambient'), 'rgb(250 179 135 / 12%)');
    button('Save theme').click();
    await waitFor('the theme saved', () => T().prefs.customThemes.length === 1);
    const id = T().prefs.customThemes[0].id;
    await check('saved and chosen', T().prefs.theme === id && token('accent') === '#fab387', `${T().prefs.theme} ${token('accent')}`);
    button('Apply', $('.apply-bar')).click();
    await sleep(500);
    await snapshot('advanced-theme-saved');

    // Exported to a file, imported back.
    const dir = T().app.info.datadir;
    const file = `${dir}/peach-mocha.json`;
    await host('test.fileAnswer', { path: file });
    button('Export to a file', $('ul[aria-label="Your themes"]')).click();
    await sleep(800);
    await host('test.fileAnswer', { path: file });
    button('Import file…').click();
    await waitFor('the theme imported', () => T().prefs.customThemes.length === 2, 15000);
    const [a, b] = T().prefs.customThemes;
    await check('the imported theme has the colours exported', b.name === 'Peach Mocha (2)' && b.base === 'mocha' && JSON.stringify(Object.entries(b.colors).sort()) === JSON.stringify(Object.entries(a.colors).sort()), JSON.stringify(b).slice(0, 200));

    // A pasted theme with a url(): refused, nothing saved.
    button('Paste theme…').click();
    const area = await waitFor('the paste box', () => $('#theme-paste'));
    set(area, JSON.stringify({ format: 'chains-app-theme', version: 1, name: 'Evil', colors: { bg: 'url(https://example.com/x.png)' } }));
    (await waitFor('the Import button', () => { const b = button('Import'); return b && !b.disabled ? b : null; })).click();
    await waitFor('the error', () => $('p.error[role="alert"]'));
    await check('a theme with a url() is refused', T().prefs.customThemes.length === 2 && !token('bg').includes('url'), $('p.error[role="alert"]').textContent);
    await sleep(300);
    await snapshot('advanced-theme-refused');
    button('Cancel', area.closest('.paste')).click();

    // Low contrast: a warning.
    button('Edit', $$('ul[aria-label="Your themes"] li')[1]).click();
    const editor2 = await waitFor('the editor', () => $('section[aria-label="Theme editor"]'));
    set($('#tok-text'), '#45475a');
    await waitFor('the warning', () => editor2.querySelector('.warnings'));
    await check('a contrast warning, Save still there', !!button('Save theme', editor2) && !button('Save theme', editor2).disabled);
    editor2.querySelector('.warnings').scrollIntoView({ block: 'center' });
    await sleep(400);
    await snapshot('advanced-theme-contrast-warning');
    button('Cancel', editor2).click();
    await waitFor('the editor closed', () => !$('section[aria-label="Theme editor"]'));

    // Deleted, after the confirmation of the app.
    button('Delete', $$('ul[aria-label="Your themes"] li')[1]).click();
    const dialog = await waitFor('the confirmation', () => $$('dialog[open]').find((d) => d.textContent.includes('Delete the theme')));
    await snapshot('advanced-theme-delete');
    button('Delete', dialog).click();
    await waitFor('the theme deleted', () => T().prefs.customThemes.length === 1);
    await check('one theme left, the chosen one', T().prefs.customThemes[0].id === id && T().prefs.theme === id);

    // The theme of the user in the sidebar, and in the Appearance tab.
    $('.foot button[aria-haspopup="listbox"]').click();
    const ownList = await waitFor('the theme list', () => $('[role="listbox"][aria-label="Theme"]'));
    await check('the sidebar lists the theme of the user', $$('[role="option"]', ownList).some((o) => o.textContent.trim() === 'Peach Mocha' && o.getAttribute('translate') === 'no'));
    $('.foot button[aria-haspopup="listbox"]').click();
    T().go('settings', 'display');
    await sleep(600);
    await snapshot('advanced-theme-appearance');
    T().go('home');
    await sleep(800);
    await snapshot('advanced-theme-home');

    T().savePrefs({ theme: 'midnight', customThemes: [] });
    await finish();
  } catch (e) {
    await snapshot('themes-failure').catch(() => undefined);
    await finish(e);
  }
})();
