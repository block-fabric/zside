// The state of the app as the page knows it: what the host says in its events,
// kept in Svelte state that the components read.
import { on } from './bridge';
import { t } from './i18n.svelte';
import { addSample } from './traffic.svelte';
import { addProgressSample, type ProgressSample } from './syncoverlay';
import { host, type AppInfo, type Dialog, type NodeSnapshot, type Phase, type WalletSummary } from './rpc';
import { isUnit, type Unit } from './units';
import { clampFontAdjust, DEFAULT_THEME } from './themes';
import { checkStoredThemes, isThemeChoice, type CustomTheme, type ThemeChoice } from './customthemes';
import { DEFAULT_LAYOUT, isLayout, type LayoutId } from './layout';

export interface Tip {
  height: number;
  time: number;
  hash: string;
  progress: number;
  state: string;
  ibd: boolean;
  loadingBlocks: boolean;
}

export interface WalletBalances {
  trusted: bigint;
  untrustedPending: bigint;
  immature: bigint;
}

export const app = $state({
  phase: 'loading' as Phase,
  phaseData: {} as Record<string, unknown>,
  info: null as AppInfo | null,
  /** Dialogs of the node that wait for the user, oldest first. */
  dialogs: [] as Dialog[],
  /** Init messages, latest last. */
  initMessage: '',
  /** A long operation of the node (rescan, verification): title and percent; null when none. */
  progress: null as { title: string; progress: number } | null,
});

export const node = $state({
  connections: 0,
  networkActive: true,
  warnings: [] as string[],
  tip: null as Tip | null,
  header: null as { height: number; time: number } | null,
  presync: null as { height: number; time: number } | null,
  /** The latest blocks seen, newest last (the chain in the status bar). */
  recent: [] as { height: number; hash: string; time: number }[],
  bansChanged: 0,
  /** The mempool: transactions, memory used, and the most it may use (ClientModel::mempoolSizeChanged). */
  mempool: { size: 0, usage: 0, max: 0 },
  /** Samples of the verification progress, the newest first (for the sync overlay). */
  progressSamples: [] as ProgressSample[],
});

export const wallets = $state({
  list: [] as WalletSummary[],
  current: null as string | null,
  balances: {} as Record<string, WalletBalances>,
  /** Progress of a wallet operation (rescan), by wallet. */
  progress: {} as Record<string, { title: string; progress: number }>,
  /** Bumped on every transaction event of the current wallet, for pages to refresh. */
  txVersion: 0,
  /** Bumped on every change of the address book of the current wallet. */
  addressVersion: 0,
  /** The latest transactions new in a wallet, oldest first (for the notices): wallet and txid; at most 100. */
  newTxs: [] as { wallet: string; txid: string }[],
  /** How many new transactions there were since the page loaded (newTxs keeps the latest 100 of them). */
  newTxTotal: 0,
});

export const prefs = $state({
  theme: DEFAULT_THEME as ThemeChoice,
  /** The themes of the user (Settings → Advanced theme), checked as strictly as a theme file. */
  customThemes: [] as CustomTheme[],
  fontAdjust: 0,
  unit: 'CHN' as Unit,
  mask: false,
  /** How the window is laid out: workspace tabs or dashboard (lib/layout.ts). */
  layout: DEFAULT_LAYOUT as LayoutId,
  lastWallet: null as string | null,
  /** Wallet options (OptionsModel): coin control features, PSBT controls, subtract the fee by default. */
  coinControl: false,
  psbtControls: false,
  subtractFeeDefault: false,
  /** The send page: the fee section folded, smart or custom fee, the target, the custom rate (sat/kvB). */
  feeMinimized: true,
  feeMode: 'smart' as 'smart' | 'custom',
  confTarget: 0,
  customFee: 0,
  /** Coin control shows coins by address (tree) or in a list. */
  coinTree: true,
  /** The size of the text of the console, in pixels (consoleFontSize). */
  consoleFont: 13,
  /** The window: an icon in the tray, minimized to it, minimized when closed (OptionsModel). */
  showTray: true,
  minimizeToTray: false,
  minimizeOnClose: false,
  /** Web pages of third-party explorers for a transaction, %s for its id, separated by | (strThirdPartyTxUrls). */
  thirdPartyTxUrls: '',
  /** The font of amounts: the embedded monospaced one, or the one of the system (FontForMoney). */
  moneyFont: 'embedded' as 'embedded' | 'system',
  /** A font chosen apart from the look ('' for the font of the look). */
  fontFamily: '' as '' | 'inter' | 'nunito' | 'mono' | 'system',
  /** The range of the traffic graph, in minutes. */
  trafficRange: 30,
  /** Solo mining: the address of the rewards and the threads, as last started (MiningAddress, MiningThreads). */
  miningAddress: '',
  miningThreads: 1,
  /** Block explorer: how many of the latest blocks are listed (0: all). */
  explorerCount: 50,
  /** Block explorer: the blocks shown a page (10, 25, 50 or 100). */
  explorerPageSize: 25,
  /** Multisig lounge: keys and shared addresses (MultisigKeys, MultisigAddresses), name and value. */
  multisigKeys: [] as { name: string; value: string }[],
  multisigAddresses: [] as { name: string; value: string }[],
  /** Simple (the pages a beginner needs, in plain words) or Advanced (every section of every page); lib/uimode.svelte.ts. */
  uiMode: 'simple' as 'simple' | 'advanced',
  /** The first-run tips of Home were dismissed (components/FirstRunTips.svelte). */
  tipsDone: false,
  loaded: false,
});

