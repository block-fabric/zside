# chains-app: independent verification (branch webui-windows)

> **History.** chains-qt (`src/qt`) was removed from the tree by the commit "Remove the Qt GUI"
> (`git log --grep='Remove the Qt GUI'`), after the work this document records: chains-app is the GUI of the
> build. This document stays as the record of the parity; the `src/qt` sources it cites are those
> of the parent of that commit (`git show <commit>^:src/qt/<file>`).

Checked on 2026-10-14, at `e0d9800895` (after the merge of `webui` and the three security fixes),
with template `4c516d7e49` and chains `6ec9f92df7`. Everything was rebuilt incrementally first.
This round changed no code: only this report.

## Results

### Test suites

| Suite | Result |
|-------|--------|
| vitest (src/webui/app) | PASS: 166/166, 21 files |
| svelte-check `--fail-on-warnings` | PASS: 448 files, 0 errors, 0 warnings |
| test_chains-app (Linux) | PASS, but **flaky**: 1 failure in 9 full runs (see "Failed") |
| page_rpc_policy alone | PASS 25/25 |
| e2e smoke / wallet / node | PASS: 20, 36 and 69 checks |
| e2e audit (7 runs) | PASS: audit 24, restart 2, nowallet 15, narrow700 22, narrow480 22, signer 7, lang 4 |
| e2e layouts / titlebar / refresh | PASS: 3 (and 53 pictures), 21, 4 |
| Windows `run_windows.py` (WSL interop) | PASS: test_chains-app.exe, smoke 20 (URI handed over a named pipe), layouts 3, titlebar 21, refresh 4. Ran in `%TEMP%\chains-app-test` with `-webuiregisteruri=0`. The folder was removed afterwards and the demo folder was not touched |
| chains: ctest (test_bitcoin, secp256k1) | PASS: 379/379 (script_assets_tests skipped, as usual) |
| chains: test_bitcoin-qt, offscreen | PASS: every QTest class |
| template + 6 sidechains: test_bitcoin (ctest) | PASS: 381 / 384 / 383 / 383 / 384 / 383 / 382 |
| template + 6 sidechains: feature_sidechain* | PASS: all 7 tests in each (8 in bitassets, which adds payouts), with mainchain `chains/build/bin` |

### The user's decisions

| Decision | Test | Picture | Verdict |
|----------|------|---------|---------|
| Dashboard is the default layout | layouts.test.ts; refresh.js "the dashboard is the layout of a first start" | refresh/dashboard-home-midnight; on Windows, window-frame.png | PASS |
| Midnight is the default theme | contrast.test.ts; refresh.js "Midnight is the look of a first start" | same | PASS |
| Chain before tools | layouts.test.ts (menu groups, sidebar sections, workspace areas) | the sidebar and the Menus button | **PARTLY**: see "Failed" 2 |
| Integrated title bar with minimize, maximize and close | titlebar.js (21 checks, Linux and Windows) | titlebar-*, window-frame.png (Windows) | PASS |
| In Compact, the logo grows the sidebar | — (the compact sidebar is gone: user decision of 2026-10-15) | — | REMOVED |
| All functions of chains-qt kept | parity list: 118/120 `[x]`; A15 (macOS) `[ ]`; B09 `[~]` (macOS) | | PASS, apart from macOS |
| No credits to the inspiration | nothing under src/webui, doc/ or README names it; the About box and doc/webui.md list only the licences (webview, Svelte, QDarkStyleSheet, fonts, translators) | | PASS |

### Parity sample (30 items picked at random with seed 20261014, checked against src/qt)

A08 B02 B05 C03 C07 C12 C13 C15 D02 D12 E01 E02 E06 E10 E12 F02 G03 H01 I03 I06 J01 K02
L04 L06 L07 L09 M02 M03 N05 T03.

- 30 of 30 do what chains-qt does. Checks included: NetworkStyle hues 70/30, 35/15 and 160/30
  on the window and tray icons (GTK and Win32); the `chains://` message; every item of the Qt
  "Your Wallet" menu; history disabled while values are masked; the HD and lock icons; the
  create-wallet options; the context menu of the transactions; the console's list of sensitive
  commands; the traffic ranges from 5 minutes to 24 hours; font sizes from -6 to +12.
- **Wrong in the document, not in the code:**
  - **L04** says downloads are "https/http only", but the code now takes https only.
  - **D12** cites `Send.svelte` for an out-of-sync warning. chains-qt shows that warning on the
    overview only, and the send page shows the fee-estimation warning, as Qt does.
  - **N05** still mentions "the themed frame (C16)". It now means the title-bar option under
    Appearance.

