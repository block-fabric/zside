# chains-app: parity and quality audit (phase 4)

> **History.** chains-qt (`src/qt`) was removed from the tree by the commit "Remove the Qt GUI"
> (`git log --grep='Remove the Qt GUI'`), after the work this document records: chains-app is the GUI of the
> build. This document stays as the record of the parity; the `src/qt` sources it cites are those
> of the parent of that commit (`git show <commit>^:src/qt/<file>`).

An independent audit of `chains-app` (branch `webui`, at 34f5d2e810) against `chains-qt`
(`src/qt`, read directly, not through `doc/webui-parity.md`). Nothing was changed in the code.

**Counts:** 1 critical, 2 high, 7 medium, 12 low (22 findings). Four more gaps (Windows and
macOS ports, themed frame) are already open in the parity list and are not counted again.

## Method

* **Reading.** `src/qt` against `src/webui` and `src/webui/app`. A script compared every `tr()`
  string and `.ui` string of `src/qt` with the strings of the web UI. About 300 had no
  counterpart, and each of them was checked by hand.
* **Driving the real app.** `build_webui/bin/chains-app` ran on regtest in xvfb, driven by page
  scripts through the test hook (`-webuitestscript`). The runner, the scripts and the data
  generator are in the scratchpad: `…/scratchpad/audit/` (`run.py`, `*.js`, `gen2.py`,
  `utf8.py`, `term.py`). The pictures are in `…/scratchpad/audit/shots/`.
* **Data used:**
  * a 21,800-transaction wallet (`big`, 300 labels, some with HTML in them);
  * a miner wallet with 24,989 records;
  * 5 wallets with HTML and Unicode names, one watch-only and one encrypted;
  * a block, built by hand, with a sidechain proposal whose title is not valid UTF-8.
* **Existing tests:** `test_chains-app` reports no errors, and `vitest` passes 84 of 84.

---

## Critical

### C1. With `-disablewallet`, opening the Wallets page aborts the whole app
* **Repro:** `chains-app -regtest -disablewallet`, then Overview → "Create or open a wallet"
  (`pages/Overview.svelte:143`), or any "Your wallet" menu item that leads to `wallets`.
* **Result:** the process aborts with exit 134:
  `node/interfaces.cpp:388 … walletLoader(): Assertion 'm_context->wallet_loader' failed`.
  Every `wallets.*` host method (`walletapi.cpp:799` onwards) calls `m_node().walletLoader()`
  without a check. The node is not shut down cleanly.
* **chains-qt:** with `-disablewallet` it builds no wallet UI at all (`enableWallet=false`).
* **Fix:** register the wallet host methods only when the wallet is enabled, or answer an
  error from them. Hide the wallet menus and the Overview button when `info.walletEnabled` is
  false.

## High

### H1. Chain text that is not valid UTF-8 breaks the bridge: the Sidechains page stays empty for good
* **Repro:** `…/audit/utf8.py`. A block whose coinbase has an M1 proposal with the title
  `"Bad \xff\xfe title"`, submitted on regtest.
* **Result:** after that block:
  * `listsidechainproposals` never resolves in the page (5 s timeout in the test);
  * the Sidechains page shows "The drivechain state is not available yet." on every tab
    (`shots/utf8-sidechains-light.png`);
  * `getblock` still works.
* **Cause:**
  * the RPC returns the raw bytes of the title (`rpc/drivechain.cpp:57`);
  * `Bridge::Reply` → `webview::resolve` → `json_escape` → `webkit_web_view_evaluate_javascript`;
  * WebKit drops a script that is not valid UTF-8, so the reply is lost silently;
  * events take the same path (`App::DeliverEvents`, `app.cpp:135`).
* **Who can trigger it:** any miner (proposal title or description), and any depositor:
  `listsidechaindeposits` returns `destination`, up to 100 arbitrary bytes, so one cheap
  deposit breaks the deposit list of that sidechain.
* **chains-qt:** not affected; `QString::fromStdString` replaces bad sequences.
* **Fix:** make every string valid UTF-8 (U+FFFD) before `Reply` and `DeliverEvents`, in one
  place. Add a test with a bad proposal and a bad deposit.

### H2. Large wallets: the whole history is reloaded and resent on every block and every transaction
* **How it works:**
  * `walletdata.svelte.ts:58` (`refreshRecords`) reloads every record after each tip change,
    at most every 2.5 s (`TIP_RELOAD_MS`), and after each transaction;
  * `walletapi.cpp:1012` (`wallet.transactions`) rebuilds every record under `cs_wallet` and
    sends all of them as one JSON string evaluated in the page;
  * the Overview loads the full history to show 5 rows and the chart.
