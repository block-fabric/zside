// The fixes of the audit of chains-app (doc/webui-audit.md) in the interface.
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import Shell from '../views/Shell.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { buildCommands, menuSections } from '../lib/commands.svelte';
import { dialogs } from '../lib/dialogs.svelte';
import { go, nav } from '../lib/nav.svelte';
import { app, node, prefs, wallets } from '../lib/state.svelte';
import { resetWalletData } from '../lib/walletdata.svelte';

beforeAll(() => {
  const proto = HTMLDialogElement.prototype as unknown as Record<string, unknown>;
  proto.showModal ??= function (this: HTMLDialogElement) {
    this.setAttribute('open', '');
  };
  proto.close ??= function (this: HTMLDialogElement) {
    this.removeAttribute('open');
  };
  // What jsdom lacks and the layouts use (the palette, the narrow window).
  window.matchMedia ??= ((query: string) => ({ matches: false, media: query, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia;
  Element.prototype.scrollIntoView ??= function () {};
});

function info(walletEnabled: boolean) {
  return { name: 'Chains', version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp/x', walletEnabled, devtools: false, test: true, platform: 'linux', blockSpacing: 600 };
}

beforeEach(() => {
  app.info = info(true);
  prefs.layout = 'dashboard';
  prefs.unit = 'CHN';
  prefs.mask = false;
  nav.page = 'overview';
  nav.tab = '';
  node.tip = { height: 130, time: 1_700_000_000, hash: '00'.repeat(32), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
});

afterEach(() => {
  setTransport(null);
  dialogs.queue.splice(0);
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
});

/** The host: what the handlers answer, an error for the rest. */
function host(handlers: Record<string, (r: BridgeRequest) => unknown> = {}): BridgeRequest[] {
  const calls: BridgeRequest[] = [];
  setTransport(async (r) => {
    calls.push(r);
    const h = handlers[r.method ?? ''];
    if (!h) throw { code: -32601, message: `No ${r.method}` };
    return h(r);
  });
  return calls;
}

/** The menus of chains-qt: the sections that have items (the workspace's Menus button and the palette show them). */
async function menuLabels(): Promise<string[]> {
  return menuSections(buildCommands())
    .filter((s) => s.items.length > 0)
    .map((s) => s.title);
}

describe('a node without wallets (-disablewallet, C1)', () => {
  it('has no wallet menus nor wallet pages, and never calls the wallet functions', async () => {
    app.info = info(false);
    const calls = host();
    render(Shell);
    await waitFor(() => expect(document.querySelector('.content')).toBeTruthy());
    const menus = await menuLabels();
    expect(menus.length).toBeGreaterThan(0);
    expect(menus).not.toContain('Your wallet');
    expect(menus).not.toContain('Banking');
    expect(document.body.textContent).toContain('This node runs without wallets');
    expect(document.body.textContent).not.toContain('Create or open a wallet');
    // The sidebar shows no wallet pages.
    expect(screen.queryByRole('button', { name: /^Send/ })).toBeNull();
    // The Wallets page, if reached, says why instead of calling the wallet functions.
    go('wallets', 'create');
    await waitFor(() => screen.getByRole('heading', { name: 'Wallets' }));
    expect(document.body.textContent).toContain('-disablewallet');
    expect(calls.some((c) => c.method?.startsWith('wallets.'))).toBe(false);
  });

  it('shows the wallet pages without a wallet as a way to make one, not a placeholder (L1)', async () => {
    host();
    render(Shell);
    go('send');
    await waitFor(() => screen.getByRole('heading', { name: 'Send' }));
    expect(document.body.textContent).toContain('This page needs a wallet');
    expect(document.body.textContent).not.toMatch(/being rebuilt|Parity items/);
    await fireEvent.click(screen.getByRole('button', { name: 'Open wallet…' }));
    expect(nav.page).toBe('wallets');
  });
});

describe('the settings wait for Apply (M7)', () => {
  const OPTIONS = {
    dbcache: 450, par: 0, prune: false, pruneSize: 2, spendZeroConf: true, signer: '', natpmp: false, listen: true, server: false,
    proxyUse: false, proxyIp: '127.0.0.1', proxyPort: '9050', torUse: false, torIp: '127.0.0.1', torPort: '9050', lang: '', startAtLogin: false,
    ignored: {}, overridden: '', limits: { dbcacheMin: 4, parMin: -8, parMax: 15, pruneMinGB: 1 }, proxyReach: { ipv4: false, ipv6: false, tor: false },
    externalSigner: false, restartRequired: false, pending: 0,
  };

  async function settingsPage(tab: string) {
    const options = { ...OPTIONS };
    const calls = host({
      'options.get': () => ({ ...options }),
      'options.set': (r) => {
        const { name, value } = r.params as { name: string; value: unknown };
        Object.assign(options, { [name]: value, restartRequired: true, pending: options.pending + 1 });
        return { ...options };
      },
      'options.apply': () => Object.assign(options, { pending: 0 }) && { ...options },
      'options.cancel': () => Object.assign(options, { ...OPTIONS, pending: 0 }) && { ...options },
      'prefs.set': () => ({}),
      'tray.state': () => ({ available: true, visible: true, windowVisible: true }),
    });
    const { default: DialogHost } = await import('../components/DialogHost.svelte');
    const { default: Settings } = await import('../pages/Settings.svelte');
    nav.tab = tab;
    render(DialogHost);
    render(Settings);
    return calls;
  }

  it('asks before applying a prune, and applies nothing on Cancel', async () => {
    const calls = await settingsPage('main');
    await waitFor(() => screen.getByLabelText('Prune block storage to'));
    await fireEvent.click(screen.getByLabelText('Prune block storage to'));
    await waitFor(() => screen.getByText('1 change is not applied yet.'));
    expect(calls.some((c) => c.method === 'options.apply')).toBe(false);
    // Apply: the risky change is confirmed first; "Cancel" there leaves it waiting.
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => screen.getByText('Some of these changes are hard to take back. Apply them?'));
    expect(document.body.textContent).toContain('Reverting this setting requires re-downloading the entire blockchain.');
    const dialog = document.querySelector('dialog[open]') as HTMLElement;
    await fireEvent.click([...dialog.querySelectorAll('button')].find((b) => b.textContent?.trim() === 'Cancel')!);
    expect(calls.some((c) => c.method === 'options.apply')).toBe(false);
    // Cancel on the page: taken back by the host.
    await fireEvent.click(screen.getAllByRole('button', { name: 'Cancel' }).at(-1)!);
    await waitFor(() => expect(calls.some((c) => c.method === 'options.cancel')).toBe(true));
    await waitFor(() => screen.getByText('Changes wait here until you apply them.'));
  });

  it('previews the look, saves it on Apply, and puts it back on Cancel', async () => {
    prefs.theme = 'light';
    const calls = await settingsPage('display');
    await waitFor(() => screen.getByText('Ocean Teal'));
    await fireEvent.click(screen.getByLabelText('Ocean Teal'));
    expect(prefs.theme).toBe('ocean');
    expect(calls.some((c) => c.method === 'prefs.set')).toBe(false);
    await fireEvent.click(screen.getAllByRole('button', { name: 'Cancel' }).at(-1)!);
    await waitFor(() => expect(prefs.theme).toBe('light'));
    await fireEvent.click(screen.getByLabelText('Matrix Green'));
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => expect(calls.find((c) => c.method === 'prefs.set')?.params).toEqual({ theme: 'matrix' }));
    expect(prefs.theme).toBe('matrix');
  });

  it('takes the external signer only from the file chooser of the desktop, never as text (security audit 2026-10-14, 2b)', async () => {
    const options = { ...OPTIONS, externalSigner: true };
    const calls = host({
      'options.get': () => ({ ...options }),
      'options.set': (r) => {
        const { name, value } = r.params as { name: string; value: unknown };
        Object.assign(options, { [name]: value });
        return { ...options };
      },
      // The host shows its file chooser and its confirmation, then answers.
      'options.chooseProgram': () => Object.assign(options, { signer: '/usr/local/bin/hwi', restartRequired: true, pending: 1 }) && { ...options },
      'tray.state': () => ({ available: true, visible: true, windowVisible: true }),
    });
    const { default: Settings } = await import('../pages/Settings.svelte');
    nav.tab = 'wallet';
    render(Settings);
    const field = (await waitFor(() => screen.getByLabelText('External signer script path'))) as HTMLInputElement;
    expect(field.readOnly).toBe(true);
    // Typing changes nothing on the host.
    field.value = '/tmp/evil';
    await fireEvent.input(field);
    await fireEvent.change(field);
    expect(calls.some((c) => c.method === 'options.set' && (c.params as { name: string }).name === 'signer')).toBe(false);
    await fireEvent.click(screen.getByRole('button', { name: 'Choose…' }));
    await waitFor(() => expect((screen.getByLabelText('External signer script path') as HTMLInputElement).value).toBe('/usr/local/bin/hwi'));
    expect(calls.find((c) => c.method === 'options.chooseProgram')?.params).toEqual({ name: 'signer' });
    // Clear is the one thing the page sets: an empty value.
    await fireEvent.click(screen.getByRole('button', { name: 'Clear' }));
    await waitFor(() => expect((screen.getByLabelText('External signer script path') as HTMLInputElement).value).toBe(''));
    const sets = calls.filter((c) => c.method === 'options.set').map((c) => c.params as { name: string; value: unknown });
    expect(sets).toEqual([{ name: 'signer', value: '' }]);
  });
});

describe('the smaller findings (L2 to L10)', () => {
  it('lines the table headers up with their columns, and sorts from the keyboard (L2, L9)', async () => {
    const { default: TableHarness } = await import('./TableHarness.svelte');
    render(TableHarness, { count: 20 });
    // The rows are not the .row of base.css: its gap widened every column of the rows, not of the head.
    const rows = [...document.querySelectorAll('[role="grid"] [role="row"]')].slice(1);
    expect(rows.length).toBeGreaterThan(0);
    for (const r of rows) expect(r.classList.contains('row')).toBe(false);
    const sort = screen.getAllByRole('columnheader')[0].querySelector('button') as HTMLButtonElement;
    expect(sort.tabIndex).toBe(0);
    sort.focus();
    expect(document.activeElement).toBe(sort);
  });

  it('opens the sync overlay by itself while behind, until the user closes it (L3)', async () => {
    host();
    const { default: SyncOverlay } = await import('../components/SyncOverlay.svelte');
    const { overlay } = await import('../lib/syncoverlay');
    overlay.userClosed = false;
    overlay.shown = false;
    nav.dialog = null;
    node.tip = { height: 1, time: 1_300_000_000, hash: '11'.repeat(32), progress: 0.1, state: 'init', ibd: true, loadingBlocks: false };
    render(SyncOverlay);
    await waitFor(() => expect(nav.dialog).toBe('sync'));
    expect(document.body.textContent).toContain('Attempting to spend coins that are affected by not-yet-displayed transactions will not be accepted by the network.');
    await fireEvent.click(screen.getByRole('button', { name: 'Hide' }));
    expect(nav.dialog).toBeNull();
    expect(overlay.userClosed).toBe(true);
    // Caught up: an overlay that opened by itself goes.
    overlay.userClosed = false;
    overlay.shown = false;
    node.tip = { ...node.tip!, height: 2, hash: '22'.repeat(32) };
    await waitFor(() => expect(nav.dialog).toBe('sync'));
    node.tip = { ...node.tip!, height: 3, time: Math.floor(Date.now() / 1000), hash: '33'.repeat(32), ibd: false };
    await waitFor(() => expect(nav.dialog).toBeNull());
  });

  it('says what is wrong with an amount, refuses dust, a bad custom fee, and a signer without its script (L4, L5, L6)', async () => {
    const { default: Send } = await import('../pages/Send.svelte');
    const INFO = { name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, externalSigner: false, signerConfigured: false, hd: true, canGetAddresses: true, taproot: true, defaultAddressType: 'bech32', confirmTarget: 6, requiredFeePerK: 1000, maxTxFee: 10_000_000, multiwallet: false, balance: 5_000_000_000, unconfirmed: 0, immature: 0 };
    let info = { ...INFO };
    const calls = host({
      'wallet.info': () => info,
      'wallet.fee': () => ({ feePerK: 1000, fallback: false, target: 6, requiredFeePerK: 1000 }),
      'send.isDust': (r) => Number((r.params as { amount: number }).amount) < 546,
      'wallet.send.prepare': () => {
        throw { code: -6, message: 'should not be asked' };
      },
      validateaddress: () => ({ isvalid: true }),
    });
    wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
    wallets.current = 'w';
    wallets.balances.w = { trusted: 5_000_000_000n, untrustedPending: 0n, immature: 0n };
    prefs.feeMode = 'smart';
    const { regtestP2wpkh } = await import('./bech32');
    render(Send);
    await fireEvent.input(screen.getByLabelText('Pay to'), { target: { value: regtestP2wpkh } });
    // The address is checked offline, then by the node.
    await new Promise((resolve) => setTimeout(resolve, 500));
    // Too many decimals: the toast says so, not "larger than 0".
    await fireEvent.input(screen.getByLabelText('Amount'), { target: { value: '1.123456789' } });
    const send = await waitFor(() => screen.getByRole('button', { name: /^Send/ }) as HTMLButtonElement);
    await waitFor(() => expect(send.disabled).toBe(false));
    await fireEvent.click(send);
    const { toasts } = await import('../lib/state.svelte');
    await waitFor(() => expect(toasts.list.at(-1)?.message ?? '').not.toBe('The amount to pay must be larger than 0.'));
    expect(toasts.list.at(-1)?.message).toMatch(/decimal|digits|precision/i);
    // Dust.
    await fireEvent.input(screen.getByLabelText('Amount'), { target: { value: '0.000001' } });
    await waitFor(() => screen.getByText('The amount is too small to send: the output would be dust.'));
    await fireEvent.click(send);
    await waitFor(() => expect(toasts.list.at(-1)?.message).toBe('The amount is too small to send: the output would be dust.'));
    // A custom fee that is not valid is not replaced by the smart fee in silence.
    await fireEvent.input(screen.getByLabelText('Amount'), { target: { value: '1' } });
    prefs.feeMode = 'custom';
    prefs.feeMinimized = false;
    await waitFor(() => screen.getByLabelText('Fee per 1000 virtual bytes'));
    await fireEvent.input(screen.getByLabelText('Fee per 1000 virtual bytes'), { target: { value: '0.000005' } });
    await waitFor(() => expect(screen.getByText(/At least the minimum fee/)).toBeTruthy());
    await fireEvent.click(send);
    await waitFor(() => expect(toasts.list.at(-1)?.message ?? '').toMatch(/^Enter a custom fee of at least/));
    expect(calls.some((c) => c.method === 'wallet.send.prepare')).toBe(false);
    prefs.feeMode = 'smart';
    prefs.customFee = 0;
    toasts.list.splice(0);
    // A hardware wallet without the path of its signer script.
    info = { ...INFO, externalSigner: true, privateKeysDisabled: true, signerConfigured: false };
    const { refreshInfo } = await import('../lib/walletdata.svelte');
    await refreshInfo(true);
    await waitFor(() => screen.getByText('Set external signer script path in Options -> Wallet'));
    const sign = screen.getByRole('button', { name: /Sign on device/ }) as HTMLButtonElement;
    expect(sign.disabled).toBe(true);
    expect(sign.title).toBe('Set external signer script path in Options -> Wallet');
    info = { ...INFO, externalSigner: true, privateKeysDisabled: true, signerConfigured: true };
    await refreshInfo(true);
    await waitFor(() => expect((screen.getByRole('button', { name: /Sign on device/ }) as HTMLButtonElement).title).toBe('Connect your hardware wallet first.'));
  });

  it('says "Indexing blocks on disk" while a reindex reads the block files (L7)', async () => {
    const { syncStatus } = await import('../lib/sync');
    const base = { tipHeight: 10, tipTime: 0, progress: 0.1, loadingBlocks: true, headerHeight: 10, headerTime: 0, connections: 0, networkActive: true, spacing: 600, now: 1_700_000_000 };
    expect(syncStatus(base).label).toBe('Processing blocks on disk…');
    expect(syncStatus({ ...base, headerHeight: 5000 }).label).toBe('Indexing blocks on disk…');
  });

  it('tells in the intro what the folder is, and how old a backup the pruned blocks restore (L8)', async () => {
    const { backupDays, pathMessage } = await import('../lib/intro');
    expect(backupDays(2)).toBe(6);
    expect(backupDays(550)).toBe(1697);
    expect(pathMessage({ exists: true, isDirectory: true, free: 1 })?.text).toBe('Directory already exists. Add /name if you intend to create a new directory here.');
    expect(pathMessage({ exists: true, isDirectory: false, free: 1 })).toEqual({ text: 'Path already exists, and is not a directory.', error: true });
    expect(pathMessage({ exists: false, isDirectory: false, free: 1 })?.text).toBe('A new data directory will be created.');
    expect(pathMessage({ error: 'No folder of this path exists' })).toEqual({ text: 'Cannot create data directory here.', error: true });
  });

  it('closes the dialog on top with Ctrl+W, and opens the node tabs from anywhere (L9)', async () => {
    const { handleShortcut } = await import('../lib/keyboard');
    host();
    nav.page = 'overview';
    const key = (k: string) => new KeyboardEvent('keydown', { key: k, ctrlKey: true });
    expect(handleShortcut(key('p'))).toBe(true);
    expect(nav.page).toBe('node');
    expect(nav.tab).toBe('peers');
    nav.page = 'send';
    handleShortcut(key('t'));
    expect([nav.page, nav.tab]).toEqual(['node', 'console']);
    const { default: DialogHost } = await import('../components/DialogHost.svelte');
    const { inform } = await import('../lib/dialogs.svelte');
    render(DialogHost);
    const done = inform('Title', 'A message');
    await waitFor(() => screen.getByText('A message'));
    expect(handleShortcut(key('w'))).toBe(true);
    await done;
    expect(dialogs.queue.length).toBe(0);
    // No dialog: Ctrl+W closes the node window.
    expect(handleShortcut(key('w'))).toBe(true);
    expect(nav.page).toBe('overview');
  });

  it('shows two notices at most, the others waiting behind them (L10)', async () => {
    const { default: Toasts } = await import('../components/Toasts.svelte');
    const { toast, toasts } = await import('../lib/state.svelte');
    toasts.list.splice(0);
    render(Toasts);
    for (let i = 0; i < 4; i++) toast('info', `Notice ${i}`, 'text', 0);
    await waitFor(() => screen.getByText('2 more'));
    expect(document.querySelectorAll('.toast').length).toBe(2);
    expect(screen.getByText('Notice 3')).toBeTruthy();
    toasts.list.splice(0);
  });
});

describe('the parity items finished with the audit', () => {
  it('shows the status tip of a menu item in the status bar (C14)', async () => {
    host({ 'tray.state': () => ({ available: false, visible: false, windowVisible: true }) });
    // The menus of chains-qt are in the workspace's Menus button (the dashboard's sidebar has none: user decision).
    prefs.layout = 'workspace';
    render(Shell);
    await fireEvent.click(screen.getAllByRole('button', { name: /^Menus of chains-qt/ })[0]);
    const item = await waitFor(() => screen.getByRole('menuitem', { name: /Backup wallet/ }));
    await fireEvent.mouseEnter(item);
    await waitFor(() => expect(document.querySelector('.statusbar .tip')?.textContent).toBe('Backup wallet to another location'));
    await fireEvent.mouseLeave(item);
    await waitFor(() => expect(document.querySelector('.statusbar .tip')).toBeNull());
  });
});
