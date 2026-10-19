# chains-app: the layouts of the main window

The main window of `chains-app` comes in two layouts, switched live in
**Settings → Appearance → Layout** (or from the command palette: "Layout: …"),
and kept in the GUI preferences of the app (`prefs.layout`, per network, in the
app settings file next to the theme and the unit). The pages are the same in
both: only the navigation, the menus and the frame around the page change.

**User decisions (2026-10-15):** the **Compact sidebar** layout is gone (its frame, its icon rail,
`prefs.sidebarExpanded`, its denser tables, its palette commands); a layout kept as `sidebar` (or any
layout not known) is read as the dashboard. **Home** is in every layout: the first page of the
dashboard's sidebar and the first area of the workspace, and every layout starts on it after
launch (switching the layout keeps the page shown).

| Layout | Id | What it looks like |
|--------|----|--------------------|
| Workspace tabs | `workspace` | **Home** then five areas across the top — **Wallet**, **Chain**, **Sidechains**, **Tools**, **Node** — each with a strip of sub-tabs (Home: the Home of the dashboard, its cards, tables and the Send/Receive side panel; Wallet: Overview, Send, Receive, History, Manage wallets, Advanced; Chain: Explorer, Mine; Sidechains: Sidechains, Sidechain nodes; Tools: the one Tools page; Node: Console, Peers, Info, Traffic, Settings). What concerns the chain comes before the tools, here as in the sidebar and the menus. The wallet selector, the palette field and a **Menus** button sit at the right of the top bar. A **detail panel** opens at the right of the page when a row is selected (a transaction in the history; blocks, peers and sidechains as their pages publish them). The status bar stays. |
| Dashboard | `dashboard` (default) | A **Home**: a row of quick actions; four summary cards — **Balance** (available, pending, immature; Send, Receive), **Network** (sync, height, peers, mempool; Peers, Console, network on/off), **Sidechains** (active, pending bundles, escrow, proposals; Deposit, Votes), **Mining** (hash rate, difficulty, last block, BMM; Solo mine, Timestamp); the **Wallet balance** chart (timeframes 1D 7D 1M 1Y All, its change, axes); **Blind merged mining** (blocks of the last 24 that committed to each sidechain); then tables in tabs — **Transactions** (Details, Bump fee, Copy), **Sidechains** (Deposit, Withdrawals, View), **Blocks** (View, Copy), **Peers** (View) — with sortable headers, status pills and small trends. Each card's arrow opens its full page; the name of a row opens the detail panel. **Send** and **Receive** open a side panel at the right of the home (one recipient and a big Send button; a new address with its QR code); the Send and Receive pages stay for the rest (several recipients, choosing the coins, PSBT, the requests, the receiving addresses). The sidebar: a card of the wallet shown (its name, its lock, the unit) at the top, every page (Home first) in groups, the theme, the text size and the **Menus** at its foot, then Close wallet and Quit apart. The bar above the page: where you are, the palette, the wallet, the notifications, the light/dark switch. On a narrow window the sidebar is a drawer. |

Whatever the layout:

- **Command palette** — Ctrl+K (Cmd+K), or the search field / icon: every page and
  every item of the old menu bar, found by name (words in any order, also by
  keywords: "history", "password", "zoom"…), with their shortcuts. Up/Down choose,
  Enter runs, Escape closes. It also holds the layouts, the units and the network
  switch of the status bar.
- **Keyboard shortcuts** of chains-qt: Alt+1…5 pages, Ctrl+Shift+D node window,
  Ctrl+Shift+C console, Ctrl+I / Ctrl+T / Ctrl+N / Ctrl+P the node window tabs,
  Ctrl+Shift+M mask values, Ctrl+M minimize, Ctrl+Q quit, Ctrl+ / Ctrl− text size.