* **Measurements:**

| Wallet | Overview opened | Each new block | Search, per key | Peak memory |
|---|---|---|---|---|
| 21,800 tx (`big`) | 1.5 s stall | 2.3 s reload, 0.5 s stall | 0.8 s stall | app 444 MB, web 597 MB |
| 24,989 records (`miner`) | 1.7 s stall | 3.0 s reload, 1.2 s stall | 0.5 s stall | app 539 MB, web 866 MB |

  * Ten blocks in a row: a worst stall of 0.5 to 0.8 s each.
  * The search field (`Transactions.svelte:212`) is not debounced: typing 7 characters took
    2.3 s.
  * The list itself is virtualized: scrolling and sorting take about 0.3 to 0.5 s.
* **At 100k transactions:** scaling linearly, that is several seconds of frozen UI per block
  and more than 3 GB in the web process. During a catch-up with blocks arriving every second,
  the UI would be frozen most of the time.
* **chains-qt:** `TransactionTableModel` updates single rows (`updateTransaction`), and the
  Overview uses a filtered proxy.
* **Fix:**
  * send incremental changes (a transaction id plus its new status), with statuses refreshed
    for the visible rows only;
  * give the Overview a dedicated host call that returns the latest records;
  * debounce the search.

## Medium

### M1. Four long bridge calls freeze the whole interface
* **Cause:** the bridge has 4 worker threads (`bridge.h:82`) shared by everything.
* **Measured:** with 4 `waitfornewblock 8000` calls running, `getblockcount` took 7,805 ms
  instead of 35 ms (`audit/pool.js`).
* **Real triggers:**
  * a sidechain download (`curl` with a 3600 s timeout, `sidechainnodes.cpp`);
  * a timestamp search;
  * `scantxoutset` (Proof of funds, Multisig);
  * console commands such as `rescanblockchain` or `importdescriptors` with a rescan.
* **Fix:** a separate pool, or detached tasks, for host calls that can run long; or limit the
  long calls per kind.

### M2. Deposits, stamps, multisig signing and proofs leave the wallet unlocked for up to 10 minutes
* **How it works:** `withWalletKeys` (`lib/dialogs.svelte.ts:159`) calls
  `walletpassphrase <pass> 600`, runs the work, then `walletlock` in a `finally` that runs in
  the page.
* **When the wallet stays unlocked:** if the page reloads (`web-process-terminated` reloads
  it, `window_gtk.cpp`), crashes, or the call hangs on a busy pool (M1). For the rest of the
  10 minutes, any RPC client (`-server`) can spend.
* **Used by:** `Sidechains.svelte:166`, `Timestamp.svelte:53`, `Multisig.svelte:212`,
  `Proof.svelte:42`.
* **chains-qt:** `UnlockContext` is scoped in the process. The send, receive and PSBT pages
  here already do that right with the host's `UnlockScope`.
* **Fix:** route these four through host methods that use `UnlockScope`.

### M3. The page can do anything with the node: raw RPC without limits, and `node.setSetting`
* **Not found:** no XSS. The CSP holds (see the end of this document).
* **Defense in depth is missing:**
  * `node.setSetting` (`app.cpp:832`) writes any key of `settings.json`, for example
    `blocknotify` or `walletnotify`, which run commands at the next start. The page never
    uses it (it is only declared in `lib/rpc.ts:184`).
  * Raw RPC is open to the page with any method. `backupwallet` and `dumptxoutset` write to
    any path, which bypasses the rule "files only chosen by the user" of `file.write`.
* **Fix:**
  * remove `node.setSetting`;
  * give `kind: rpc` an allowlist of the methods the pages use;
  * keep the console as the only path to arbitrary commands (it already runs on the host).

### M4. Not usable below about 760 px wide
Tested at 700 × 640 and 480 × 640 (`shots/w700-*`, `shots/w480-*`):
* **Menu bar:** it is clipped, with no overflow menu. At 700 px "This node" is cut; at 480 px
  "Chains", "Crypto tools", "Window" and "This node" are off-screen.
* **Sidebar:** it never collapses by itself (only to 12 rem, `Sidebar.svelte:189`).
* **Transactions:** the Amount column is off-screen at 700 px.
* **Status bar:** it hides the block height and how far behind the node is
  (`StatusBar.svelte:307`).
* **Pages:** the Mine panels, the Settings tabs and the Reset button, and the Wallets buttons
  are clipped at 480 px.

