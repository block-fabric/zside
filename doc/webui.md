# chains-app: the HTML interface of Chains

`chains-app` is the GUI of Chains: the node with its wallet and an HTML
interface in a native web view, in one program, without Qt. It replaced
`chains-qt`, which was removed from the tree (the commit "Remove the Qt GUI").
What it had to do to replace it is recorded in [webui-parity.md](webui-parity.md);
the audit of it, and how each finding was closed, in
[webui-audit.md](webui-audit.md). Its window comes in two layouts
(dashboard, workspace tabs), both starting on Home, with a command palette and an
integrated title bar: [webui-layouts.md](webui-layouts.md).

This document says how to build and run it, and how it is made, so that the
sidechains can take it over for their own apps.

> **In this tree (zSide).** `zside-app` is the app of the sidechain template with the pages of zSide's
> shielded wallet: the transparent and shielded balances, private payments with memos, shielding and
> unshielding, the memos received, the viewing keys, melt & cast, the shielded pool. They are described in
> [The pages of zSide](#the-pages-of-zside); their inventory against the Qt GUI of zSide is section ZS of
> [webui-parity.md](webui-parity.md).
>
> **From the sidechain template.** The kit is the one of `chains-app`, made configurable: the
> program is `<stem>-app` (here `zside-app`, and `test_zside-app`), and what the app shows of its
> chain is in one file, `src/webui/app/src/chain/chain.json`. The app of a sidechain has pages of its own
> (Peg, Merged mining, its home, the Mainchain tab of Settings) in place of those of the mainchain
> (Sidechains, Sidechain nodes, Solo mine). How, and how a sidechain adds its own pages so that the
> merges of the template stay clean: [The app of a sidechain](#the-app-of-a-sidechain). The commands
> below say `chains-app`; read `<stem>-app` (the CMake targets keep the names of chains-app).

## Build

```
cmake -B build              # BUILD_WEBUI is ON by default; -DBUILD_WEBUI=OFF for a node without a window
cmake --build build --target chains-app test_chains-app
```

* **Needs**: GTK 3 and WebKitGTK 4.1 (`libwebkit2gtk-4.1-dev`), Node.js with
  npm. The first build runs `npm ci` in src/webui/app (the exact versions of
  package-lock.json); later builds need no network.
* `-DWEBUI_PREBUILT_DIST=<dir>` takes an interface built elsewhere
  (`npm run build` in src/webui/app) and needs no npm.
* The interface is built by `npm run build`: the catalogs of the languages
  first (`scripts/i18n.mjs`, from the catalogs in src/webui/locale; the
  catalogs made are committed in `public/locales`), then Vite. The result is embedded in
  the binary (cmake/embed.cmake → `assets_data.cpp`).
* Windows (WebView2) has its own window (window_win32.cpp, below); macOS
  (WKWebView) builds the generic window (window_generic.cpp) until its port
  (file choosers, tray, frame, App Nap), see the parity list.

On Linux, `cmake --build build --target deploy-app` makes the release as a
tarball (`release-app/chains-app-<version>-<arch>-linux-gnu.tar.gz`: chains-app,
the programs of the node, the desktop entry and the icon, under `bin/` and
`share/`). `cmake --install build` installs the same (component `chains-app`
for the app, its desktop entry and icon; `test_chains-app` goes to libexec).

### Windows

Cross-compiled from Linux (or WSL) with the depends system and mingw-w64 (posix):

```
make -C depends HOST=x86_64-w64-mingw32 -j4
cmake -B build_win --toolchain depends/x86_64-w64-mingw32/toolchain.cmake -DWITH_CCACHE=OFF
cmake --build build_win -j4 --target chains-app test_chains-app
cmake --build build_win --target deploy-app   # zip + NSIS installer in build_win/release-app
```

The depends package `webview2` brings the headers of the Microsoft.Web.WebView2
NuGet package (BSD-3-Clause, its LICENSE.txt in depends/<host>/share/webview2).
No WebView2Loader is linked or shipped: webview.h finds the WebView2 Runtime
itself. The runtime is part of Windows 10 (since 2021) and 11; the app and the
installer point to its download page when it is missing. The window
(window_win32.cpp) has the integrated title bar, the tray icon and its menu,
notifications (balloons of the tray icon, shown as toasts), file dialogs
(IFileDialog), the clipboard, per monitor DPI awareness, the single instance
over a named pipe only the user can open, and registers `chains:` for the user
(HKCU\Software\Classes; `-webuiregisteruri=0` to skip, never with a test
script). Its data (WebView2 caches) are in `%LOCALAPPDATA%\Chains\WebView2`.

Debug options: `-webuidevtools` (web inspector), `-webuiconfig=<file>`,
`-webuitestscript=<file>` and `-webuitestshots=<dir>` (tests). The last two,
and the test methods of the bridge they come with (`test.confirmAnswer`,
`test.fileAnswer`...), are only in a build for the tests:
`cmake -DWEBUI_TEST_HOOKS=ON` (OFF by default). A release is built with it OFF
(`deploy-app` refuses a build that has them, and the ctest
`webui_test_hooks_built` checks the program); the end-to-end tests need it ON.
Working on the interface alone: `npm install` then `npm run dev` in
src/webui/app opens it in a browser with a pretend node (lib/mock.ts).

## Run

```
build/bin/chains-app [options of chainsd] [chains:URI]
```

* The options of chainsd, and the GUI options of chains-qt: `-choosedatadir`,
  `-lang=<lang>`, `-min`, `-resetguisettings`, `-splash`.
* Settings: those of the node stay in `settings.json` of the data directory,
  as with chains-qt. Those of the app (the data directory chosen in the intro,
  the window, the look, the unit, the language of the menus...) are in
  `~/.config/Chains/chains-app.json`, one set per network.
* One app per data directory: a second start hands its `chains:` URIs to the
  first one (single-instance socket in a private folder of the user).
* Debug options, from the command line only (a chains.conf that sets them is
  ignored): `-webuidevtools` (web inspector), `-webuiconfig=<file>` (another
  settings file of the app), `-webuitestscript=<file>` and
  `-webuitestshots=<dir>` (the tests: a script run in the page, with test
  methods on the bridge; only in a build with `WEBUI_TEST_HOOKS=ON`).

## How it is made

```
page (Svelte, chains-app://app/, UI thread)            the user
  │ window.__chains(request)          ▲ window.__chainsEvents(batch)
  ▼                                   │
Bridge                                EventHub (batches, coalesces, throttles)
  ├─ quick lane (4 workers)           ▲
  ├─ long lane (6 workers)            NodeEvents ◄── signals of the node and wallets
  ├─ UI thread (window, clipboard)
  ├─ rpc  → RPC policy → interfaces::Node::executeRpc
  └─ host → methods of the app (app.cpp, walletapi.cpp, nodeapi.cpp, ...)

App, on a thread of its own: intro, configuration, init, then shutdown
```

### The host (src/webui, C++)

| File | What it does |
|------|--------------|
| main.cpp | The command line, as chains-qt: options, payment URIs, `-help`; starts App; translations of the node (`G_TRANSLATION_FUN`) |
| app.cpp | The life of the app: intro, `InitConfig`, single instance, node init and shutdown on a thread, the phases the page follows, dialogs of the node, the host methods of the app (files chosen by the user, clipboard, preferences, options, the window, the tray, notifications), the language, the integrated title bar |
| bridge.cpp | Requests of the page: the RPC policy, the lanes, cancellation, answers made valid UTF-8, secrets wiped after use, the internal error hook |
| rpcpolicy.cpp | The RPC methods the pages may call, and which of them run on the long lane |
| events.cpp | The events to the page: batched, a key coalesces the ones that change fast (the tip during a sync) |
| nodeevents.cpp | Signals of the node and of each wallet made into events; balances polled as WalletModel does |
| walletapi.cpp | The wallet functions of chains-qt (WalletModel, WalletController, the wallet dialogs) as host methods, with the same interfaces::Wallet calls, rules and messages |
| nodeapi.cpp, console.cpp | The node window: the console (RPCParseCommandLine and RPCExecutor without Qt), peers and bans, tools (hashes, merkle trees, decoders, deposit addresses, stamps) |
| options.cpp | The options of the node as OptionsModel keeps them, and `OptionsJournal`: the changes of the Settings page wait for Apply |
| sidechainnodes.cpp | The Sidechain Nodes window: install, start, stop the nodes of sidechains |
| i18n.cpp | The language: chosen as chains-qt chooses it, the catalog of the page, the translation of the node's messages |
| icontint.cpp | The icon of the network (NetworkStyle) |
| assets.cpp | The embedded interface, its content security policy, the inline document of the other platforms |
| appconfig.cpp, singleinstance.cpp | The settings file of the app; one app per data directory |
| window_gtk.cpp, tray_gtk.cpp | The window on Linux: WebKitGTK through webview/webview, file choosers, clipboard, the integrated title bar, the tray (StatusNotifierItem) and notifications |
| window_win32.cpp, urischeme_win.cpp | The window on Windows: WebView2 through webview/webview (the interface served by the web view itself under `https://app.chains-app.invalid/`, a name that never resolves, with the same policy as a header), the integrated title bar (WM_NCCALCSIZE, snap layouts), the tray, toasts, IFileDialog, the clipboard, the session end; `chains:` registered for the user |
| window_generic.cpp | The window of the other platforms (macOS), through webview/webview (to be completed by its port) |

**The bridge.** The page sends `{rid, kind: rpc|host|cancel, method, params,
wallet}`. RPC calls go to the in-process dispatcher, as the console of
chains-qt does: no HTTP server, no credentials. They are limited to the
methods of `rpcpolicy.cpp` (a frontend test checks that every method the pages
name is there); the console (`console.run`) is the one way to run any command,
and a command that does more than read waits for the user's word, given to the
desktop (below). The policy holds in the end-to-end tests too: their script
sets the node up with requests of the kind `testrpc` (taken only with
`-webuitestscript` in a build with `WEBUI_TEST_HOOKS`), and each run fails on
an RPC method the policy refused to a page (`test.refused`).
Calls never run on the UI thread. Calls that may take long (a command of the
console, a download, a rescan, a file chooser, loading a wallet,
`scantxoutset`) run on a lane of their own and never hold up the quick ones. A
call can be cancelled. Every answer and every event batch is made valid UTF-8
(text of the chain is any bytes, and WebKit drops a script that is not UTF-8).
Strings named `passphrase`, `old`, `new` are wiped once a request is handled.
A host method that throws something it did not expect shows the internal error
dialog of chains-qt.

**Keys.** Calls that need the keys of an encrypted wallet take its passphrase
for that call: the host unlocks the wallet, runs the call and locks it again
(`UnlockScope`, as WalletModel::UnlockContext). A locked wallet answers -13 and
the page asks for the passphrase. RPC methods that need keys (deposits, stamps,
signing a PSBT, proofs of funds) go through `wallet.withKeys` the same way; the
page never leaves a wallet unlocked for a while.

**The history.** `wallet.transactions` gives the whole history once (records as
TransactionRecord makes them, as arrays), the records of some transactions
(`txids`: the ones an event named), or the newest (`latest`). Each record says
the height of its block and its state, so the page brings the status of every
record up to date with each block by itself
(TransactionRecord::updateStatus); only transactions out of a block are asked
for again. The balance chart is made by the host (`wallet.balanceHistory`).

**Settings.** The options of the node are written to settings.json as they
change, and what they replaced is kept by `OptionsJournal` (in chains-app.json):
Apply forgets it, Cancel puts it back, and the next start puts back what was
never applied, before the node reads its settings.

**Languages.** The catalogs of Bitcoin Core's translators (src/webui/locale,
about 100 languages, as chains-qt shipped them; see
[translation_process.md](translation_process.md)) are made into catalogs of the app by `scripts/i18n.mjs`: the messages the
interface shows (normalized as it writes them: no mnemonic `&`, `…`, Chains for
Bitcoin, labels without their colon) with their plural forms, and the messages
of the node. The host chooses the language (-lang, the option, the locale of
the system) and gives the catalog to the page (`app.locale`); the page
translates its own words only (opt-in, see Translations below), and the node's
messages go through `G_TRANSLATION_FUN`.

### The interface (src/webui/app, Svelte 5 + TypeScript + Vite)

```
src/
  main.ts, App.svelte     start: hello, language, the title bar, phases (intro, splash, shell, fatal)
  views/                  Shell (the layout chosen, the palette), layouts/ (Dashboard, Workspace),
                          home/ (Home, in both layouts), PageView, Intro, Splash, Fatal
  pages/                  one per page of chains-qt (Overview, Send, Receive, Transactions,
                          Sidechains, Mine, Explorer, NodeWindow, Settings, Wallets, ...)
  components/             shared parts: DataTable (virtual, sortable), Dialog, DialogHost,
                          AmountInput, AddressInput, MenuButton, CommandPalette, DetailPanel,
                          Sidebar, StatusBar, SyncOverlay, Toasts, TitleStrip, WindowControls,
                          ResizeHandles, ...
  lib/
    bridge.ts             the transport, requests, events
    rpc.ts                typed RPC and host calls
    state.svelte.ts       the state of the app, the node, the wallets, the preferences
    walletdata.svelte.ts  the current wallet: info, history kept up to date, address book
    wallet.ts             wallet calls, record status, filters, CSV
    i18n.svelte.ts        the language: t(), tn() (no translator of the whole page)
    commands.svelte.ts    every action: the pages and the menus of chains-qt (Menus, palette)
    layout.ts, detail.svelte.ts  the layouts; the detail panel
    windowframe.svelte.ts the integrated title bar
    settingsdraft.svelte.ts  the changes of Settings until Apply
    nav.svelte.ts, keyboard.ts, dialogs.svelte.ts, units.ts, format.ts, ...
  styles/                 themes.css (colour tokens of the themes), base.css (shared rules)
  test/                   vitest: pages, components, the audit fixes, contrast, translations
public/locales/           the catalogs of the languages (made by scripts/i18n.mjs)
```

Rules the code keeps:

* **Programs run only on the user's word, given to the desktop.** Whatever
  makes the app run a program (a sidechain downloaded from Sidechain Nodes, the
  external signer `-signer`) goes through `Window::Confirm`, a dialog of the
  window system that the page can neither draw nor answer, and a program path
  only comes from the file chooser of the desktop. Downloads are https only,
  with no user name before the host (`https://a@b/` is refused), with a
  SHA-256 checked before anything is unpacked; the releases built into the app
  (`sidechainreleases.h`) come first. The dialog says first what the app
  decides, in fixed words: its title whether the release is built into the app
  or was typed in it, its first line the host the download comes from (parsed
  by the host), alone on its line; what the page gave (the name of the
  sidechain, the address) only after. The name of a sidechain added by hand is
  one line of at most 64 printable characters (refused otherwise, and cleaned
  up again wherever it is shown). Sidechain nodes get the RPC
  password of this node in an owner-only file (`-mainchainrpcpasswordfile`),
  never on their command line.
* **The page does not act for the user where it matters.** The page draws the
  app; it cannot, by itself, do what a compromised page would want
  (guard.cpp, audit port-1 #3). These wait for a confirmation of the desktop,
  in fixed words that say first what the host parsed:
  - a command of the console that does more than read (anything but `get…`,
    `list…`, `decode…`, `estimate…`, `verify…`, `help` and the commands that
    only build or decode data; `listdescriptors` and `gethdkeys` always): the
    name of the command first, once per command and line, checked as each
    command runs, nested ones too;
  - a web address that is not one of the app's own links (`app.open`): its
    host first, http said to be unencrypted;
  - in the app of a sidechain, what spends the wallet of the mainchain node
    and where its credentials go (doc/sidechain.md).
  A test that did not queue an answer (`test.confirmAnswer`) gets "no", never
  a dialog on the desktop.
* **Direction.** Text from outside is isolated as to direction: SafeText is
  `dir="auto"`, every `translate="no"` is `unicode-bidi: isolate`, and a value
  that can turn the text (a letter of a right-to-left script, a mark of
  direction) put in `t()`/`tn()` goes between FSI and PDI, so that a title
  starting with U+202E cannot turn the app's own sentence around, amounts
  included (audit port-1 #2).
* **Text is text.** No `{@html}`, no `innerHTML`: what comes from the chain,
  the node or the user shows as text (a test checks the sources). The content
  security policy allows nothing but the app's own files: no remote content,
  no inline scripts or styles, no `eval`.
* **Translations (opt-in).** Write the interface in English, with the words of
  chains-qt where it has them (they are then translated by its catalogs). Only
  the app's own words are translated, never what the page happens to show:
  - the words of the templates (text, and the title, placeholder, aria-label
    and alt attributes; the label/title/placeholder/empty/text props of
    components; string literals in `{a ? 'X' : 'Y'}`) become calls of `t()`
    when the components are built (`scripts/i18n-preprocess.mjs`); values put
    in them (`{n} blocks behind`) are spliced in as they are, never looked up;
  - words of the app held in code (tabs, menus, titles) are shown with `t()` /
    `tn()`;
  - a message with values (a toast, a dialog, a status, "3 hours behind") is
    translated where it is made, with its own placeholders:
    `t('%1 behind', niceTimeOffset(s))`, `tn('%n hour(s)', n)` ("(s)" is the
    English plural without a catalog). Toasts, dialogs and statuses show what
    they get as it is (`SafeText`): an error of the host, a message of the
    node or the RPC is never looked up, so never partly translated;
  - text from the chain, peers, the wallet or the user (labels, memos,
    comments, sidechain titles, peer versions, URIs, wallet names) is shown
    with `<SafeText text={...} />` (translate="no"), or in an element with
    `translate="no"`. `test/i18n-lint.test.ts` fails on a field of words shown
    bare, on such text put in `t()`, on text put together given to `t()` (a
    call, a template with values, a sum), and on words of the app put in a
    toast or a dialog without `t()`.
  Only the source of a message, as written in the app, is looked up, as a
  whole: nothing is matched in other text. After a change of
  the texts, `npm run i18n` makes the catalogs again (a test checks they are
  up to date).
* **Themes.** Colours are tokens of themes.css (`--text`, `--text-muted`,
  `--text-faint`, `--accent`, `--accent-fg`, ...): every text token reaches
  4.5:1 on every surface of every theme (test/contrast.test.ts measures it).
  Use `--accent-fg`, not `--accent`, for text.
* **Narrow windows.** Pages wrap rather than clip; grids use
  `minmax(min(Xrem, 100%), 1fr)`; below 760 px the dashboard's sidebar is a
  drawer; the menus are in one Menus button
  that opens inside the window; tables can `fit` the width.
* **Long lists.** Keep them as raw state (`$state.raw`), replaced as a whole,
  and draw them with DataTable (only the rows in view).

## The app of a sidechain

### The chain, in one place: chain.json

`src/webui/app/src/chain/chain.json` says what the app shows of its chain. CMake reads it
(src/webui/CMakeLists.txt makes `webui/chainconfig.h` of it for the host) and so does the interface
(`src/chain/config.ts`, `CHAIN`). A chain made from the template changes this file, not the kit:

| Field | What it sets |
|---|---|
| `name`, `bin` | The name users see and the stem of the programs: `CLIENT_NAME` and `CLIENT_BIN_NAME` of CMakeLists.txt, which must be the same (CMake stops otherwise; `contrib/sidechain/rename.py` sets both). The program is `<bin>-app`; its settings file `<bin>-app.json` in the folder `<name>` of the user's configuration; its desktop entry, tray, notifications, startup link, single-instance socket and WebView2 folder are named after them; the name takes the place of "Bitcoin" in the catalogs (`scripts/i18n.mjs`, `i18n.cpp`: run `npm run i18n` after a change) |
| `role` | `sidechain` (its peg and merged mining pages, its home) or `mainchain` (the pages of Chains). C++: `WEBUI_SIDECHAIN` |
| `coin` | The ticker and the name of the coin: the units (`CHN`, `mCHN`, `bits`, `sat` are the ids kept in the preferences; the names shown are the chain's) |
| `uriScheme` | The scheme of its payment URIs (BIP 21): `sidechain:` here, `thunder:`, `bitnames:`... The links the app opens, hands over to the running app, registers on Windows and in the desktop entry |
| `brand` | Its mark (`mark`: SVG paths on a 32 grid, drawn by components/Brand.svelte), the colour of the mark (`color`, the CSS variable `--brand`), the hue the icon of the app is turned by (`iconHue`, before the tint of a test network: icontint.cpp) |
| `networks` | Per network (main, test, signet, regtest): the default ports (`p2p`, `rpc`) and the address prefixes (`hrp`, `pubkey`, `script`) the interface checks addresses with. `test_<bin>-app` (chainconfig_tests.cpp) checks them against the chain parameters |
| `mainchain` | A sidechain: the name, ticker and URI scheme of its mainchain, and its networks (the RPC port, the default of the Mainchain settings; the address prefixes, for the withdrawals: `checkAddress(..., 'mainchain')`) |
| `links` | Help links (`guide`, `website`, `source`, `issues`): items of the menu "This node" and the palette, opened in the browser of the desktop |
| `drop` | Pages of the kit the chain does not have. A sidechain drops `sidechains`, `sidechain-nodes` and `mine` (no sidechains of its own, no proposals or votes, no solo mining): they leave the sidebars, the workspace, the menus, the palette and the quick actions (`chainHas`, `pageAvailable` in lib/commands.svelte.ts); the host does not register the sidechain nodes, and the RPC policy of a sidechain has none of their methods |

### The pages of a sidechain

| Page | What it does | Code |
|---|---|---|
| Home (the dashboard's) | Cards: Balance, Network, **Drivechains** (the peg; user decision 2026-10-15: shown as Drivechains) (deposits on their way, withdrawals waiting, the bundle and its votes, the slot), **Merged mining** (on or off, blocks mined, requests, the share of the last 24 mainchain blocks that committed to this chain); the wallet balance; the blocks of the mainchain, lit when they committed; the tables (transactions, blocks, peers) | views/home/SideHome.svelte |
| **Peg** (Chain section, Alt+5) | The peg drawn (the mainchain and this chain, the connection, the heights). Deposit: a deposit address of the wallet (copy, QR code) to give the wallet of the mainchain; the helper, which has the wallet of the mainchain node make the deposit (`peg.deposit`); the deposits of the last 2000 mainchain blocks, credited or on their way, those to this wallet marked. Withdraw: to an address of the mainchain (checked with its prefixes, or one of the mainchain wallet a click away), the fee for its miners; the withdrawals not paid yet, those of the wallet taken back while they wait. Bundle: the bundle the mainchain votes on, its votes (`listwithdrawalbundles` of the mainchain) against the score it needs and the blocks it has left; the bundles before it, paid or failed | pages/Peg.svelte |
| **Merged mining** (Chain section) | Automatic mining on and off (`setbmm`), the address of the fees, "always" with its offer (tests); one block by hand (`requestbmmblock`); requests, blocks mined, outbids, candidates; the last mainchain blocks and the block of this chain each committed to, in this chain or not; the connection to the mainchain node; a log of what happened since the app started | pages/Bmm.svelte |
| Settings → *the mainchain* | The connection to the mainchain node: its address, RPC port (empty for the default of the network), the wallet that pays for merged mining, the user name; the cookie file and the password file chosen in the file chooser of the desktop only (options.chooseFile), never typed; they wait for Apply and take effect after a restart | components/MainchainSettings.svelte, options.cpp |

The state they share (the node's view every 3 seconds, the mainchain's when its tip moves, the events
of its blocks fetched as they come) is in lib/sidestate.svelte.ts; what puts it together (the bundles
and their history, the deposits, the blocks that committed) in lib/side.ts.

The host side (src/webui/sidechainapi.cpp) reaches the mainchain node on the node's own connection
(`sidechain::Follower`, -mainchainrpc*); the page never names the node or its credentials:

| Host method | What |
|---|---|
| `peg.connection` | How the node reaches its mainchain node: connected or why not, host:port, the slot, the wallet, cookie or password (never the password) |
| `peg.call {method, params}` | A method of the mainchain node, from a list that only reads (`getdrivechaininfo`, `listwithdrawalbundles`, `getsidechainevents`, `listsidechaindeposits`...) and of its wallet (`getbalances`, `getwalletinfo`, `getnewaddress`) |
| `peg.deposit {destination, amount}` | The deposit helper: `createsidechaindeposit` of the mainchain wallet, only to a deposit address of this sidechain (its slot, its checksum, a valid address of this chain), on the user's word given to the desktop |
| `bmm.set {mine, address?, always?, amount?}` | Merged mining on (`setbmm`: the address of the fees, an address of this chain; with `always`, the offer for every block of the mainchain, at most 0.01) on the user's word given to the desktop; off at once |
| `bmm.request {address, amount}` | One block by hand (`requestbmmblock`), the offer at most 0.01, on the user's word given to the desktop |

**What spends the wallet of the mainchain node waits for the desktop** (audit port-1 #4). A deposit,
merged mining turned on and a block asked for by hand are host methods: the host checks what the
page gives, caps the offers of merged mining (`SidechainApi::MAX_BMM_OFFER`), then asks with
`Window::Confirm`, the amount it parsed first ("Amount: 5.00 coins of Chains", "Offer: 0.0001 coins
of Chains for every block of Chains"). A decline answers `CANCELLED`, which the pages take quietly.
The address of the mainchain node (`mainchainConnect` in Settings) is another machine only on the
user's word too: the node sends that machine the credentials of the mainchain node, unencrypted.
`setbmm`, `requestbmmblock` and `createsidechaindeposit` are not methods the page calls.

The RPC policy of a sidechain (rpcpolicy.cpp) adds its methods (`getmainchaininfo`, `getbmminfo`,
`syncmainchain`, `listwithdrawals`, `getwithdrawalbundle`, `getdepositaddress`, and
`createwithdrawal` and `refundwithdrawal`, which need the keys) and has none of the drivechain methods
of the mainchain. `test/rpcpolicy.test.ts` reads it as this chain builds it (the branch of `#if
WEBUI_SIDECHAIN` of its role, with local.cpp), against the sources that run in this chain (not the
pages chain.json drops, nor the home of the other role); the end-to-end runs keep to it as well.

The help links of chain.json (`links`) are the app's own: they open without asking (they are built into
the host, webui/chainconfig.h); any other web address waits for the desktop.

In a narrow panel (a window of 480 px) the tables of Peg and Merged mining are lists of cards, every
cell under the words of its column (`use:stacked`, a container query in styles/side.css).

The rules of the kit hold in these pages: text from the chain (addresses, hashes, errors of the
mainchain node) is shown with `SafeText` or `translate="no"`; the app's own words go through `t()`;
files only through the native chooser; amounts with `Amount`; tables in the colour tokens.

### Pages of a chain of its own

A sidechain made from the template (BitNames, BitAssets, Thunder...) adds its pages in three files the
template keeps empty, and nowhere else, so that a merge of the template never conflicts with them:

1. **src/webui/app/src/chain/local.ts**: an entry per page in `LOCAL_PAGES` (`id`, `title`, `icon`,
   `summary`, `section`: `wallet`, `chain` or `tools`, `wallet: true` if it needs one, a `quick` action
   of the home), and menu items in `LOCAL_COMMANDS`. The page then is in the sidebars (in its
   section), the workspace (its area), the palette, the menus (those of `chain` in the menu of the
   chain, before the tools: the `chainBeforeTools` tests check every list) and `go('<id>')` opens it.
2. **src/webui/app/src/chain/LocalPages.svelte**: imports the page components (in
   `src/chain/pages/`, say) and shows the one of `page`.
3. **src/webui/local.cpp**: the RPC methods the pages call (`RpcMethods`, added to the page's
   allowlist; those that need the keys in `KeyedRpcMethods`) and the host methods they need
   (`RegisterHost`, after the kit's; with `confirm`, the confirmation of the desktop, for what a page must
   not do on its own word, such as showing a key: a no answers `BRIDGE_CANCELLED`).

The template keeps more hooks empty for what a chain adds beside its pages (audit port-2 #7), so that
these too never conflict:

| File | What a chain puts there |
|---|---|
| `src/chain/icons.ts` | `LOCAL_ICONS`: the icons of its pages, added to the kit's (components/Icon.svelte) |
| `src/chain/LocalCards.svelte` | Its cards on the home of the dashboard, under the kit's (views/home/SideHome.svelte) |
| `src/chain/LocalSend.svelte` | What it sends beside its coin (an asset, a name), at the top of Send; `active` set while its own form takes the place of the kit's |
| `chain/local.ts` `shortcut: 'Alt+6'` | Alt+6 and on open its pages (lib/keyboard.ts), never over the kit's Alt+1 to Alt+5 |
| `chain.json` `blockWeight` | Not needed: the explorer draws blocks against the weight the node says (`app.info.blockWeight`, `consensus.max_block_weight` of the network); `blockWeight` is only the fallback before it has |
| `chain.json` `blockTarget` | The time between blocks the chain aims at, in seconds: the "target" of the explorer's ribbon (Chains' 60 when not set) |
| `src/chain/explorer.ts` | `EXPLORER`: its columns of the block list (after Height, from `getblock`), the kit's columns it hides, facts at the top of a block's details, the tag of a ribbon box, the block target as the node says it (`target()`, over `blockTarget`), a `refresh` with each tip. The template's is empty |
| `src/chain/roles.ts` + `local::CoinbaseRoles` (local.cpp) | The roles of the outputs of its coinbases (the 19th field of a wallet record): the history says the role's label instead of "Mined", the details its source, the type filter has one entry per role; an output of a role that pays 0 is left out. The template's are empty |
| `src/webui/test/local_tests.cpp` | The unit tests of local.cpp, built into `test_<stem>-app` when the file is there |
| `src/webui/test/e2e/local.cmake` | `webui_local_e2e(<name> <port> [options])` for each page script `e2e/<name>.js` of its pages: run by run_sidechain.py (`--script`, `--app-arg`, `--js-var NAME=VALUE`, `--file NAME=TEXT` written in the folder of the run, `{tmp}` in an `--app-arg` for that folder, `--width`; `window.__E2E_DIR` is the folder of the run) against a live mainchain, as `webui_e2e_<name>` |

```ts
// chain/local.ts
export const LOCAL_PAGES = [
  { id: 'names', title: 'Names', icon: 'book', section: 'chain', summary: 'Register names and their records', wallet: true, quick: { label: 'Register a name' } },
] as const satisfies readonly LocalPage[];
```

Its own words follow the rules of the kit (`t()` for the app's words; `SafeText` for a name read from
the chain); its tests go in `src/chain/` too (`*.test.ts` are found anywhere under src). Nothing in
`lib/`, `components/`, `pages/` or `views/` needs to change; when a page needs something the kit does
not offer, make it a change of the template (generic, behind `chain.json`), and merge it.

### The pages of zSide

In `src/webui/app/src/chain`: `local.ts` lists them (all five in the Chain section, after Peg and Merged
mining and before the explorer and the tools: the Wallet section keeps the user's six pages, audit port-2 #9), `LocalPages.svelte` shows them, `LocalCards.svelte` puts their cards on the home of the
dashboard, `icons.ts` draws their icons; `pages/` holds the pages, `zs/` what they share (`model.ts`,
`store.svelte.ts`, the privacy indicator `Privacy.svelte`, `ProvingPanel.svelte`, `ZAddressInput.svelte`,
`zs.css`). `src/webui/local.cpp` adds the RPC methods of the shielded pool and wallet to the page's
allowlist; those that prove a bundle (sendshielded, shieldcoins, unshieldcoins, bumpshieldedfee) and the
import of a viewing key (a rescan) run on the long lane; those that need the keys (the four that prove,
meltcoins, and getshieldedaddress, which sets the account up the first time) are keyed methods. Not
`exportshieldedviewingkey`: the node answers it even with the wallet locked, so the page asks the host
(`zside.viewingkey.export {wallet}`), which shows the key only after the desktop asked (`Window::Confirm`,
outside the page; a no answers `BRIDGE_CANCELLED`, taken quietly) (audit port-2 #3).

| Page | What it does |
|---|---|
| Private (Alt+6) | Overview: the transparent and the shielded balances side by side, each with its privacy indicator, the moves between them, how much of the wallet is shielded (a ring), the latest of the shielded account. Pay: privately, to one to ten recipients (shielded or transparent: `ZAddressInput` tells which, and a transparent one is said to unshield, in public, with no memo), a memo counted in bytes as the node counts them (512 at most), the fee chosen or not; confirmed in the page, then proved. Shield & unshield: transparent coins into the pool (all of it but a little for the fee; to the wallet's account or another shielded address; a memo), and out of it (an amount or all, the fee taken out or not; to a new address of the wallet or one typed). Receive: the default shielded address with its QR code, new ones nobody can link to it (`getshieldedaddress true`). Viewing keys: the key of the account, shown only after a warning whose box is ticked and the desktop's confirmation, blurred until asked; a viewing key watched in a wallet without private keys (`importshieldedviewingkey`, from a block) |
| Memos | The inbox: every memo a shielded payment brought the account (never those of its change), newest first, unread ones marked (in the app's preferences, one set per network, per wallet: the newest 1000 read, by short keys), searched, read in a pane of their own, as text (SafeText); copied, marked unread, or opened in their transaction |
| Private activity | The transactions of the shielded account in words (shielded, received privately, paid privately, unshielded), each with its privacy, its memos and whom it paid, filtered (received, paid, public) and searched; one waiting for a block pays a higher fee (`bumpshieldedfee`). The newest 500 are read, and the page (and Memos) says when older ones are left out, with "Show older" (500 more each time, up to 5000). The notes: amount, kind (received, change, watched, watch-only), memo, confirmations; the spent ones are read only while "Show the spent notes" is on |
| Melt & cast | The plan the wallet carries out as blocks come (`listmeltcast`): what is to go, soonest first, with its block and why it waits, then what went; a step or all cancelled. Melt (every transparent coin alone over the blocks chosen), cast (an amount as standard bills, the bills shown as it is typed, what stays in the pool). An encrypted wallet is told that a step waits while it is locked |
| Shielded pool | `getshieldedinfo`: what the pool holds, the notes made, the nullifiers revealed, the root of the tree, and whether the index of the whole tree (-orchardindex) is synced with it; what the chain shows and what it does not |
| Home | Cards: Private (both balances with their privacy, the ring, Pay privately, Shield), Memos (unread, the latest three), Melt & cast (while it has steps to go) |

**Proofs off the UI thread.** A payment out of the pool proves its bundle with Halo 2 in the node
(seconds). The page sends the call on the long lane of the bridge (`Bridge::Thread::LONG`) and stays
usable; `ProvingPanel` shows the proof under way, its steps and the time left, estimated from the earlier
proofs of the same kind on this computer (browser storage; a guess of 6 s at first), and never says done
before the node answers. The node itself proves without the wallet's lock (`SendShielded`), so the
other pages keep reading the wallet meanwhile.

**Privacy, said everywhere.** Every balance, address and transaction of these pages carries the
privacy indicator (`zs/Privacy.svelte`): Private (a shield, in the colour of the chain's mark) or Public
(an eye, in the warning colour), with a sentence that says why. A shielding and an unshielding are
public (the amount that goes in or out shows on the chain); a payment inside the pool is private.

**Viewing keys.** `exportshieldedviewingkey` gives the full viewing key of the account (`zview1…`,
`tzview1…`, `rzview1…`); `importshieldedviewingkey <key> ( height )` watches one, only in a wallet
without private keys (so that what it sees is not mixed with an account the wallet spends from), and
scans the blocks again from the height given. The key is kept with the keys of retired accounts
(version `KEY_VIEW_ONLY`); its notes are watch-only (`viewonly` in `listshieldednotes`).

## Tests

| Test | What it covers |
|------|----------------|
| `test_chains-app` | The host: the bridge (also in front of a real node), its lanes, policy, UTF-8 repair, wiped secrets and internal errors; events; app settings; assets and the inline document; single instance; node events; the wallet functions on a regtest chain (also without wallets, the history in parts, keys for one call, dust); the node window, options and their journal; the languages; the icon of the network |
| `npm test` (vitest) | The interface: units, addresses, URIs, sync labels, tables, the bridge client, the state, components, wallet helpers, dialogs, pages (send, transactions, settings, sidechains, node window...), the history kept up to date, the RPC policy, the contrast of every theme, the translations, narrow layouts, the fixes of the audit |
| `npm run check` | Types (svelte-check) |
| `e2e/run_smoke.py` | chains-app on regtest in a virtual display, driven through its page: the bridge, a wallet, events, preferences, escaping, shortcuts, a URI handed over by a second start; pictures of every page in each theme |
| `e2e/run_startup.py` | The start of chains-app: a new data directory, then four starts with no wallet to load (the node is ready before the page has its language: the page must take the "ready" event over the older answer of `app.hello`, and the dashboard open its home), then a start with a wallet; with the renderer of a desktop, where the race shows |
| `e2e/run_wallet.py` | The wallet pages as a user drives them |
| `e2e/run_node.py` | The node and Chains pages, with a pretend desktop (tray, notifications) and a second node |
| `e2e/run_audit.py` | The fixes of the audit: the sync overlay, a block built by hand with a title that is not UTF-8, long calls, a history kept up to date in parts, a proof with a locked wallet, contrast measured on what the page draws, aligned tables, the title bar of the desktop or the integrated one, settings that wait for Apply (and the next start), `-disablewallet`, windows 700 and 480 px wide, a hardware wallet (the mock signer of the functional tests), `-lang=fr` |
| `e2e/run_layouts.py` | The three layouts, the palette, the detail panel, the integrated title bar and the refresh (layouts.js, titlebar.js, refresh.js); pictures of each layout |
| `e2e/run_windows.py` | From WSL, on the Windows of the machine (`--app build_win/bin/chains-app.exe --unit build_win/bin/test_chains-app.exe --shots <dir>`, in a folder `<stem>-app-test` of the Windows temporary folder, removed at the end): the unit tests, then smoke.js, layouts.js and titlebar.js; for a sidechain sidechain.js, the app on Windows following a regtest mainchain run in WSL |
| `e2e/run_sidechain.py` | A sidechain (`webui_e2e_sidechain`, with `WEBUI_MAINCHAIN_BIN_DIR` or `MAINCHAIN_BIN_DIR` naming the folder of `chainsd`): a regtest mainchain started and mining a block every second, the sidechain activated in its slot, the app following it with a user name and a password file; through the page: a deposit address, the deposit made by the helper, merged mining on, the deposit credited, a withdrawal to an address of the mainchain wallet, its bundle voted on and paid (checked on the mainchain); pictures of the pages of the sidechain in the layouts and looks |
| `e2e/run_release.py` | The release as a user gets it (`deploy-app`, built with `WEBUI_TEST_HOOKS=OFF`; `--windows` for the zip, run on the Windows of the machine): what the package holds and SHA256SUMS, no hooks of the tests in the app; the app run with no test script on regtest against a live mainchain, its node answering the command line program of the package, following the mainchain in its slot, and stopping cleanly |
| `test_<stem>-app`: chainconfig_tests, sidechainapi_tests | chain.json against the chain parameters (names, unit, ports, prefixes); the methods of the mainchain the pages may call, the deposit helper refusing what is not a deposit address of this sidechain, the RPC policy of a sidechain |
| `npm test`: chain/zside.test.ts | zSide: shielded addresses and viewing keys of each network, memos in bytes, the history told in words with its privacy, the memos of the inbox, the bills of a cast, the plan in order, the estimate of a proof; the pages in the Wallet and Chain sections, the menus, Alt+6; the pages driven against a pretend node: the balances with their privacy, a private payment with a memo, a payment to a transparent address that unshields, shielding and unshielding, new addresses, the viewing key exported only once the warning is ticked and one watched only in a wallet without keys, the inbox, a fee bump, the notes, melt & cast, the pool, the proof under way, the cards of the home, a wallet to unlock once |
| `e2e/run_sidechain.py --script zside.js` (`webui_e2e_zside`) | zSide on regtest against a live mainchain, through its pages: coins shielded with a memo (the proof shown while the page answers), a private payment with a memo to a second wallet whose inbox shows it, an unshielding, the viewing key exported after its warning and watched in a wallet without keys, a cast planned and cancelled; pictures of every page in Dashboard and Workspace, Midnight and Light |
| `npm test`: test/sidechain.test.ts | The chain of chain.json (units, URIs, addresses of both chains), the pages dropped, Peg and Merged mining in the Chain section and the menus, the events of the mainchain kept up to date across a reorganisation, the bundles, deposits and commitments, the log of merged mining, and the pages driven: the deposit helper, a withdrawal and its refund, the votes of the bundle, merged mining on and off and one block by hand, the cards of the home, the Mainchain settings |

Each e2e script takes `--app build/bin/chains-app --tmp <dir> --shots <dir>`;
all are CTest tests (`webui_tests`, `webui_frontend_tests`,
`webui_frontend_types`, `webui_e2e_smoke`, `webui_e2e_wallet`,
`webui_e2e_node`, `webui_e2e_audit`). In the app of a sidechain the end-to-end tests of the mainchain
(they mine their own blocks) are not registered: `webui_e2e_sidechain` is.

## Credits

The menus and pages follow chains-qt. The translations are the work of the
translators of Bitcoin Core (Transifex), as chains-qt ships them. The tray
speaks the StatusNotifierItem and dbusmenu protocols, and the notifications
the Desktop Notifications specification (freedesktop.org); no code of other
projects is in them. The classic dark theme takes its colours from
QDarkStyleSheet (MIT), as chains-qt does. Fonts: Inter, Nunito, JetBrains Mono
(SIL Open Font License 1.1). webview/webview: MIT.