### Audit items (doc/webui-audit.md)

All 22 findings are listed as closed: C1, H1–H2, M1–M7 and L1–L12. Every test the closure table
names exists. The e2e audit runs cover C1, H1, H2, M1, M4, M5, M6, L3, L6 and L11, and all pass.
Spot checks: `Planned.svelte` is gone (L1), `node.setSetting` is gone (M3), and `-webuidevtools`
and `-webuitestscript` are taken from the command line only (L11).

Two parts of the document are out of date:
- **M4** still says "menus fold into More". The merge removed that; on narrow windows the menus
  are now in the sidebar drawer. The narrow-window e2e runs still pass.
- **The last paragraph** says the Windows window "is not built nor tested yet". It is now built
  and tested, and A14 is `[x]`.

## Security findings: attempts to break them

### 1. RPC password on the command line: CLOSED

- chains-app and chains-qt pass only `-mainchainrpcpasswordfile=<sidechain datadir>/mainchain.rpcpass`.
  No caller passes `-mainchainrpcpassword=` any more (grep over chains-app and chains src/qt).
- `WriteSecretFile` creates the file with `O_EXCL|O_NOFOLLOW` and mode 0600, fchmods it, fsyncs
  it and renames it over the old one. Where the file goes is safe: the stem is limited to
  `[A-Za-z0-9_-]`, so `../evil` is refused, and the file name is fixed.
- On the reading side (template and all six sidechains), the file must be a regular file, owned
  by this user, with no group or other bits, of at most 4 KB, with a non-empty first line. Setting
  both the password and the file is refused, and so is the file without `-mainchainrpcuser`
  (feature_sidechain_record passes in all seven repos).
- Left open, as documented: no ACL check on Windows, where the file is written with default ACLs.
  Minor: `open()` without `O_NONBLOCK`, so a FIFO given by the user blocks startup.

### 2. Downloads and the signer: CLOSED for what was reported, with one weakness left

- An address that does not start with https, a hash that is not 64 hex characters, a URL with
  quotes or whitespace, a declined dialog, or a wrong hash: each one stops before curl or tar
  runs. curl also gets `--proto =https --proto-redir =https`.
- The `test.confirmAnswer` and `test.fileAnswer` hooks exist only with `-webuitestscript`, which
  is taken from the command line only. The page cannot answer the dialog.
- The page can no longer set `-signer` (`options.set` with a value is refused; only clearing it
  works). The `*notify` options are not settable. The options journal (Cancel) is kept outside
  the page's `prefs.set` area, so a forged undo entry cannot restore a signer. No RPC in the page
  policy can run a program.
- **Weakness left (LOW–MEDIUM):** the page can make the confirmation dialog misleading.
  `sidechainNodes.add` accepts any `name`: it is only trimmed, with no length limit and newlines
  allowed. That name goes unchanged into the title and the first line of the native dialog
  ("Download and install programs of <name>?"). A hostile page can add a sidechain whose name
  holds fake "From:/SHA-256:" lines and many blank lines, which push the real address and the
  line "an address typed in the app" out of view.
  - **Fix:** limit names to one line of about 64 printable characters, and put the real URL
    first in the dialog.

### 3. Translator: CLOSED

- Nothing scans the page: no MutationObserver or TreeWalker in i18n; the only MutationObserver
  left is the sync overlay's check for an open dialog. No `{@html}`, `innerHTML` or `eval` in
  the page.
- Speed: no regular expression matching any more. A probe with the real French catalog:
  100 × 10 KB texts took 2.9 ms, and 1000 × 300-character texts made of " and " repeats took
  17 ms.
- Chain, peer and user text goes through SafeText (`translate="no"`), and i18n-lint guards it.
- **Leftover (LOW, not a vulnerability):** `tx()` still runs on messages the app builds, such as
  toasts, dialogs, the fatal view, and PSBT/sign statuses. Those messages can carry outside text
  (host errors, curl stderr, RPC messages). Generic catalog patterns then rewrite them partly. In
  French:
  - "%1 and %2" turns "The name of the sidechain and of its programs are needed." into
    "… sidechain et of its programs …";
  - "%1 behind" turns "3 hours behind" into "en retard de 3 hours".

  The values are spliced in unchanged, so nothing can be spoofed, but the wording is broken.
  - **Fix:** drop patterns whose literal parts are under ~6 characters from `tx()`, or translate
    with `t()` at the place the message is built.

## Failed

