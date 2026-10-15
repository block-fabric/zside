// A typed client of the RPC methods of the node, and of the host methods of the app.
import { request, type CallOptions } from './bridge';

export interface BlockchainInfo {
  chain: string;
  blocks: number;
  headers: number;
  bestblockhash: string;
  difficulty: number;
  time: number;
  mediantime: number;
  verificationprogress: number;
  initialblockdownload: boolean;
  size_on_disk: number;
  pruned: boolean;
  warnings: string[] | string;
}

export interface NetworkInfo {
  version: number;
  subversion: string;
  protocolversion: number;
  connections: number;
  connections_in: number;
  connections_out: number;
  networkactive: boolean;
  localaddresses: { address: string; port: number; score: number }[];
  warnings: string[] | string;
}

export interface Balances {
  mine: { trusted: number; untrusted_pending: number; immature: number; used?: number };
}

export interface WalletInfo {
  walletname: string;
  txcount: number;
  keypoolsize: number;
  unlocked_until?: number;
  private_keys_enabled: boolean;
  descriptors: boolean;
  scanning: false | { duration: number; progress: number };
}

export interface ValidateAddress {
  isvalid: boolean;
  address?: string;
  scriptPubKey?: string;
  isscript?: boolean;
  iswitness?: boolean;
  witness_version?: number;
  error?: string;
}

export interface MempoolInfo {
  loaded: boolean;
  size: number;
  bytes: number;
  usage: number;
  total_fee: number;
}

/** The RPC methods the interface uses, with their params and results. Any other goes through rpc.raw. */
export interface RpcMethods {
  getblockchaininfo: { params: []; result: BlockchainInfo };
  getnetworkinfo: { params: []; result: NetworkInfo };
  getblockcount: { params: []; result: number };
  getblockhash: { params: [height: number]; result: string };
  getbestblockhash: { params: []; result: string };
  getmempoolinfo: { params: []; result: MempoolInfo };
  uptime: { params: []; result: number };
  listwallets: { params: []; result: string[] };
  listwalletdir: { params: []; result: { wallets: { name: string }[] } };
  getbalances: { params: []; result: Balances };
  getwalletinfo: { params: []; result: WalletInfo };
  getnewaddress: { params: [label?: string, address_type?: string]; result: string };
  validateaddress: { params: [address: string]; result: ValidateAddress };
  generatetoaddress: { params: [nblocks: number, address: string]; result: string[] };
  createwallet: { params: [name: string, disable_private_keys?: boolean, blank?: boolean, passphrase?: string]; result: { name: string; warnings?: string[] } };
  loadwallet: { params: [name: string]; result: { name: string } };
  unloadwallet: { params: [name: string]; result: { warnings?: string[] } };
  help: { params: [command?: string]; result: string };
}

export type Method = keyof RpcMethods;

export const rpc = {
  /** Call a method the client knows the types of. */
  call<M extends Method>(method: M, params: RpcMethods[M]['params'], options?: CallOptions): Promise<RpcMethods[M]['result']> {
    return request<RpcMethods[M]['result']>('rpc', method, params as unknown[], options);
  },
  /** Call any method (the console, methods of the chain the client has no types for). */
  raw<T = unknown>(method: string, params: unknown[] | Record<string, unknown> = [], options?: CallOptions): Promise<T> {
    return request<T>('rpc', method, params, options);
  },
};

// --- Host methods (src/webui/app.cpp RegisterHostMethods) ---

export type Phase = 'loading' | 'intro' | 'init' | 'ready' | 'shutdown' | 'fatal';

export interface AppInfo {
  name: string;
  version: string;
  bugreport: string;
  network: string;
  networkLabel: string;
  datadir: string;
  walletEnabled: boolean;
  devtools: boolean;
  /** The language of the interface ('' for English). */
  language?: string;
  test: boolean;
  platform: string;
  blockSpacing?: number;
  /** The weight a block of this chain and network may have (consensus.max_block_weight). */
  blockWeight?: number;
}

export interface Dialog {
  id?: number;
  kind: 'message' | 'question' | 'settingsError';
  level?: 'error' | 'warning' | 'info';
  title: string;
  message: string;
  question?: string;
  details?: string[];
  style?: number;
}

export interface Hello {
  phase: Phase;
  phaseData: Record<string, unknown>;
  info: AppInfo;
  dialogs: Dialog[];
}

export interface IntroData {
  defaultDir: string;
  dir: string;
  blockchainGB: number;
  chainstateGB: number;
  pruneGB: number;
  minPruneGB: number;
  pruneForced?: boolean;
}

export interface Space {
  exists?: boolean;
  isDirectory?: boolean;
  free?: number;
  error?: string;
}

export interface WalletSummary {
  name: string;
  encrypted: boolean;
  locked: boolean;
  privateKeysDisabled: boolean;
  hd: boolean;
}

