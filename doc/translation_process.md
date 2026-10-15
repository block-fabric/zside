Translations
============

chains-app, the GUI of Chains, is translated with the catalogs of Bitcoin Core: the work of its
translators on [Transifex](https://explore.transifex.com/bitcoin/bitcoin/), which chains-qt
shipped before the app replaced it. The catalogs, one per language, are in
[`src/webui/locale`](../src/webui/locale) (`bitcoin_<lang>.ts`, the XML format of Qt Linguist,
read by a script: no Qt is needed). The app takes from them the messages its interface shows
(written as the app writes them: no mnemonic `&`, `…`, Chains for Bitcoin, labels without their
colon), with their plural forms, and the messages of the node.

How the interface uses them (the opt-in rules of `t()`, `tn()` and `SafeText`) is in
[webui.md](webui.md#the-interface-srcwebuiapp-svelte-5--typescript--vite).

### The files

| File | What it is |
|------|------------|
| `src/webui/locale/bitcoin_<lang>.ts` | The catalogs, as Transifex gives them |
| `src/webui/app/scripts/i18n.mjs` | Makes the catalogs of the app from them |
| `src/webui/app/public/locales/<lang>.json`, `index.json` | The catalogs of the app (committed, embedded in the program) |
| `.tx/config` | The Transifex resource, and where its files go |

### Updating the translations

With the [Transifex client](https://developers.transifex.com/docs/cli) (the configuration is in
`.tx/config`; you shouldn't need to change it):

```sh
tx pull --all --force                       # the catalogs, into src/webui/locale
(cd src/webui/app && npm run i18n)          # the catalogs of the app, in public/locales
git add src/webui/locale src/webui/app/public/locales
git commit
```

`npm run i18n` keeps the finished translations only, and only the messages the app can show (the
text of `src/webui/app/src` and of the host, `src/webui/*.cpp`). The same is needed after a
change of the texts of the app: run `npm run i18n` again. A test of the interface
(`src/webui/app/src/test/i18n.test.ts`, also run by `npm test` and `ctest`) fails when the
catalogs of the app are not up to date with the catalogs and the sources.

Do not download translations one by one from the Transifex website: pull them all, so the
catalogs stay one set.

### Adding a language

A language Transifex has is added by `tx pull` (a new `bitcoin_<lang>.ts`) and `npm run i18n`:
the app lists every language whose catalog has messages it shows, by the name the language gives
itself. Messages the app has and the catalogs of Bitcoin Core do not have stay in English.