export interface Toast {
  id: number;
  kind: 'info' | 'success' | 'warning' | 'error';
  title: string;
  message?: string;
  /** Buttons of the notice; each dismisses it. */
  actions?: { label: string; run: () => void }[];
}

export const toasts = $state({ list: [] as Toast[] });

/** The status tip of the menu item or the entry under the pointer (QAction::statusTip), shown in the status bar. */
export const statusTip = $state({ text: '' });
let nextToast = 1;

export function toast(kind: Toast['kind'], title: string, message?: string, timeoutMs = kind === 'error' ? 9000 : 4500, actions?: Toast['actions']): number {
  const id = nextToast++;
  toasts.list.push({ id, kind, title, message, actions });
  if (toasts.list.length > 5) toasts.list.splice(0, toasts.list.length - 5);
  if (timeoutMs > 0) setTimeout(() => dismissToast(id), timeoutMs);
  return id;
}

export function dismissToast(id: number): void {
  const i = toasts.list.findIndex((t) => t.id === id);
  if (i >= 0) toasts.list.splice(i, 1);
}

const RECENT_BLOCKS = 7;

function setTip(tip: Tip): void {
  node.tip = tip;
  addProgressSample(node.progressSamples, { t: Date.now(), progress: tip.progress });
  const last = node.recent[node.recent.length - 1];
  if (!last || last.hash !== tip.hash) {
    // A reorganisation or a jump: the chain shown starts again from the new tip.
    if (last && tip.height !== last.height + 1) node.recent = [];
    node.recent.push({ height: tip.height, hash: tip.hash, time: tip.time });
    if (node.recent.length > RECENT_BLOCKS) node.recent.splice(0, node.recent.length - RECENT_BLOCKS);
  }
}

function sats(n: unknown): bigint {
  return typeof n === 'number' ? BigInt(Math.round(n)) : 0n;
}

/** Whether the node has wallets at all (not with -disablewallet). */
export function walletsEnabled(): boolean {
  return app.info?.walletEnabled !== false;
}

/** What to tell the user who needs a wallet and has none open. */
export function noWalletText(why = ''): string {
  if (!walletsEnabled()) return t('This node runs without wallets (-disablewallet).');
  return why ? t('Open or create a wallet first: %1', why) : t('Open or create a wallet first.');
}

export function selectWallet(name: string | null): void {
  wallets.current = name;
  host.setWalletTitle(wallets.list.length > 1 ? name : null).catch(() => undefined);
  if (name) savePrefs({ lastWallet: name });
}

function addWallet(w: WalletSummary): void {
  const i = wallets.list.findIndex((x) => x.name === w.name);
  if (i >= 0) wallets.list[i] = w;
  else wallets.list.push(w);
  wallets.list.sort((a, b) => a.name.localeCompare(b.name));
  if (wallets.current === null || (prefs.lastWallet === w.name && !wallets.list.some((x) => x.name === wallets.current))) {
    selectWallet(w.name);
  } else if (wallets.list.length > 1) {
    host.setWalletTitle(wallets.current).catch(() => undefined);
  }
}

