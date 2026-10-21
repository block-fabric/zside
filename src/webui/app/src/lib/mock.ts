// A pretend host, for working on the interface in a browser (npm run dev) and for the tests.
// Never part of the build of chains-app (only loaded when import.meta.env.DEV is set).
import { dispatch, setTransport, type BridgeRequest } from './bridge';

export interface MockOptions {
  phase?: string;
  wallets?: string[];
}

export function installMock(options: MockOptions = {}): void {
  let height = 1200;
  const start = Math.floor(Date.now() / 1000);
  const prefs: Record<string, unknown> = {};
  const wallets = options.wallets ?? ['alice'];
  // The browser preview shows the integrated title bar, as chains-app on Linux.
  const frameState = { titleBar: 'integrated', supported: true, frameless: true, restartNeeded: false, maximized: false };
  const hash = (h: number) => h.toString(16).padStart(64, '0');

  const host: Record<string, (params: Record<string, unknown>) => unknown> = {
    'app.hello': () => ({
      phase: options.phase ?? 'ready',
      phaseData: {},
      dialogs: [],
      info: { name: 'Chains', version: 'v32.0.0-dev (preview)', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp/preview', walletEnabled: true, devtools: true, test: false, platform: 'browser', blockSpacing: 600 },
    }),
    'app.help': () => ({ usage: 'Usage: chains-app [options] [URI]', options: 'Options:\n\n  -?\n       Print this help message and exit' }),
    'app.about': () => ({ name: 'Chains', version: 'preview', license: 'Distributed under the MIT software license.' }),
    'app.quitGuard': () => true,
    'prefs.get': () => prefs,
    'prefs.set': (p) => Object.assign(prefs, p),
    'node.state': () => ({
      connections: 8,
      networkActive: true,
      warnings: [],
      tip: { height, time: start - 30, hash: hash(height), progress: 1, state: 'synced', ibd: false, loadingBlocks: false },
      headerTip: { height, time: start - 30 },
      wallets: wallets.map((name) => ({ name, encrypted: true, locked: true, privateKeysDisabled: false, hd: true })),
    }),
    'window.setWallet': () => true,
    'window.state': () => ({ ...frameState }),
    'window.minimize': () => true,
    'window.toggleMaximize': () => {
      frameState.maximized = !frameState.maximized;
      queueMicrotask(() => dispatch([{ type: 'window', data: { maximized: frameState.maximized } }]));
      return true;
    },
    'window.close': () => true,
    'window.startMove': () => true,
    'window.startResize': () => true,
    'window.setTitleBar': (p) => {
      frameState.titleBar = p.mode === 'system' ? 'system' : 'integrated';
      frameState.frameless = frameState.titleBar === 'integrated';
      return { ...frameState };
    },
    'clipboard.write': () => true,
    'clipboard.read': () => '',
    'intro.space': () => ({ exists: true, isDirectory: true, free: 512e9 }),
    // A wallet with a history, for the pages and the dashboard of the preview.
    'wallet.info': (p) => ({ name: p.wallet ?? '', encrypted: true, locked: true, privateKeysDisabled: false, externalSigner: false, hd: true, canGetAddresses: true, taproot: true, defaultAddressType: 'bech32', confirmTarget: 6, requiredFeePerK: 1000, maxTxFee: 10_000_000, multiwallet: wallets.length > 1, balance: 12.5, unconfirmed: 0.25, immature: 50 }),
    'wallet.transactions': () => ({ height, hash: hash(height), records: history() }),
    'wallet.transaction': (p) => ({ txid: p.txid, coinbase: false, rows: [{ k: 'Status', text: '6 confirmations' }], notes: [], ids: [{ k: 'Transaction ID', text: String(p.txid) }], messages: [] }),
    'wallet.txActions': () => ({ canAbandon: false, canBump: true, hex: '' }),
    'wallet.addresses': () => [],
    'wallet.requests': () => [],
    'wallet.fee': () => ({ feePerK: 1000, fallback: false, target: 6, requiredFeePerK: 1000 }),
    // A sidechain (chain.json role "sidechain"): its mainchain node, through the host (peg.*).
    'peg.connection': () => ({ connected: true, node: '127.0.0.1:29554', slot: 5, via: 'json-rpc', walletSet: false, wallet: '', auth: 'cookie', cookieFile: '~/.chains/regtest/.cookie' }),
    'peg.call': (p) => {
      const args = (p.params ?? []) as number[];
      switch (p.method) {
        case 'getdrivechaininfo':
          return { withdrawalperiod: 600, withdrawalminscore: 300, height: height + 500 };
        case 'listwithdrawalbundles':
          return [{ slot: 5, hash: 'cd'.repeat(32), index: 0, height: height + 400, score: 120, lastupvote: height + 500, blocksleft: 480, payable: false, known: true, vote: 'upvote' }];
        case 'getsidechainevents':
          return Array.from({ length: Math.min(60, Number(args[2])) }, (_, i) => {
            const h = Number(args[1]) + Number(args[2]) - 60 + i;
            return { height: h, hash: hash(h + 7), previousblockhash: hash(h + 6), time: start - (60 - i) * 60, bmm: i % 3 ? hash(h) : undefined, deposits: i === 58 ? [{ destination: 's5_rsc1qdeposit_abcdef', amount: 2, txid: 'de'.repeat(32), burnindex: 0 }] : [], bundles: [], proposed: i === 10 ? ['cd'.repeat(32)] : [], pending: i >= 10 ? [{ hash: 'cd'.repeat(32), score: i * 2 }] : [] };
          });
        case 'getbalances':
          return { mine: { trusted: 42 } };
        case 'getnewaddress':
          return 'rchn1qmainchainwallet';
        default:
          return null;
      }
    },
    'peg.deposit': () => ({ txid: 'de'.repeat(32), total: 9.25 }),
    'bmm.set': (p: Record<string, unknown>) => ({ mining: p.mine === true }),
    'bmm.request': () => ({ hash: 'bc'.repeat(32), mainchainheight: 101, transactions: 1, fees: 0, amount: 0.0001, mainchaintxid: 'ab'.repeat(32) }),
  };

  /** Records as the host sends them (RecordJson): a salary, some spending, mined coins. */
  const history = () => {
    const rows: unknown[][] = [];
    const items: [string, string, string, number, number][] = [
      ['recv', 'Salary', 'rchn1qsalary', 40, 1_250_000_000],
      ['send', 'Coffee shop', 'rchn1qcoffee', 34, -42_002_820],
      ['mined', '', '', 30, 5_000_000_000],
      ['send', 'Rent', 'rchn1qrent', 24, -400_002_820],
      ['recv', 'Refund', 'rchn1qrefund', 16, 75_000_000],
      ['send', 'Savings', 'rchn1qsavings', 9, -300_002_820],
      ['recv', 'Bookshop', 'rchn1qbook', 3, 15_000_000],
      ['send', 'Bookshop', 'rchn1qbook2', 0, -15_002_820],
    ];
    items.forEach(([type, label, address, ago, amount], i) => {
      const depth = ago === 0 ? 0 : ago * 6;
      rows.push([(i + 10).toString(16).padStart(64, 'a'), 0, type, address, label, start - ago * 86400, amount > 0 ? amount : 0, amount < 0 ? amount : 0, depth === 0 ? 'unconfirmed' : type === 'mined' && depth < 100 ? 'immature' : 'confirmed', depth, 0, depth > 0, `k${i}`]);
    });
    return rows;
  };

  const rpc: Record<string, (params: unknown[]) => unknown> = {
    getbalances: () => ({ mine: { trusted: 12.5, untrusted_pending: 0.25, immature: 50 } }),
    getblockheader: (p) => {
      const h = parseInt(String(p[0]), 16);
      return { height: h, time: start - (height - h) * 600, nTx: (h * 7) % 13 + 1, previousblockhash: h > 0 ? hash(h - 1) : undefined };
    },
    getblockchaininfo: () => ({ chain: 'regtest', blocks: height, headers: height, time: start, size_on_disk: 3_200_000, pruned: false }),
    getnetworkinfo: () => ({ subversion: '/Chains:32.0.0/', connections: 8, connections_in: 2, connections_out: 6, localaddresses: [] }),
    getmempoolinfo: () => ({ size: 3, usage: 4096 }),
    uptime: () => 3600,
    validateaddress: (p) => ({ isvalid: String(p[0]).startsWith('rchn1') }),
    getpeerinfo: () =>
      [1, 2, 3, 4].map((id) => ({ id, addr: `203.0.113.${id * 11}:38333`, inbound: id === 3, pingtime: 0.02 * id, synced_blocks: height, network: 'ipv4', connection_type: 'outbound-full-relay', subver: '/Chains:32.0.0/', version: 70016, startingheight: 1, synced_headers: height, bytessent: 1, bytesrecv: 2, conntime: start, lastsend: start, lastrecv: start })),
    getmininginfo: () => ({ blocks: height, difficulty: 4.6565e-10, networkhashps: 12.4, pooledtx: 3 }),
    getnetworkhashps: (p) => 12.4 * (1 + 0.2 * Math.sin((Number(p[1]) < 0 ? height : Number(p[1])) / 50)),
    getdrivechaininfo: () => ({ activesidechains: 2, proposals: 1, pendingbundles: 1, escrowtotal: 7.25, withdrawalperiod: 100 }),
    listactivesidechains: () => [
      { slot: 2, title: 'Thunder', description: 'Fast payments', escrow: { txid: 'aa', n: 0, amount: 5.25 }, pendingbundles: 1 },
      { slot: 5, title: 'BitNames', description: 'Names and their records', escrow: { txid: 'bb', n: 0, amount: 2 }, pendingbundles: 0 },
    ],
    listwithdrawalbundles: () => [{ slot: 2, hash: 'cd'.repeat(32), score: 14, blocksleft: 86, payable: false }],
    getmainchaininfo: () => ({ slot: 5, connected: true, node: '127.0.0.1:29554', height: height + 500, bestblockhash: hash(height + 500), tipheight: height + 498 }),
    getpeginfo: () => ({
      via: 'json-rpc', height: height + 500, rules: 'chains', withdrawalperiod: 600, withdrawalminscore: 300, activesidechains: 2, proposals: 1,
      sidechain: { slot: 5, active: true, activationheight: 120, title: 'BitNames' }, escrow: { txid: 'bb'.repeat(32), n: 0, amount: 2 },
      bundles: [{ hash: 'cd'.repeat(32), height: height + 400, score: 120, blocksleft: 480, payable: false, vote: 'upvote' }], votehistory: true,
    }),
    getbmminfo: () => ({ mining: true, address: 'rsc1qminer', always: false, idle: false, lastfees: 0.0012, lastoffer: 0.001188, requests: 31, blocks: 27, outbid: 2, candidates: 1 }),
    getwithdrawalbundle: () => ({ status: 'pending', hash: 'cd'.repeat(32), height: height - 20, withdrawals: 2, amount: 1.5, mainchainfee: 0.0002, waiting: 1 }),
    listwithdrawals: () => [
      { txid: 'w1'.repeat(32), vout: 0, amount: 1, mainchainfee: 0.0001, mainchainscript: '00', refundaddress: 'rsc1qrefund', height: height - 30, status: 'bundled' },
      { txid: 'w2'.repeat(32), vout: 1, amount: 0.25, mainchainfee: 0.0001, mainchainscript: '00', refundaddress: 'rsc1qrefund2', height: height - 2, status: 'waiting' },
    ],
    getaddressinfo: () => ({ ismine: true }),
    getdepositaddress: () => ({ depositaddress: 's5_rsc1qdeposit_abcdef', address: 'rsc1qdeposit', slot: 5 }),
    getsidechainevents: (p) => Array.from({ length: Number(p[2]) }, (_, i) => ({ height: Number(p[1]) + i, bmm: (i * (Number(p[0]) + 3)) % 4 !== 0 ? 'ab' : undefined })),
  };

  setTransport(async (request: BridgeRequest) => {
    if (request.kind === 'cancel') return true;
    const params = request.params ?? [];
    if (request.kind === 'host') {
      const fn = host[request.method ?? ''];
      if (!fn) throw { code: -32601, message: `No host method ${request.method}` };
      return fn(params as Record<string, unknown>);
    }
    const fn = rpc[request.method ?? ''];
    if (!fn) throw { code: -32601, message: 'Method not found' };
    return fn(params as unknown[]);
  });

  // A block now and then.
  setInterval(() => {
    height++;
    dispatch([{ type: 'tip', data: { height, time: Math.floor(Date.now() / 1000), hash: hash(height), progress: 1, state: 'synced', ibd: false, loadingBlocks: false } }]);
  }, 15000);
}
