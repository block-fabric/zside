// The interface in the language of the user (audit M6): -lang=fr.
(async () => {
  const { host, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip);
    await check('M6 the language of -lang', T().i18n.code === 'fr' && document.documentElement.lang === 'fr', `${T().i18n.code} ${document.documentElement.lang}`);
    T().nav.dialog = null;
    T().go('send');
    await sleep(500);
    await check('M6 the pages show in it', text('.content h1') === 'Envoyer', text('.content h1'));
    await check('M6 the sidebar too', text('.sidebar').includes('Vue d’ensemble') || text('.sidebar').includes("Vue d'ensemble"), text('.sidebar').slice(0, 80));
    const languages = await host('app.languages');
    await check('M6 about a hundred languages', languages.length > 90 && languages.some((l) => l.code === 'fr' && l.name.startsWith('Fran')), String(languages.length));
    await snapshot('audit-lang-fr-send');
    // An error of the host in a notice: shown as the host wrote it, never partly translated ("%1 and %2" made
    // "… sidechain et of its programs …"); the words of the app around it in French.
    const error = await host('sidechainNodes.add', { name: '', stem: '', slot: 0 }).then(() => null, (e) => e);
    await check('M6 the host answers with an error', !!error && /and of its programs/.test(error.message), String(error && error.message));
    T().toast('error', T().t('Settings'), error.message);
    await sleep(300);
    const notice = document.querySelector('.toast');
    await check('M6 an error of the host stays as it is in French', notice && notice.querySelector('p').textContent === error.message && !/\bet of\b/.test(notice.textContent), notice ? notice.textContent : 'no notice');
    await check('M6 the title of the notice in French', notice && notice.querySelector('strong').textContent === 'Paramètres', notice ? notice.querySelector('strong').textContent : '');
    await snapshot('audit-lang-fr-host-error');
    T().go('settings', 'display');
    await sleep(500);
    await snapshot('audit-lang-fr-settings');
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