function removeWallet(name: string): void {
  wallets.list = wallets.list.filter((w) => w.name !== name);
  delete wallets.balances[name];
  if (wallets.current === name) selectWallet(wallets.list[0]?.name ?? null);
}

/** Take the state of a node snapshot (after the page loads, or a reload). */
export function applySnapshot(s: NodeSnapshot): void {
  node.connections = s.connections;
  node.networkActive = s.networkActive;
  node.warnings = s.warnings;
  setTip(s.tip);
  if (s.headerTip.height !== undefined) node.header = { height: s.headerTip.height, time: s.headerTip.time ?? 0 };
  for (const w of s.wallets) addWallet(w);
}

let subscribed = false;

/** How many phase events the page took: the answer of app.hello is older than any of them (App.svelte). */
export const phaseEvents = { count: 0 };

/** Follow the events of the host. */
export function subscribe(): void {
  if (subscribed) return;
  subscribed = true;
  on<{ phase: Phase; data: Record<string, unknown> }>('phase', ({ phase, data }) => {
    phaseEvents.count++;
    app.phase = phase;
    app.phaseData = data ?? {};
  });
  on<Dialog>('dialog', (d) => {
    app.dialogs.push(d);
  });
  on<Dialog>('notice', (d) => {
    toast(d.level === 'error' ? 'error' : d.level === 'warning' ? 'warning' : 'info', d.title, d.message);
  });
  on<{ message: string }>('init.message', ({ message }) => {
    app.initMessage = message;
  });
  on<{ title: string; progress: number }>('progress', ({ title, progress }) => {
    app.progress = progress >= 100 || progress < 0 ? null : { title, progress };
  });
  on<{ count: number }>('connections', ({ count }) => {
    node.connections = count;
  });
  on<{ active: boolean }>('networkActive', ({ active }) => {
    node.networkActive = active;
  });
  on<{ warnings: string[] }>('alert', ({ warnings }) => {
    node.warnings = warnings;
  });
  on('bans', () => {
    node.bansChanged++;
  });
  on<Tip>('tip', (tip) => setTip(tip));
  on<{ recv: number; sent: number; time: number }>('traffic', ({ recv, sent, time }) => addSample({ t: time, recv, sent }));
  on<{ size: number; usage: number; max: number }>('mempool', (m) => {
    node.mempool = m;
  });
  on<{ height: number; time: number; presync: boolean }>('headerTip', (h) => {
    if (h.presync) node.presync = { height: h.height, time: h.time };
    else {
      node.header = { height: h.height, time: h.time };
      node.presync = null;
    }
  });
  on<WalletSummary>('wallet.loaded', (w) => addWallet(w));
  on<{ name: string }>('wallet.unloaded', ({ name }) => removeWallet(name));
  on<{ name: string; encrypted: boolean; locked: boolean }>('wallet.status', (s) => {
    const w = wallets.list.find((x) => x.name === s.name);
    if (w) {
      w.encrypted = s.encrypted;
      w.locked = s.locked;
    }
  });
  on<{ name: string; trusted: number; untrusted_pending: number; immature: number }>('wallet.balance', (b) => {
    wallets.balances[b.name] = { trusted: sats(b.trusted), untrustedPending: sats(b.untrusted_pending), immature: sats(b.immature) };
  });
  on<{ name: string; title: string; progress: number }>('wallet.progress', (p) => {
    if (p.progress >= 100 || p.progress < 0) delete wallets.progress[p.name];
    else wallets.progress[p.name] = { title: p.title, progress: p.progress };
  });
  on<{ name: string; txid: string; change: string }>('wallet.tx', ({ name, txid, change }) => {
    if (name === wallets.current) wallets.txVersion++;
    if (change === 'new') {
      wallets.newTxTotal++;
      wallets.newTxs.push({ wallet: name, txid });
      if (wallets.newTxs.length > 100) wallets.newTxs.splice(0, wallets.newTxs.length - 100);
    }
  });
  on<{ name: string }>('wallet.addressbook', ({ name }) => {
    if (name === wallets.current) wallets.addressVersion++;
  });
}