1. **The bridge tests are flaky.** One full test_chains-app run in nine failed with
   `webui_bridge_tests/cancel: bridge.Pending() == 0 [1 != 0]` (bridge_tests.cpp:317). Cause:
   in `Bridge::Handle`, a worker calls `Reply()` before `Finish()` removes the request. A test
   that reads `Pending()` as soon as it has the reply can still see the request. This is the
   same race as the earlier one-off `page_rpc_policy` "Pending" failure. Only tests read
   `Pending()`, so the app is not affected.
   - **Fix:** in the tests, wait until `Pending()` reaches 0. Alternatively, finish before
     replying.
2. **Chain before tools is not followed on the Home quick actions** (views/home/Home.svelte:203–209).
   The order is Send, Receive, **Sign message, Load PSBT**, Deposit to a sidechain, Block
   explorer, Mine. Sign message and PSBT are tools: they belong to the Tools sidebar section, and
   Sign message is in the Tools menu. No test checks this row.

## Missing

- macOS: A15, and the macOS part of B09; the window keeps the desktop's frame. None of it is
  built or tested.
- No automated test covers the order of the Home quick actions, or a sidechain name holding
  several lines in the download dialog.
- The built-in release list is empty, so only addresses the user types can be downloaded.
- The "Use the title bar of the desktop" option applies at once, not on Apply (known, deferred).
- Document fixes are needed: parity L04, D12 and N05; audit M4 and the closing paragraph.

## Where things are

Logs and pictures of this round are in the session scratchpad, under `verify/`: `e2e_*.log`,
`t_*` and `shots/{smoke,wallet,node,audit,layouts,titlebar,refresh,windows}`.

---

# Round 2: the fixes, and the verdict

Checked on 2026-10-14 at `2a30fbd45c` (six commits on top of `5d447f3b00`). Linux build
`build_linux`, Windows build `build_win`, both configured with `-DWEBUI_TEST_HOOKS=ON` (a build for
the tests; see "Test hooks" below).

## What "Failed", "Missing" and the security leftovers of round 1 became

| Round 1 | Fix | Test | Status |
|---------|-----|------|--------|
| Failed 1: flaky bridge (`Pending()` seen at 1 after the answer) | `Bridge::Reply` removes the request from the pending list before the answer goes out, on every path; the stop and duplicate-rid answers are sent outside the lock. The tests were not touched | new `request_gone_before_its_answer` reads `Pending()` inside the reply callback for every kind of call, 50 rounds; test_chains-app run **30 times in a row: 0 failures** | CLOSED |
| Failed 2: Home quick actions put tools before the chain | Send, Receive, Deposit to a sidechain, Block explorer, Mine, Sign message, Load PSBT (`HOME_QUICK_ACTIONS`, lib/layout.ts) | `chainBeforeTools()` helper (kind of a page = its sidebar section), applied in layouts.test.ts to the quick actions, the sidebar sections, the workspace areas, "Go to", and each menu (plus the order of the menus); the row as the dashboard draws it | CLOSED |
| Security 2 leftover: confirmation spoofing (audit item 4) | Names: one line, at most 64 printable characters, no control/format/bidi characters; refused at `sidechainNodes.add`, cleaned up (`DisplayName`) wherever shown. URLs: `user@host` refused, host must be `[A-Za-z0-9.-]` (+ numeric port). Dialog: the title is the trust statement in fixed words decided by the host ("Install a release built into this app" / "Install from an address typed in this app"); line 1 is `Host: <parsed host>` alone; line 2 the trust sentence; the page's name and address only after | nodeapi_tests `sidechain_nodes`: names with `\n`, `\r\n`, tab, U+202E, U+2028, 65 chars refused; a tampered name in the settings shown on one line, cut with "…"; `DownloadHost` cases; two userinfo URLs refused before any dialog; exact shape of both dialogs (title, line 1, line 2, then `Sidechain:`/`From:`) | CLOSED |
| Test hooks only gated by `-webuitestscript` | CMake option `WEBUI_TEST_HOOKS` (OFF by default): `-webuitestscript`, `-webuitestshots`, the `test.*` methods and the answers of the dialogs are compiled only with it; `deploy-app` refuses a build that has them | ctest `webui_test_hooks_built` (the strings are in the program exactly when the option says); `app.cpp` compiled with the option OFF: none of `test.confirmAnswer`, `test.fileAnswer`, `-webuitestscript=<file>` in the object | CLOSED |
| Security 3 leftover: translator garbles host/RPC messages | No pattern matching left: `t()`/`tn()` look up only the source of a message written in the app, as a whole; `tx()`/`translateText()` removed. Toasts, dialogs, statuses, the fatal view show what they get as it is (SafeText); the app's words in them go through `t()` where made, with their own placeholders (codemod over every toast/inform/confirm/prompt/ask, statuses by hand). Helpers with values translate themselves (sync status, `niceTimeOffset` with `tn('%n hour(s)')` and `'%1 and %2'`, transaction status/type, ping, rates, ranges, sidechain summary, votes). Lint: fails on text put together given to `t()`, and on app words put in a toast/dialog without `t()`, in components and lib/ | i18n.test.ts, i18n-safety.test.ts in French: a host error with " and " in a toast and a dialog stays whole, title "Paramètres"/"Erreur"; "en retard de 3 heures", "1 an et 4 semaines", "Confirmée (7 confirmations)"; `t('%1 hours behind', 3)` stays English instead of half French; e2e audit_lang.js (`-lang=fr`): a real host error in a notice stays as the host wrote it | CLOSED |
| Missing: "Use the title bar of the desktop" applied at once | A change of the draft: ticked, counted as waiting, made by Apply, dropped by Cancel/Discard | titlebar.test.ts (no call before Apply, one after, none on Cancel); titlebar.js and audit.js check the window keeps its title bar until Apply, on Linux and Windows | CLOSED |
| User's screenshot: wallet card shows "(" on Windows (Midnight) | The name has a minimum width (5 rem) with an ellipsis and the whole name as tooltip; the unit pill is narrower and goes under the name when both do not fit; the picker too | walletcard.js: one wallet and two (picker), "demo" and a 33-character name, sidebar 13/15/17 rem, text +0/+2, page at 100/125/150 % (18 cases each): 90 checks on Linux and 90 on Windows | CLOSED |
| Docs: parity L04, D12, N05; audit M4 and the Windows paragraph | L04 https only + no user@host + the dialog shape + name rule; D12 says what the overview and send pages really show; N05 names the title-bar option (waits for Apply) and the language (applied with Apply); audit M4 describes the drawer; the closing paragraph says Windows is built and tested (A14 and the Windows part of B09 done), macOS left; L11 notes the compiled-out hooks; webui.md documents `WEBUI_TEST_HOOKS` and the new translation rules | review | CLOSED |