### M5. Text contrast fails WCAG AA (4.5:1) in every theme
* **Method:** contrast measured on rendered pixels, 18 pages × 5 themes (`audit/tour.js`).
* **Failing text nodes per theme:**

| Theme | Failing text nodes |
|---|---|
| light | 176 |
| dark | 100 |
| ocean | 97 |
| matrix | 96 |
| classic dark | 122 |

* **Faint and caption text** (sidebar section titles, secondary lines, hints) is the main
  cause:

| Theme | Contrast |
|---|---|
| light | 3.11 |
| matrix | 3.65 |
| dark | 3.82 |
| classic dark | 3.88 |
| ocean | 4.04 |

* **Worst single cases:** the HD badge of the classic dark theme at 2.34, the `regtest` badge
  of the light theme at 4.26 (11 px), and negative amounts in classic dark at 4.31.

### M6. The interface is English only; chains-qt ships about 100 translations
* `src/qt/locale` holds about 100 languages.
* `-lang` and the language option are accepted and saved, but have no effect (A04 and N05 are
  partial). For users of a translated chains-qt, this is a regression.

### M7. Settings apply at once, with no Cancel
* Each control writes to `settings.json` as it changes (`Settings.svelte:160`, and every
  other `set(...)` call).
* One stray click on "Prune block storage" persists pruning: at the next start, blocks are
  deleted, and undoing it means downloading the chain again.
* **chains-qt:** the options dialog applies on OK and throws everything away on Cancel.
* **Fix:** either confirm the risky options (prune, dbcache, proxy), or keep pending changes
  with Apply and Cancel.

## Low

* **L1. Wallet pages without a wallet show a developer placeholder.** The text reads "This
  page is being rebuilt for the new interface… Parity items: G01, G02…"
  (`Planned.svelte:16` via `Shell.svelte:177`).
* **L2. Table headers are misaligned with their columns.** The offsets grow per column
  (+8, +17 and +25 px); the Amount header ends 14 px left of the values. Seen in
  Transactions and the Block explorer (`webui-shots/transactions-light.png`,
  `explorer-block-dark.png`).
* **L3. The sync overlay never opens by itself.** chains-qt shows it at start while catching
  up, unless the user closed it (`bitcoingui.cpp:1522`, `modaloverlay.cpp:181`). Its line
  "Attempting to spend … not-yet-displayed transactions will not be accepted" is also missing.
* **L4. Send form validation:**
  * Dust is not checked in the form (chains-qt: `SendCoinsEntry::validate` with
    `GUIUtil::isDust`). The user gets "Transaction creation failed! Transaction amount too
    small" from the wallet instead.
  * Amounts with too many decimals, negative amounts, or more than 21M all give the toast
    "The amount to pay must be larger than 0.". The message under the field is right.
* **L5. A custom fee that is not valid falls back silently to the smart fee.** If the custom
  fee is below the minimum or cannot be parsed, `customFee` becomes `null`, and `feeChoice`
  (`Send.svelte:65`) uses the smart fee while "Custom" is still selected. Found by reading
  the code. In chains-qt, the spin box cannot go below the minimum.
* **L6. No external-signer hints on the Send button.** chains-qt disables it with the hints
  "Set external signer script path in Options -> Wallet" or "Connect your hardware wallet
  first." (`sendcoinsdialog.cpp:195-205`).
* **L7. A reindex shows the wrong label.** It reads "Processing blocks on disk…"; chains-qt
  says "Indexing blocks on disk…" for a reindex (`sync.ts:50`, `bitcoingui.cpp:1462`).
* **L8. The intro is missing some of chains-qt's information.** There is no "sufficient to
  restore backups N days old" for pruning, and none of the path messages of
  `freespacechecker.cpp` ("Directory already exists. Add /name…", "…is not a directory").
* **L9. Keyboard gaps:**
  * Ctrl+W does not close dialogs (`guiutil.cpp:427`);
  * Ctrl+I/T/N/P work only while the node window page shows (chains-qt: from anywhere,
    through the Window menu);
  * table columns cannot be sorted from the keyboard (headers have `tabindex=-1`,
    `DataTable.svelte:124`).
* **L10. Content overflows or is covered:**
  * the "After fee" chip of coin control spills out of its box (`coin-control-light.png`);
  * stacked toasts cover the rows and amounts at the bottom right (`transactions-light.png`).