- All the looks (Midnight, Light, Dark, Ocean Teal, Matrix Green, classic Drivechain, Catppuccin
  Mocha, Dracula, the desktop's, and the themes of the user) — the layouts use only the colour variables of styles/themes.css (see "The looks").
- **Top bar buttons**: the notifications (a bell with a dot while some are new: the transactions
  that came since the app started; one of the wallet shown opens in the detail panel) and the
  light/dark switch (sun, moon: a dark look goes Light, Light goes back to the dark look last
  used, Midnight at first). In the dashboard and the workspace they are in the top bar.
- **Smaller windows** (≤ 1380 px with the integrated title bar, ≤ 1240 px without): the workspace's
  areas other than the current one show their icon only (named in their tooltip and for screen readers).
- **Narrow windows** (phone width, ≤ 760 px): the workspace areas become a row of
  icons under the top bar and the sub-tabs scroll; the dashboard sidebar becomes a
  drawer (☰ in the top bar); the detail panel
  covers the page (≤ 900 px); the palette fills the width.
- **Accessibility**: a "Skip to the page" link first; areas, sub-tabs and the sidebar are
  `nav` landmarks with `aria-current`; arrows move between workspace areas; the
  palette is a modal `dialog` with a combobox and a listbox (`aria-activedescendant`);
  the Menus buttons are `menu`s with arrow keys, Home/End and Escape; icon-only
  buttons have names; the detail panel is a labelled `complementary` region, closed
  with Escape.

## The integrated title bar

**User request (2026-10-14):** "in the top bar of the UI, we should have a minimize, resize screen
and X button". The window has no frame of the system by default: the page draws the title bar.

- **Where**: at the very top in every layout. In the workspace it *is* the top bar (logo, areas,
  wallet, palette, Menus, then the buttons). In the dashboard it is a slim
  strip above the layout: the logo, "Chains" and the network badge, the page title and the wallet
  shown. Before the node runs (the intro, the splash, the error screen) the same strip without a
  page. In the dashboard, the sidebar's own logo goes (it is in the strip).
- **Buttons** at the right: **Minimize**, **Maximize / Restore** (its picture and name follow the
  state of the window, also when the desktop maximizes it), **Close** (red under the pointer or
  focused). Close does what the close button of the system does: minimize if "Minimize on close" is
  set, else the app stops (and stays in the tray when that is set, as before).
- **Drag**: a press on the empty part of the bar (not on a button, link, field, menu or tab) moves
  the window; a double click maximizes or restores it.
- **Resize**: thin invisible handles along the four edges and at the four corners of the page (4 px
  strips, 10 px corners), only when the window is not maximized.
- **Keyboard**: the three buttons are a labelled group ("Window") of named buttons, in the tab order
  after the bar's own controls; Ctrl+M still minimizes.
- **Looks**: the colours of styles/themes.css only (all five looks and the desktop's); the red of
  Close is the same everywhere.
- **The title bar of the system**: Settings → Appearance → "Use the title bar of the desktop" (kept
  in the app settings file as `titleBar`: `integrated`, the default, or `system`, for every network,
  read when the window is made). On X11 it changes at once; on Wayland the setting says "Applies
  after a restart of Chains". With it, the page draws no title bar or handles.

The host side (src/webui/window.h, window_gtk.cpp, windowframe.h):

| Bridge method / event | What |
|---|---|
| `window.state` | `{titleBar, supported, frameless, restartNeeded, maximized}` |
| `window.minimize`, `window.toggleMaximize`, `window.isMaximized` | The buttons |
| `window.close` | As the close button of the system (`close_requested` → `App::CloseRequested`) |
| `window.startMove` | On the press in the bar: the window manager moves the window (`gtk_window_begin_move_drag` from the last press in the page) |
| `window.startResize {edge}` | On the press on a handle: `top`, `bottom`, `left`, `right`, `top-left`, `top-right`, `bottom-left`, `bottom-right` (`gtk_window_begin_resize_drag`) |
| `window.setTitleBar {mode}` | Keeps the choice, applies it when it can; returns the state |
| event `window` `{maximized}` | The window was maximized or restored |

Linux has it (GTK: `gtk_window_set_decorated(false)`), and Windows (window_win32.cpp: no caption
through WM_NCCALCSIZE, the other borders of the system kept for resizing, the shadow and the
rounded corners; `startMove`/`startResize` as WM_NCLBUTTONDOWN with HTCAPTION or the HT of the
edge; a window over the page's maximize button answering HTMAXBUTTON for the snap layouts of
Windows 11). macOS (window_generic.cpp) keeps the frame of the system for now (`supported: false`,
the page hides its bar); the TODO in window.h says how: a hidden title bar keeping the native
traffic lights at the left.

## The looks

**User decision (2026-10-14):** a visual refresh, and a new default look, **Midnight**: a charcoal
base (not black), soft depth, an ambient glow of the accent behind the page, a lime accent. It is
the look of a first start (`prefs.theme` absent); a look chosen before is kept. The five looks of
chains-qt (Light, Dark, Ocean Teal, Matrix Green, classic Drivechain) and the desktop's stay, with
the same effects in their own colours.

**User request (2026-10-15):** two more built-in looks, **Catppuccin Mocha** (from its palette, MIT) and
**Dracula**, red on black (user decision: crimson on near-black, not the purple draculatheme.com palette),
held to the same contrast rules (test/contrast.test.ts); and
**Settings → Advanced theme**, where the user makes themes of their own:

- A theme of the user is a built-in look (its fonts, corners and shadows) with some of its colour
  tokens changed (lib/customthemes.ts `TOKEN_GROUPS`: backgrounds, text, accent, states, light and
  shade). The editor starts from any theme, built-in or the user's; each colour has a colour picker
  and a text field. The theme is drawn on the whole app while it is edited, and the look comes back
  on Cancel. A contrast under 4.5:1 of text on its backgrounds is a warning, never a refusal.
- Saved, renamed, duplicated and deleted (after the confirmation of the app) at once: they are kept
  in the preferences (`prefs.customThemes`, at most 50). Choosing one waits for Apply, as the other
  looks. They are listed with the built-in looks in Appearance, the sidebar and the palette.
- Theme files (JSON: `format` "chains-app-theme", `version` 1, `name`, `base`, `colors`) are
  exported and imported through the file chooser of the desktop (`file.choose`, `file.read`,
  `file.write`: the page never names a path), or copied and pasted as text.
- A theme file is untrusted: at most 64 KB; only those five fields; a name of at most 48
  characters without control or direction marks (shown with SafeText, never translated); only known
  tokens; only colours (#rgb, #rrggbb, #rrggbbaa, rgb()/rgba()/hsl()/hsla() of numbers) — no url(),
  var(), expression, semicolon or brace. Anything else refuses the whole file. The colours are set
  one by one with `style.setProperty` on the known tokens; the chevron of the dropdowns is drawn
  from the parsed muted colour. Stored themes are checked again when the preferences are read.

- **Depth**: the page has a still glow of the accent (two soft radial lights, `--glow`,
  `--glow-2`); cards and panels are big soft surfaces (`--radius-xl`, 20–24 px; small in Matrix and
  classic), lit from above (`--card-top`) with a soft shadow (`--shadow-card`).
- **Amounts**: the decimals are dimmed (in the muted colour, never a transparency):
  **5 050**.00000000 CHN, everywhere (components/Amount.svelte).
- **Chart**: the balance as a line over a gradient, the balance now a lit point; with its axes on
  the home.
- **Pills**: the current page of the dashboard is a filled pill of the accent; the workspace's areas are pills; the state
  of a row is a coloured pill (`.status.ok`, `.wait`, `.bad`); the wallet and the unit are pills.
- **Contrast**: every text colour of every look passes WCAG AA (4.5:1) on the backgrounds it is
  drawn on; `--accent` is a fill, `--accent-fg` the accent drawn as text or icons
  (test/contrast.test.ts reads styles/themes.css).
- **Dropdowns and scroll bars** (user decision, 2026-10-15): our own, the same in WebKitGTK and WebView2.
  Every select has no native face (`appearance: none`) and the chevron of the icons in the muted colour
  of the look (`--chevron`, one per look in styles/themes.css), the padding and radius of the fields or
  pills, a focus ring, a disabled state; the list that opens stays the native one (keyboard, screen
  readers), in the colours of the look where the engine allows (`color-scheme` per look, `option`
  colours: WebView2; GTK draws its own popup). Scroll bars: 10 px, round, `--line-strong` thumb,
  `--text-muted` under the pointer, no track, through `::-webkit-scrollbar` (both engines; the standard
  `scrollbar-width`/`scrollbar-color` only where those are unknown, as Chromium would drop ours).
  test/controls.test.ts checks the rules for every look; `run_layouts.py --script controls.js` checks
  every select drawn and the bars of the page, a long table and the console in the engine (pictures
  `controls-*`).
- **Motion**: only answers to an action (hover, a panel opening); none with
  `prefers-reduced-motion`. No remote fonts or images (the fonts are bundled; CSP unchanged). The
  long tables stay virtual (components/DataTable.svelte).

## The block explorer's ribbon (user request, 2026-10-15)

The Block explorer opens on a ribbon of the latest blocks, after the /blocks page of the user's pool site
(epool.cash, ~/datumpool: BlockRibbon, BlockBox): components/BlockRibbon.svelte, BlockBox.svelte,
lib/blocks.ts.

- **Boxes**: each block a box (a lid and a side as two skewed faces, no 3D), newest at the left, joined by a
  link. The face: the height; the reward (subsidy and fees, `getblockstats`; decimals dimmed); the miner's
  tag of the coinbase (its printable text after the height, text of the chain: SafeText); transactions and
  weight; a **BMM n** badge per sidechain whose block it committed to (`getsidechainevents`); the time since
  the block before it, coloured against the median of the ribbon (fast under half, slow over twice; the
  target of Chains is 1 minute) and how long ago.
- **Fill**: rising from the floor of the face, the weight against the 800,000 a block of Chains may have
  (`CHAINS_MAX_BLOCK_WEIGHT`; none for the coinbase alone, at least 6 % otherwise), tinted by the average
  fee rate (under 2 sat/vB, under 10, more).
- **The block being mined** first, in outline: tip + 1, the transactions waiting and their size and fees
  (`getmempoolinfo`), what it would pay, the expected time at the hash rate of the network (the target when
  that guess is far off); a click opens the waiting transactions.
- A click (or Enter) on a box chooses it: its details below (the detail panel in the workspace); the arrows
  move between the boxes; the strip scrolls sideways, never the page. A block that comes while the page is
  shown slides in once (transform and opacity only; none with reduced motion). Below, the tables of the
  blocks and of the waiting transactions as before.
- Tests: test/blocks.test.ts (fill, tint, interval colour, tag, the box and the ribbon);
  `run_layouts.py --script explorer.js` (pictures `explorer-*` in Midnight and Light, a block with
  transactions, a new block, the workspace's panel).

## Performance (user decision, 2026-10-15)

`src/webui/test/e2e/run_perf.py --app build/bin/chains-app --tmp <dir> --shots <dir>` makes a wallet of
20,000 records (40 transactions of 500 outputs), starts the app again on it with software GL (as the demo
under WSLg) and measures, in the page (perf.js): the start (first paint; the home with the balance; with
the history), each page opened (go() to the frame after it), a full repaint (another look), typing in the
search of the history, scrolling the history and the home (frame lengths), a new block (the longest frame),
what pages ask the host for in 10 s shown and hidden, and the processor time of the app meanwhile. It
fails over its limits (`LIMITS`), so a regression shows (the start is reported, not limited: it is mostly
the node's and the machine's, 0.7 s on an idle machine, up to 27 s on a busy one). Before and after the changes of 2026-10-15, on
the same data (ms unless said; xvfb 1400×900, llvmpipe):

| Measure | Before | After |
|---|---|---|
| First paint / home with the balance / with the history | 215 / 720 / 823 | 205 / 685 / 783 |
| Page opened: median / slowest (the home) | 19 / 52 | 18 / 42 (home 36) |
| Full repaint (another look) | 47 | 42 |
| Search: a key, slowest / rows filtered after 8 keys (250 ms pause included) | 19 / 775 | 20 / 776 |
| History scrolled: frame median / longest (fps) | 19 / 24 (53) | 18 / 25 (56) |
| A new block: longest frame | 25 | 17 |
| Calls in 10 s with the window hidden: Mine / Node ▸ Peers | 12 / 7 | 2 / 2 |
| Processor time in 10 s hidden: Mine / Node ▸ Peers | 40 / 50 | 30 / 30 |

What changed: the timers of the pages (Mine, Peers, Sidechain nodes, the home's peers and clocks, the
status bar, the traffic graph, the Wallets page) rest while the window is hidden and catch up when it is
shown (lib/poll.ts `every`); the sync overlay watches the page and ticks only while it is shown (it
watched every change of the page, each row of a long table scrolled in); the home takes its newest rows
in one pass instead of sorting the whole history, and the history is sorted by its keys as they are, not
by the locale; the ambient light is a layer of its own painted once (not a fixed background of the body,
painted again at each scroll); shadows and filters are no longer animated (colours only). Already in
place: the long tables are virtual, the history is kept in raw state and updated in parts, the search
waits for a pause. The interface is one script and one style sheet on purpose (the inline document of
the other platforms), so it is not split. test/perf.test.ts guards the styles (no animated shadow or
filter, no backdrop blur or fixed background) and the timers.

## Where things are in the code

| File | What |
|------|------|
| src/webui/app/src/lib/layout.ts | The layouts, the areas and sub-tabs of the workspace, the sections of the dashboard's sidebar |
| src/webui/app/src/lib/commands.svelte.ts | Every command: the pages and the menu items of chains-qt (ids, groups as the old menus, shortcuts, when disabled), the palette's search, `setLayout` |
| src/webui/app/src/lib/detail.svelte.ts | The detail panel: `showDetail({ kind: 'tx' \| 'block' \| 'peer' \| 'sidechain', … })`, `detail.follow` |
| src/webui/app/src/views/Shell.svelte | Picks the layout; the palette and the transaction notices for all |
| src/webui/app/src/views/layouts/Workspace.svelte, Dashboard.svelte | The two frames |
| src/webui/app/src/views/home/ | Home and its cards (the first page of both layouts) |
| src/webui/app/src/views/PageView.svelte | The page shown, whatever the layout |
| src/webui/app/src/components/CommandPalette.svelte, MenuButton.svelte, DetailPanel.svelte, Sidebar.svelte, LayoutPicker.svelte | Shared pieces |
| src/webui/app/src/lib/windowframe.svelte.ts, components/TitleStrip.svelte, WindowControls.svelte, ResizeHandles.svelte | The integrated title bar: its state, the strip, the buttons, the handles at the edges |
| src/webui/app/src/views/home/Home.svelte, Card.svelte, HomeTables.svelte | Home (both layouts): the summary cards, the chart, the tables in tabs |
| src/webui/app/src/lib/quick.svelte.ts, components/QuickPanel.svelte | Send and Receive in the side panel of Home, in both layouts (the confirmation of the Send page) |
| src/webui/app/src/components/TopActions.svelte, NotificationBell.svelte, ThemeToggle.svelte, lib/notices.svelte.ts | The round buttons of the top bars |
| src/webui/app/src/styles/themes.css, styles/base.css, lib/themes.ts, lib/contrast.ts | The looks, Midnight the default; the shared pieces (cards, pills, the big button) |

For the pages: a page with a selectable list publishes its selection with
`showDetail(…)` when `detail.follow` is on (the workspace layout), and keeps its
dialog otherwise — pages/Transactions.svelte does so (a click selects and shows
the transaction in the panel; the context menu's "Show transaction details" keeps
the dialog), and so do the blocks of pages/Explorer.svelte, the peers of the node
window (components/PeersView.svelte) and the sidechains of pages/Sidechains.svelte.
The other way, the panel and the rows of the dashboard open the real pages with
the item chosen in them (`goTo(page, tab, key)`, taken once by the page with
`takeFocus`): a block opens in the explorer, a peer is selected in the peers tab,
**Deposit** opens the Sidechains tab with that sidechain chosen and the deposit
form focused, **Withdrawals** the Withdrawals tab with its first bundle chosen.

The sync overlay (components/SyncOverlay.svelte) covers the page area of each
layout. The Settings page waits for Apply in every tab (doc/webui-audit.md M7):
the look and the layout chosen in Appearance show at once and go back on Cancel;
"Use the title bar of the desktop" is kept by the host at once, as before. The node window's own tabs are hidden in the workspace, whose Node
sub-tabs are those tabs. A new menu action goes in `buildCommands()` only: the
palette, the Menus buttons, the tests and this table follow from it.

## The addresses and the coins (user decision, 2026-10-15)

- **No Addresses page.** The receiving addresses are the **Addresses** tab of the Receive page (the requests stay
  its main tab): label, address, type, used or not, amount received, last used; search, copy, QR code, edit the
  label, export, and **Generate new address** with a label and a type (bech32, bech32m when the wallet has
  taproot, p2sh-segwit, legacy). The old page id `addresses` is `tabOf { page: 'receive', tab: 'addresses' }`
  (as Sidechain nodes is a tab of Sidechains): the menu item "Receiving addresses", the palette and the Wallets
  page open the tab.
- **No sending addresses**: no menu item, no address book button on the recipient field of Send, none in the
  verify form of Sign and verify.
- **Choose coins** on the Send page: a switch in the "Coins to spend" panel, between the recipients and the fee
  (kept as the coin control setting of Settings → Wallet). On, the coins of the wallet show in the page
  (components/CoinPanel.svelte, a virtual list): tick the coins to spend, sort, filter, select all or none, see
  what they hold against the amount to send (a warning when it is not enough), lock and unlock them, and the
  custom change address. Off, the wallet chooses the coins as before.

## Tools and Advanced transactions (user decision, 2026-10-15)

- **Tools** is one page; **Timestamp**, **Proof of funds** and **Crypto tools** are its tabs (pages/Tools.svelte;
  Crypto tools keeps its own tabs: hash calculator, Merkle tree, address decoder). The sidebar's Tools section
  and the workspace's Tools area show it alone.
- **Advanced transactions** is one page, with the wallet; **Sign & verify**, **PSBT** and **Multisig** are its tabs
  (pages/Advanced.svelte). It is the last page of the Wallet section of the sidebar (after Transactions and
  Wallets), and the **Advanced** sub-tab of the workspace's Wallet area. The Chain section still comes before
  Tools.
- The old page ids (`timestamp`, `proof`, `crypto`; `messages`, `psbt`, `multisig`) are `tabOf` the new pages, as
  Sidechain nodes and the receiving addresses: `go(page, tab)` opens the tab, and the page's own tab follows a
  `/` in `nav.tab` (`crypto/merkle`, `psbt/file`, `messages/sign:<address>`; `ownTab()`/`setOwnTab()` in
  lib/nav.svelte.ts). Every menu item (Sign message…, Verify message…, Load PSBT…, Timestamp file, Proof of
  funds, Multisig lounge, Hash calculator, Merkle tree, Address decoder), the palette and the dashboard's quick
  actions open the right tab (`test/layouts.test.ts`).

## Every item of the old menu bar

Every item is also in the **Menus** button of each layout (the workspace's top
bar, the foot of the dashboard's sidebar), under the name of its old menu, and in the
**palette** (Ctrl+K) under the same name. The table says where else it lives.
`test/layouts.test.ts` checks that each item below is a command of its menu.

**Addresses (user decision, 2026-10-15):** "Sending addresses" is gone; "Receiving addresses" opens the
Addresses tab of Receive (see above). `test/layouts.test.ts` checks both.

**Menus (user decision, 2026-10-14):** the menus are regrouped so that what concerns the chain comes
before the tools: *Your wallet*, *Banking*, **Chains** (Sidechains, Sidechain nodes, Solo mine,
Sidechain proposals, Withdrawal votes, Block explorer), **Tools** (Timestamp file, Sign message, Verify
message, Hash calculator, Merkle tree, Address decoder), *Window*, *This node*. The table below keeps
the menus of chains-qt as they were (first column); every item is still in a menu. With
`-disablewallet` the wallet menus, and the items that need a wallet, are not shown (as chains-qt
builds no wallet UI then), nor are the wallet pages. Under the pointer (or the focus) an item shows
the status tip of chains-qt in the status bar, and a page of a sidebar its summary.


| Old menu | Item | Command id | Workspace tabs | Dashboard |
|---|---|---|---|---|
| Your wallet | Create wallet… | `wallet.create` | Wallet ▸ Wallets (Create) | Sidebar Wallets (and its wallet card) |
| Your wallet | Open wallet… | `wallet.open` | Wallet ▸ Wallets (Open) | Sidebar Wallets |
| Your wallet | Close wallet… | `wallet.close` | Wallet ▸ Wallets (Close) | Sidebar Wallets |
| Your wallet | Close all wallets… | `wallet.closeAll` | Wallet ▸ Wallets (Close all) | Sidebar Wallets |
| Your wallet | Migrate wallet… | `wallet.migrate` | Wallet ▸ Wallets (Migrate) | Sidebar Wallets |
| Your wallet | Restore and migrate wallet file… | `wallet.restoreMigrate` | Wallet ▸ Wallets | Sidebar Wallets |
| Your wallet | Sending addresses | — | Gone (user decision, 2026-10-15: no sending addresses) | — |
| Your wallet | Receiving addresses | `wallet.receivingAddresses` | Wallet ▸ Receive (Addresses tab) | Sidebar Receive (Addresses tab); Wallets page |
| Your wallet | Backup wallet… | `wallet.backup` | Wallet ▸ Wallets (Backup) | Sidebar Wallets |
| Your wallet | Restore wallet… | `wallet.restore` | Wallet ▸ Wallets (Restore) | Sidebar Wallets |
| Your wallet | Export watch-only wallet… | `wallet.exportWatchOnly` | Wallet ▸ Wallets | Sidebar Wallets |
| Your wallet | Encrypt wallet… | `wallet.encrypt` | Wallet ▸ Wallets (Encrypt) | Sidebar Wallets |
| Your wallet | Change passphrase… | `wallet.passphrase` | Wallet ▸ Wallets | Sidebar Wallets |
| Your wallet | Unlock wallet for a while… | `wallet.unlock` | Wallet ▸ Wallets | Sidebar Wallets |
| Your wallet | Mask values (Ctrl+Shift+M) | `wallet.mask` | Wallet ▸ Overview button; Settings ▸ Appearance | Overview button; Settings ▸ Appearance |
| Banking | Send (Alt+2) | `banking.send` | Wallet ▸ Send | Sidebar Send; Home: quick action and Balance card (side panel, which links the page) |
| Banking | Receive (Alt+3) | `banking.receive` | Wallet ▸ Receive | Sidebar Receive; Home: quick action and Balance card (side panel, which links the page) |
| Banking | Transactions (Alt+4) | `banking.transactions` | Wallet ▸ History | Sidebar Transactions; Home: Transactions tab "All transactions" |
| Banking | Open URI… | `banking.openUri` | Wallet ▸ Send ("Open URI…") | Send page button |
| Banking | Proof of funds | `banking.proof` | Tools ▸ Tools (Proof of funds tab) | Sidebar Tools (Proof of funds tab) |
| Banking | Multisig lounge | `banking.multisig` | Wallet ▸ Advanced (Multisig tab) | Sidebar Advanced transactions (Multisig tab) |
| Banking | Load PSBT from file… | `banking.psbtFile` | Wallet ▸ Advanced (PSBT tab, From file) | Sidebar Advanced transactions (PSBT tab); Home: "Load PSBT" |
| Banking | Load PSBT from clipboard… | `banking.psbtClipboard` | Wallet ▸ Advanced (PSBT tab, From clipboard) | Sidebar Advanced transactions (PSBT tab); Home: "Load PSBT" |
| Use Chains | Timestamp file | `use.timestamp` | Tools ▸ Tools (Timestamp tab) | Sidebar Tools (Timestamp tab); Mining card "Timestamp" |
| Use Chains | Sign message… | `use.sign` | Wallet ▸ Advanced (Sign & verify tab, Sign) | Sidebar Advanced transactions (Sign & verify tab); Home: "Sign message" |
| Use Chains | Verify message… | `use.verify` | Wallet ▸ Advanced (Sign & verify tab, Verify) | Sidebar Advanced transactions (Sign & verify tab) |
| Use Chains | Sidechains (Alt+5) | `use.sidechains` | Sidechains ▸ Sidechains | Sidebar Sidechains; Sidechains card; Home: "Deposit", Sidechains tab (Deposit, Withdrawals) |
| Chains | Sidechain nodes | `chains.nodes` | Sidechains ▸ Sidechain nodes | Sidebar Sidechain nodes; BMM card |
| Chains | Solo mine | `chains.mine` | Chain ▸ Mine | Sidebar Mine; Mining card; Home: "Mine" |
| Chains | Sidechain proposals | `chains.proposals` | Sidechains ▸ Sidechains (Proposals tab) | Sidechains card "Votes" |
| Chains | Withdrawal votes | `chains.withdrawals` | Sidechains ▸ Sidechains (Withdrawals tab); sidechain panel "Vote on bundles" | Sidechains tab "Withdrawals" of a row |
| Crypto tools | Block explorer | `tools.explorer` | Chain ▸ Explorer | Sidebar Block explorer; Blocks tab "Block explorer"; Home quick action |
| Crypto tools | Hash calculator | `tools.hash` | Tools ▸ Tools (Crypto tools tab, Hash) | Sidebar Tools (Crypto tools tab) |
| Crypto tools | Merkle tree | `tools.merkle` | Tools ▸ Tools (Crypto tools tab, Merkle) | Sidebar Tools (Crypto tools tab) |
| Crypto tools | Address decoder | `tools.address` | Tools ▸ Tools (Crypto tools tab, Address) | Sidebar Tools (Crypto tools tab) |
| Window | Minimize (Ctrl+M) | `window.minimize` | Menus; Ctrl+M | Menus; Ctrl+M |
| Window | Information (Ctrl+I) | `window.info` | Node ▸ Info | Sidebar Node window; Network card arrow |
| Window | Console (Ctrl+T) | `window.console` | Node ▸ Console | Network card "Console"; Node window tab |
| Window | Network traffic (Ctrl+N) | `window.traffic` | Node ▸ Traffic | Node window tab |
| Window | Peers (Ctrl+P) | `window.peers` | Node ▸ Peers; status bar peers menu | Network card "Peers"; Peers tab (a row → panel); status bar |
| This node | Node window (Ctrl+Shift+D) | `node.window` | Node area | Sidebar Node window |
| This node | Options… | `node.options` | Node ▸ Settings | Sidebar Settings |
| This node | Sync status… | `node.sync` | Menus; status bar (out of sync) | Menus; Home "out of sync" |
| This node | Theme ▸ (each look) | `node.theme.*` | Node ▸ Settings ▸ Appearance; Menus; sun/moon of the top bar | Sidebar theme selector; Settings; sun/moon |
| This node | Theme ▸ Custom… | `node.themeCustom` | Node ▸ Settings ▸ Appearance | Settings ▸ Appearance |
| This node | Increase font size (Ctrl++) | `node.fontUp` | Settings ▸ Appearance; Ctrl++ | Sidebar A+; Ctrl++ |
| This node | Decrease font size (Ctrl+−) | `node.fontDown` | Settings ▸ Appearance; Ctrl+− | Sidebar A−; Ctrl+− |
| This node | Command-line options | `node.help` | Menus | Sidebar Menus |
| This node | About Chains | `node.about` | Menus | Sidebar Menus |
| This node | Interface kit (tests, -webuidevtools) | `node.kit` | Node ▸ Interface kit | Sidebar Interface kit |
| This node | Exit (Ctrl+Q) | `node.exit` | Menus; Ctrl+Q | Sidebar Menus; Ctrl+Q |


The status bar of chains-qt (sync, peers with "Show peers" / "Enable/disable
network activity", the unit, the wallet's lock) is the same in every layout; its
actions are in the palette too ("Show peers", "Disable network activity", "Show
amounts in …").

## Tests and pictures

- `npm test` (src/webui/app): test/layouts.test.ts — every old menu item is a
  command of its menu, every page is in each layout, the search, the layout switch
  and its preference (a saved `sidebar` read as the dashboard), Ctrl+K, and each layout
  rendered: workspace areas (Home first, its cards and side panel), sub-tabs, Menus, a
  transaction in the detail panel; dashboard cards opening pages and the panel; the
  palette running a command. test/startup.test.ts: each layout starts on Home.
- `src/webui/test/e2e/run_layouts.py --app build/bin/chains-app --tmp <dir> --shots <dir>`:
  a wallet with a history and two sidechains on regtest, then each layout in the
  light and dark themes — home, send, the history with the detail panel (or its
  dialog), the sidechains, the settings, the palette — plus the Menus and the
  dashboard in the other looks.
- `npm test`: test/contrast.test.ts — every look has its colours, Midnight is the default, and each
  text colour passes WCAG AA on its backgrounds. test/layouts.test.ts also: the home's cards, tabs
  and row actions, Send and Receive in the side panel, the bell and the sun/moon in each layout,
  the sidebar's wallet card with Close wallet and Quit.
- `run_layouts.py … --script refresh.js`: the pictures of the refresh — each layout in Midnight
  (Home, overview, Send, transactions, sidechains, settings; the dashboard's Receive, tables,
  sidechain panel and notifications), the dashboard in every other look.
- `npm test`: test/titlebar.test.ts — the buttons call the bridge, the maximize button follows the
  state (and its event), a press on the bar moves the window and a double click maximizes it (not
  from its buttons), the handles, the bar in each layout and none with the title bar of the system.
  `test_chains-app`: windowframe_tests.cpp — the names of the edges, the setting, `EdgeAt`.
- `run_layouts.py … --script titlebar.js`: the title bar of each layout in the light and dark
  themes (and the other looks), the red Close, then the title bar of the system chosen in the
  settings and back (pictures `titlebar-*.png`).
- `run_layouts.py … --script uishots.js`: the pages of the user decisions of 2026-10-15 in Midnight and Light —
  Receive → Addresses, Send with "Choose coins" (coins not enough, then enough), each tab of Tools and of
  Advanced transactions; and Home in the workspace and the dashboard.