## Test suites, round 2

| Suite | Result |
|-------|--------|
| vitest (src/webui/app) | PASS: 173/173, 21 files |
| svelte-check `--fail-on-warnings` | PASS: 448 files, 0 errors, 0 warnings |
| `npm run i18n --check` (catalogs up to date) | PASS (catalogs made again for the new messages) |
| test_chains-app (Linux), 30 runs in a row | PASS 30/30 (about 20 s each) |
| ctest `webui_test_hooks_built` | PASS |
| e2e smoke / wallet / node | PASS: 20, 36, 69 checks |
| e2e audit (7 runs) | PASS: audit 25, restart 2, nowallet 15, narrow700 22, narrow480 22, signer 7, lang 7 |
| e2e layouts / titlebar / refresh / walletcard | PASS: 3 (53 pictures), 22, 4, 90 |
| Windows `run_windows.py` (WSL interop, `%TEMP%\chains-app-test`, `-webuiregisteruri=0`) | PASS: test_chains-app.exe (no errors, 120 s), smoke 20 (URI over the named pipe), layouts 3, titlebar 22, walletcard 90. The folder was removed afterwards; `%TEMP%\chains-app-demo` was not touched |
| chains ctest / test_bitcoin-qt; template + 6 sidechains | Not re-run: nothing outside `src/webui` and its docs changed in this round, and chains (`6ec9f92df7`) and the sidechain repositories are at the same commits as in round 1, where they passed |

First runs of the new e2e checks found two faults in the tests themselves, fixed in
`2a30fbd45c`: the second click of the title-bar option came before Apply was enabled again, and
walletcard.js measured the name with `scrollWidth`, which rounds under a CSS zoom.

Logs and pictures: session scratchpad, `fix/` (`loop_*.log`, `e2e_*.log`, `shots/`).

## Still open (not blockers)

- macOS: A15 and the macOS part of B09 (the macOS port is not built nor tested).
- The built-in release list is empty: only addresses the user types can be downloaded (now with
  the parsed host and "typed in this app" said first, in fixed words).
- Messages written by the host (errors, the transaction details, PSBT statuses) are shown in
  English in every language: they are never matched against the catalog any more.
- The Banking menu keeps the chains-qt items Proof of funds, Multisig and Load PSBT (tools); within
  each menu and across the menus the chain comes before the tools.

## Verdict

Every failure, every missing item and every security leftover of round 1 is fixed, each with a
test, and every suite above passes on Linux and on Windows.

READY: yes