* **L11. Debug-only options are read from `chains.conf` too.** `-webuitestscript` runs any
  JavaScript with `test.fileAnswer`, which gets around the file chooser. This is not an
  escalation, since `chains.conf` can already set `blocknotify`. Still, make the test options
  command-line only, or leave them out of release builds.
* **L12. Passphrases pass through memory that is never wiped** (the webview argument string,
  `UniValue`, the JS heap) before `Secure()`. This is comparable to chains-qt's `QString`;
  noted only.

## Already open in the parity list (not counted)
* A14: blocking session end on Windows.
* A15: App Nap and the dock on macOS.
* B09: the Windows registry and the macOS `Info.plist`.
* C16: the themed window frame.

---

## Checked and found sound
* **CSP:** checked in the app (`audit/probe.js`). Inline scripts, `eval`, `new Function`,
  inline `<style>`, remote images and `fetch` are all refused, and the app itself produces no
  CSP violations on any page.
* **Navigation:** `location.href = https://…` is refused. New windows and permission requests
  are denied.
* **Devtools:** the inspector and the context menu are off by default.
* **No XSS:**
  * no `{@html}`, `innerHTML` or other HTML sinks in the app source;
  * wallet names, labels, transaction comments, and sidechain titles and descriptions
    containing `<img onerror>` or `<script>` are shown as text (`shots/multi-xss-*`).
* **URIs:**
  * `chains://` is refused, and an unknown `req-` parameter is refused;
  * a bad amount is refused, and so is an address of another network;
  * a URI only fills the form; it never sends.
* **Files and links:** files are read or written only after the user chose them;
  `app.open` opens only http(s) addresses, the debug log and the configuration file.
* **Errors at init:**
  * `-prune` with `-txindex`, a P2P port already in use, and a corrupt wallet file each give a
    modal error, then exit 1;
  * pressing q while wallets load stops cleanly in 4 s; SIGTERM while they load stops cleanly
    in 3.3 s.
* **Wallets:** zero wallets, several wallets, a watch-only and an encrypted wallet, and
  unloading the current wallet by RPC all behave.
* **Messages kept from chains-qt:** the send errors (duplicate address, over the balance,
  maximum fee), the passphrase flow (cancel, a wrong passphrase, the 1024 limit, fields
  cleared), the console warning about scammers and its history filter, and the PSBT status
  texts all match chains-qt.

---

## Phase 5: how each finding was closed

Every finding is fixed on branch `webui`, with a test. `test_chains-app`, `npm test` and the
end-to-end tests (smoke, wallet, node, and `run_audit.py`, made for these findings) pass.