// --- Preferences (kept by the app, per network) ---

export function applyPrefs(raw: Record<string, unknown>): void {
  if (raw.customThemes !== undefined) prefs.customThemes = checkStoredThemes(raw.customThemes);
  if (isThemeChoice(raw.theme, prefs.customThemes)) prefs.theme = raw.theme;
  if (typeof raw.fontAdjust === 'number') prefs.fontAdjust = clampFontAdjust(raw.fontAdjust);
  if (isUnit(raw.unit)) prefs.unit = raw.unit;
  if (typeof raw.mask === 'boolean') prefs.mask = raw.mask;
  // A layout no longer known (the compact sidebar 'sidebar', gone) becomes the default, the dashboard.
  if (raw.layout !== undefined) prefs.layout = isLayout(raw.layout) ? raw.layout : DEFAULT_LAYOUT;
  if (typeof raw.lastWallet === 'string') prefs.lastWallet = raw.lastWallet;
  if (raw.uiMode === 'simple' || raw.uiMode === 'advanced') prefs.uiMode = raw.uiMode;
  for (const key of ['coinControl', 'psbtControls', 'subtractFeeDefault', 'feeMinimized', 'coinTree', 'showTray', 'minimizeToTray', 'minimizeOnClose', 'tipsDone'] as const) {
    if (typeof raw[key] === 'boolean') prefs[key] = raw[key];
  }
  if (raw.feeMode === 'smart' || raw.feeMode === 'custom') prefs.feeMode = raw.feeMode;
  if (typeof raw.confTarget === 'number' && raw.confTarget >= 0) prefs.confTarget = Math.floor(raw.confTarget);
  if (typeof raw.customFee === 'number' && raw.customFee >= 0) prefs.customFee = Math.floor(raw.customFee);
  if (typeof raw.consoleFont === 'number' && raw.consoleFont >= 8 && raw.consoleFont <= 28) prefs.consoleFont = Math.floor(raw.consoleFont);
  if (typeof raw.thirdPartyTxUrls === 'string') prefs.thirdPartyTxUrls = raw.thirdPartyTxUrls;
  if (typeof raw.miningAddress === 'string') prefs.miningAddress = raw.miningAddress;
  if (typeof raw.miningThreads === 'number' && raw.miningThreads >= 1) prefs.miningThreads = Math.floor(raw.miningThreads);
  // 10 to 50 000 of the latest blocks, or 0 for all of them.
  if (typeof raw.explorerCount === 'number' && (raw.explorerCount === 0 || (raw.explorerCount >= 10 && raw.explorerCount <= 50000))) prefs.explorerCount = Math.floor(raw.explorerCount);
  if (raw.explorerPageSize === 10 || raw.explorerPageSize === 25 || raw.explorerPageSize === 50 || raw.explorerPageSize === 100) prefs.explorerPageSize = raw.explorerPageSize;
  const entries = (v: unknown) => (Array.isArray(v) ? v.filter((e): e is { name: string; value: string } => !!e && typeof e.name === 'string' && typeof e.value === 'string') : []);
  if (Array.isArray(raw.multisigKeys)) prefs.multisigKeys = entries(raw.multisigKeys);
  if (Array.isArray(raw.multisigAddresses)) prefs.multisigAddresses = entries(raw.multisigAddresses);
  if (raw.moneyFont === 'embedded' || raw.moneyFont === 'system') prefs.moneyFont = raw.moneyFont;
  if (raw.fontFamily === '' || raw.fontFamily === 'inter' || raw.fontFamily === 'nunito' || raw.fontFamily === 'mono' || raw.fontFamily === 'system') prefs.fontFamily = raw.fontFamily;
  if (typeof raw.trafficRange === 'number' && raw.trafficRange >= 5 && raw.trafficRange <= 1440) prefs.trafficRange = Math.round(raw.trafficRange / 5) * 5;
  prefs.loaded = true;
}

export async function loadPrefs(): Promise<void> {
  try {
    applyPrefs(await host.prefs());
  } catch {
    prefs.loaded = true;
  }
}

export function savePrefs(changes: Partial<Omit<typeof prefs, 'loaded'>>): void {
  Object.assign(prefs, changes);
  host.setPrefs(changes as Record<string, unknown>).catch(() => undefined);
}