export interface NodeSnapshot {
  connections: number;
  networkActive: boolean;
  warnings: string[];
  tip: { height: number; time: number; hash: string; progress: number; state: string; ibd: boolean; loadingBlocks: boolean };
  headerTip: { height?: number; time?: number; presync?: boolean };
  wallets: WalletSummary[];
}

/** The window and its title bar (window.state; doc/webui-layouts.md). */
export interface WindowState {
  titleBar: 'integrated' | 'system';
  supported: boolean;
  frameless: boolean;
  restartNeeded: boolean;
  maximized: boolean;
}

export const host = {
  hello: () => request<Hello>('host', 'app.hello', {}),
  info: () => request<AppInfo>('host', 'app.info', {}),
  quit: () => request<boolean>('host', 'app.quit', {}),
  restart: () => request<boolean>('host', 'app.restart', {}),
  help: () => request<{ usage: string; options: string }>('host', 'app.help', {}),
  about: () => request<{ name: string; version: string; license: string }>('host', 'app.about', {}),
  languages: () => request<{ code: string; name: string; rtl: boolean; messages: number }[]>('host', 'app.languages', {}),
  updateLanguage: () => request<string>('host', 'app.updateLanguage', {}),
  introSpace: (dir: string) => request<Space>('host', 'intro.space', { dir }),
  introChoose: (dir: string, pruneGB: number) => request<boolean>('host', 'intro.choose', { dir, pruneGB }),
  introCancel: () => request<boolean>('host', 'intro.cancel', {}),
  answer: (id: number, answer: unknown) => request<boolean>('host', 'dialog.answer', { id, answer }),
  prefs: () => request<Record<string, unknown>>('host', 'prefs.get', {}),
  setPrefs: (changes: Record<string, unknown>) => request<Record<string, unknown>>('host', 'prefs.set', changes),
  nodeState: () => request<NodeSnapshot>('host', 'node.state', {}),
  setNetworkActive: (active: boolean) => request<boolean>('host', 'node.setNetworkActive', { active }),
  getSetting: (name: string) => request<{ value: unknown; ignored: boolean }>('host', 'node.getSetting', { name }),
  setWalletTitle: (name: string | null) => request<boolean>('host', 'window.setWallet', { name }),
  minimize: () => request<boolean>('host', 'window.minimize', {}),
  windowState: () => request<WindowState>('host', 'window.state', {}),
  toggleMaximize: () => request<boolean>('host', 'window.toggleMaximize', {}),
  closeWindow: () => request<boolean>('host', 'window.close', {}),
  startMove: () => request<boolean>('host', 'window.startMove', {}),
  startResize: (edge: string) => request<boolean>('host', 'window.startResize', { edge }),
  setTitleBar: (mode: 'integrated' | 'system') => request<WindowState>('host', 'window.setTitleBar', { mode }),
  copy: (text: string) => request<boolean>('host', 'clipboard.write', { text }),
  paste: () => request<string | null>('host', 'clipboard.read', {}),
  // The node window (src/webui/nodeapi.cpp).
  consoleCheck: (line: string) => request<{ ok: boolean; filtered?: string; error?: string }>('host', 'console.check', { line }),
  consoleRun: (line: string, wallet: string | null, options?: CallOptions) =>
    request<{ ok: boolean; result?: string; error?: string }>('host', 'console.run', wallet === null ? { line } : { line, wallet }, options),
  disconnectPeer: (id: number) => request<boolean>('host', 'peers.disconnect', { id }),
  banPeer: (id: number, seconds: number) => request<boolean>('host', 'peers.ban', { id, seconds }),
  unban: (address: string) => request<boolean>('host', 'bans.unban', { address }),
  paths: () => request<{ datadir: string; blocksdir: string; debuglog: string; config: string }>('host', 'node.paths', {}),
  open: (what: 'debuglog' | 'config' | 'url', url?: string) => request<boolean>('host', 'app.open', url === undefined ? { what } : { what, url }),
  notify: (title: string, body: string, level: 'info' | 'warning' | 'critical' = 'info') => request<boolean>('host', 'app.notify', { title, body, level }),
  trayState: () => request<{ available: boolean; visible: boolean; windowVisible: boolean }>('host', 'tray.state', {}),
  // Options of the node (src/webui/options.cpp).
  options: () => request<NodeOptions>('host', 'options.get', {}),
  // A proxy, the Tor proxy or the RPC server turned on waits for a confirmation of the desktop (declined: nothing changed).
  setOption: (name: keyof NodeOptionValues | 'proxyAddress' | 'torAddress', value: unknown) => request<NodeOptions>('host', 'options.set', { name, value }),
  // A setting that names a program (the signer): the file chooser and a confirmation of the desktop; null when cancelled.
  chooseProgramOption: (name: 'signer') => request<NodeOptions | null>('host', 'options.chooseProgram', { name }),
  // A setting that names a file the node reads (a sidechain's mainchain cookie or password file): the file chooser of the desktop; null when cancelled.
  chooseFileOption: (name: 'mainchainrpccookiefile' | 'mainchainrpcpasswordfile') => request<NodeOptions | null>('host', 'options.chooseFile', { name }),
  validProxy: (address: string) => request<boolean>('host', 'options.validProxy', { address }),
  resetOptions: () => request<boolean>('host', 'options.reset', {}),
  applyOptions: () => request<NodeOptions>('host', 'options.apply', {}),
  cancelOptions: () => request<NodeOptions>('host', 'options.cancel', {}),
  // Tools.
  depositAddress: (text: string) => request<{ kind: 'plain' | 'valid' | 'invalid'; slot?: number; address?: string }>('host', 'drivechain.depositAddress', { text }),
  hashes: (text: string, hex: boolean) => request<Hashes>('host', 'tools.hashes', { text, hex }),
  merkle: (txids: string[]) => request<{ levels?: string[][]; error?: string }>('host', 'tools.merkle', { txids }),
  decode: (text: string) => request<Decoded>('host', 'tools.decode', { text }),
  /** The latest blocks from the block index, newest first (0: all): [height, hash, time, transactions, difficulty]. */
  explorerBlocks: (count: number) => request<[number, string, number, number, number][]>('host', 'explorer.blocks', { count }),
  hashFile: (path: string) => request<{ sha256: string; size: number }>('host', 'timestamp.hashFile', { path }),
  searchStamp: (payload: string, from: string, count: number, options?: CallOptions) =>
    request<{ searched: number; next: string | null; found: Stamp[] }>('host', 'timestamp.search', { payload, from, count }, options),
  // Sidechain nodes (src/webui/sidechainnodes.cpp).
  sidechainNodes: () => request<SidechainNodeList>('host', 'sidechainNodes.list', {}),
  sidechainNode: (method: SidechainNodeMethod, params: Record<string, unknown>) => request<{ log?: string[]; error?: string } | null>('host', `sidechainNodes.${method}`, params),
  sidechainStatus: (stem: string) => request<SidechainNodeStatus>('host', 'sidechainNodes.status', { stem }),
};