| Finding | Fix | Tests |
|---------|-----|-------|
| C1 `-disablewallet` aborts | The wallet functions check for a wallet loader and answer -18; without wallets there are no wallet menus, no wallet pages in the sidebar, and the wallet pages say why | walletapi_tests `wallets_disabled`; audit-ui; run_audit `audit_nowallet.js` |
| H1 text that is not UTF-8 | Every answer and every event batch is made valid UTF-8 (U+FFFD, maximal subparts) in the bridge and the event hub | hardening_tests `valid_utf8`, `answers_and_events_are_valid_utf8`; run_audit: a block built by hand with the proposal title of the audit |
| H2 the whole history on every block | The history is loaded once and kept up to date in parts (`wallet.transactions` with `txids` / `latest`); the status follows the height in the page; the overview asks for the newest records and a chart made by the host; raw state; the search waits for a pause and keeps its text per record. The wallet of 24,989 records of the audit: overview stall 106 ms (was 1.7 s), a new block 128 ms (was 1.2 s), ten blocks 251 ms at worst (was 0.5–0.8 s each), web process 365 MB (was 866 MB) | walletapi_tests `history_in_parts`; history.test.ts; run_audit (400 records: a block asks for the changed records only, small answers) |
| M1 four long calls block the rest | A lane of six workers for the calls that may take long (console, downloads, file choosers, wallet loading, `scantxoutset`...), apart from the four quick ones | hardening_tests `long_calls_do_not_hold_up_quick_ones`; run_audit (eight `waitfornewblock`) |
| M2 wallets unlocked for 10 minutes | `wallet.withKeys`: the host unlocks for the calls alone (UnlockScope); the page never calls `walletpassphrase` for them | walletapi_tests `keys_for_one_call`; run_audit (a proof of funds with a locked wallet, locked again at once) |
| M3 the page can run anything | An RPC policy of the methods the pages use (a frontend test checks the sources against it); `node.setSetting` removed; the console stays the way to any command | hardening_tests `page_rpc_policy`; rpcpolicy.test.ts |
| M4 narrow windows | Below 760 px the sidebar of the dashboard is a drawer (with the pages and the Menus button, whose menus open inside the window); heads, tabs, rows and the status bar wrap, grids shrink, the transactions table fits; the name of the wallet keeps room of its own in the card of the sidebar | audit-ui; run_audit at 700 and 480 px (no page overflows, amounts in view, menus inside the window) |
| M5 contrast | New text tokens (faint, accent text, links, amounts, badges) reach 4.5:1 on every surface of every theme; `--accent-text` for text | contrast.test.ts; run_audit measures what nine pages draw in five themes: no text below 4.5:1 |
| M6 English only | The catalogs of chains-qt (about 100 languages) made into catalogs of the app; the language chosen as chains-qt does; the page translated where it shows its texts, the node's messages through `G_TRANSLATION_FUN`; right-to-left languages | i18n.test.ts; i18n_tests; run_audit `-lang=fr` |
| M7 no Cancel | Apply and Cancel on the Settings page; the host keeps what each change replaced (`OptionsJournal`) and takes back at the next start what was never applied; risky changes are confirmed | options journal tests; audit-ui; run_audit (Cancel, and the next start) |
| L1 placeholder pages | A page that needs a wallet offers to create or open one | audit-ui |
| L2 headers out of line | The rows took the gap of the `.row` of base.css; the arrow of an amount header goes before its label | audit-ui; run_audit measures the headers |
| L3 sync overlay | Opens by itself while behind, until the user closes it; goes once caught up; a layer over the pages | audit-ui; run_audit |
| L4 send validation | Dust refused with the node's rule; the message of the amount field in the notice | walletapi_tests `dust`; audit-ui |
| L5 custom fee | A custom fee that is not valid stops the send with a message; never the smart fee in its place | audit-ui |
| L6 external signer hints | "Sign on device" disabled without `-signer`, with the hints of chains-qt | audit-ui; run_audit with the mock signer |
| L7 reindex label | "Indexing blocks on disk…" while the headers run ahead | audit-ui |
| L8 intro messages | The messages of FreespaceChecker, and "(sufficient to restore backups N days old)" | audit-ui |
| L9 keyboard | Ctrl+W closes the dialog on top; Ctrl+I/T/N/P from anywhere; headers focusable | audit-ui |
| L10 overflow | Coin control amounts wrap; two notices at most, the others behind them | audit-ui |
| L11 debug options in chains.conf | Taken from the command line only; the test options and the test methods of the bridge are compiled only into a build for the tests (`WEBUI_TEST_HOOKS=ON`), never into a release | run_audit: `webuidevtools=1` in chains.conf is ignored; ctest `webui_test_hooks_built` |
| L12 passphrases in memory | Into secure strings straight from the request; the request and its copies wiped after use | hardening_tests `secrets_are_wiped` |
| Review 2026-10-14, item 5: gate parity (proxy, Tor proxy, RPC server) | Like `-signer`, these are no longer free settings of the page: turning the proxy or the Tor proxy on, changing the address of one in use, or turning the RPC server on takes a confirmation of the desktop (`Window::Confirm`, `NodeOptions::ConfirmFn`) that the page can neither draw nor answer, saying exactly what changes ("Route all connections through 203.0.113.5:9050?", "It is … now", "Turn on the RPC server?"); declined, nothing changes (`declined: true`). Every address is asked, local ones too. Turning them off asks nothing. A proxy address with a user name or password (`user:pass@`) is refused. The address and the port of a proxy are changed at once (`proxyAddress` / `torAddress`): one question. Without a way to ask (no `ConfirmFn`) the change is refused. The changes of the options are serialized from the question to the write (`NodeOptions::ChangesMutex`, also taken by Cancel and the reset): a change made while a dialog is open waits for it, and what is turned on is the address the user was shown (checked again after the answer) | nodeapi_tests `node_options` (declined: unchanged; accepted: set; the text of each question; off asks nothing; userinfo refused; refused without a confirmation; a concurrent change during an open confirmation waits and cannot change what is turned on); `test.confirmAnswer` answers the dialog only in a build with `WEBUI_TEST_HOOKS` |

The four items already open in the parity list: C16 (the themed frame) is done. Windows is built
and tested (window_win32.cpp; `run_windows.py` runs the unit tests and the page scripts on
Windows through WSL): A14 (the session end waits for the node) is done, and so is the Windows
part of B09 (`chains:` registered for the user, and by the installer). A15 (App Nap and the dock)
and the macOS part of B09 belong to the macOS port, which is not built nor tested yet.
