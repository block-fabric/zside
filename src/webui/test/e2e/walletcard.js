// The card of the wallet at the top of the sidebar (dashboard, Midnight): the name of the wallet keeps
// room of its own next to the unit. On Windows at 125 % a name showed as "(" — cut to nothing by the unit
// pill. Checked with one wallet (its name) and two (the picker), a short name and a long one, the sidebar
// narrow and wide, the text larger (A+), and the page zoomed to 125 and 150 % (what a desktop scale does
// to the room text takes).
(async () => {
  const { rpc, sleep, log, T, waitFor, check, snapshot, finish } = E2E;
  const LONG = 'savings-for-the-whole-family-2026';
  /** What the field of the name shows, and whether it shows it whole. */
  function nameField() {
    const card = document.querySelector('.wallet-card');
    const el = card && card.querySelector('.wc-name .name, .wc-name select');
    // The unit is no longer in the card (it is chosen in the status bar): nothing to share the room with.
    const unit = card && card.querySelector('select.unit');
    if (!el) return null;
    const style = getComputedStyle(el);
    let text;
    let fits;
    let room;
    if (el.tagName === 'SELECT') {
      text = el.options[el.selectedIndex].text;
      const ctx = document.createElement('canvas').getContext('2d');
      ctx.font = `${style.fontWeight} ${style.fontSize} ${style.fontFamily}`;
      room = el.clientWidth - parseFloat(style.paddingLeft) - parseFloat(style.paddingRight);
      fits = ctx.measureText(text).width <= room + 0.5;
    } else {
      text = el.textContent;
      // The text as laid out against its box, both in the same units (scrollWidth rounds under a zoom).
      const range = document.createRange();
      range.selectNodeContents(el);
      const box = el.getBoundingClientRect().width;
      room = el.clientWidth;
      fits = range.getBoundingClientRect().width <= box + 0.5;
    }
    const a = el.getBoundingClientRect();
    const b = unit ? unit.getBoundingClientRect() : null;
    const apart = !b || a.right <= b.left + 0.5 || b.top >= a.bottom - 0.5;
    const em = parseFloat(getComputedStyle(document.documentElement).fontSize);
    return { el, text, fits, room, apart, title: el.title, ellipsis: style.textOverflow === 'ellipsis', roomEm: room / em };
  }

  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    T().nav.dialog = null;
    await rpc('createwallet', ['demo']);
    await waitFor('the wallet', () => T().wallets.current === 'demo');
    T().savePrefs({ theme: 'midnight', layout: 'dashboard', fontAdjust: 0 });
    T().go('home');
    await waitFor('the card of the wallet', () => nameField());

    const side = document.querySelector('.sidebar');
    const cases = [];
    for (const zoom of ['1', '1.25', '1.5']) {
      for (const width of ['13rem', '15rem', '17rem']) {
        for (const font of [0, 2]) cases.push({ zoom, width, font });
      }
    }
    const runCases = async (what, expected) => {
      for (const c of cases) {
        document.documentElement.style.zoom = c.zoom;
        side.style.width = c.width;
        T().savePrefs({ fontAdjust: c.font });
        await sleep(120);
        const f = nameField();
        const name = `${what}, zoom ${c.zoom}, sidebar ${c.width}, text ${c.font >= 0 ? '+' : ''}${c.font}`;
        if (!f) {
          await check(name, false, 'no card');
          continue;
        }
        const detail = JSON.stringify({ text: f.text, room: Math.round(f.room), fits: f.fits, apart: f.apart, title: f.title });
        // The whole name, or its start with "…" (the whole in its tooltip); never a field squeezed to nothing.
        const whole = f.text === expected && (f.fits || (f.ellipsis && f.roomEm >= 4.5));
        await check(`${name}: the name shows`, whole && f.apart && f.title === expected && f.roomEm >= 4.5, detail);
        if (expected === 'demo') await check(`${name}: "demo" whole`, f.fits, detail);
      }
      document.documentElement.style.zoom = '';
      side.style.width = '';
      T().savePrefs({ fontAdjust: 0 });
      await sleep(150);
    };

    await runCases('one wallet', 'demo');

    // Audit port-1 #11: the theme picker of the sidebar shows "Matrix Green" whole, and the labels of the cards
    // of Home ("Pending bundles") are never cut, at every zoom, sidebar width and text size.
    T().savePrefs({ theme: 'matrix' });
    await sleep(200);
    const textFits = (el) => {
      const range = document.createRange();
      range.selectNodeContents(el);
      return range.getBoundingClientRect().width <= el.getBoundingClientRect().width + 0.5;
    };
    for (const c of cases) {
      document.documentElement.style.zoom = c.zoom;
      side.style.width = c.width;
      T().savePrefs({ fontAdjust: c.font });
      await sleep(120);
      const look = document.querySelector('.sidebar select.look');
      const style = look && getComputedStyle(look);
      const ctx = document.createElement('canvas').getContext('2d');
      if (style) ctx.font = `${style.fontWeight} ${style.fontSize} ${style.fontFamily}`;
      // The room of the text: the box less its paddings (the arrow is in the padding on the right).
      const room = look ? look.clientWidth - parseFloat(style.paddingLeft) - parseFloat(style.paddingRight) : 0;
      const need = look ? ctx.measureText(look.options[look.selectedIndex].text).width : 1;
      const cut = [...document.querySelectorAll('.pairs dt')].filter((dt) => !textFits(dt)).map((dt) => dt.textContent);
      const name = `zoom ${c.zoom}, sidebar ${c.width}, text +${c.font}`;
      await check(`${name}: "Matrix Green" whole in the theme picker`, !!look && need <= room + 0.5, JSON.stringify({ need: Math.round(need), room: Math.round(room) }));
      await check(`${name}: no label of the cards of Home cut`, cut.length === 0 && document.querySelectorAll('.pairs dt').length > 0, JSON.stringify(cut));
    }
    document.documentElement.style.zoom = '1.25';
    await snapshot('walletcard-matrix-125');
    document.documentElement.style.zoom = '';
    side.style.width = '';
    T().savePrefs({ theme: 'midnight', fontAdjust: 0 });
    await sleep(150);
    await snapshot('walletcard-demo-midnight');
    document.documentElement.style.zoom = '1.5';
    await snapshot('walletcard-demo-midnight-150');
    document.documentElement.style.zoom = '1.25';
    await snapshot('walletcard-demo-midnight-125');
    document.documentElement.style.zoom = '';

    // Two wallets: the picker, with the long name chosen.
    await rpc('createwallet', [LONG]);
    await waitFor('two wallets', () => T().wallets.list.length === 2);
    T().selectWallet('demo');
    await waitFor('the picker', () => nameField() && nameField().el.tagName === 'SELECT');
    await runCases('two wallets, demo', 'demo');
    T().selectWallet(LONG);
    await sleep(200);
    await runCases('two wallets, a long name', LONG);
    await snapshot('walletcard-long-midnight');
    await log(`walletcard: ${cases.length} cases each`);
    await finish();
  } catch (e) {
    await snapshot('walletcard-failure').catch(() => undefined);
    await finish(e);
  }
})();