export interface NodeOptionValues {
  dbcache: number;
  par: number;
  prune: boolean;
  pruneSize: number;
  spendZeroConf: boolean;
  signer: string;
  natpmp: boolean;
  listen: boolean;
  server: boolean;
  proxyUse: boolean;
  proxyIp: string;
  proxyPort: string;
  torUse: boolean;
  torIp: string;
  torPort: string;
  lang: string;
  startAtLogin: boolean;
  // A sidechain: the connection to its mainchain node (-mainchainrpc*; empty for the default).
  mainchainConnect?: string;
  mainchainPort?: string;
  mainchainWallet?: string;
  mainchainUser?: string;
  mainchainCookieFile?: string;
  mainchainPasswordFile?: string;
}

export interface NodeOptions extends NodeOptionValues {
  /** Settings set on the command line or in chains.conf, by their name in settings.json. */
  ignored: Record<string, boolean>;
  overridden: string;
  limits: { dbcacheMin: number; parMin: number; parMax: number; pruneMinGB: number };
  proxyReach: { ipv4: boolean; ipv6: boolean; tor: boolean };
  externalSigner: boolean;
  restartRequired: boolean;
  /** Changes written since the last Apply, that Cancel takes back. */
  pending?: number;
  /** A sidechain: the defaults of the connection to the mainchain node, and what the command line or the configuration file sets of it. */
  mainchainDefaults?: { port: number; connect: string };
  mainchainIgnored?: Record<string, string>;
  /** The user said no to the confirmation of the desktop: nothing changed. */
  declined?: boolean;
}

export interface Hashes {
  sha256: string;
  sha256d: string;
  sha256dId: string;
  ripemd160: string;
  hash160: string;
  sha512: string;
  sha1: string;
  bytes: number;
  error?: string;
}

export interface Decoded {
  base58?: { version: number; payload: string };
  bech32?: { encoding: 'bech32' | 'bech32m'; prefix: string; witnessVersion?: number; witnessProgram?: string };
}

export interface Stamp {
  height: number;
  time: number;
  txid: string;
  block: string;
}

export interface SidechainNodeEntry {
  name: string;
  stem: string;
  slot: number;
  description: string;
  builtin: boolean;
  dir: string;
  installed: boolean;
  programs: string;
  dataDir: string;
  /** The release of this sidechain built into the app for this computer, if there is one. */
  release?: { version: string; url: string; sha256: string };
}

export interface SidechainNodeList {
  server: boolean;
  network: string;
  sidechains: SidechainNodeEntry[];
}

export interface SidechainNodeStatus {
  stem: string;
  installed: boolean;
  running: boolean;
  blocks?: number;
  mainchain?: string;
  mining?: boolean;
  miningText?: string;
}

export type SidechainNodeMethod = 'locate' | 'download' | 'add' | 'remove' | 'start' | 'startWallet' | 'stop' | 'mineOnce' | 'mining' | 'log';
