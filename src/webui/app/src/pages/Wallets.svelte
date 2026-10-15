<script lang="ts">
  import { every } from '../lib/poll';
  import { t } from '../lib/i18n.svelte';
  import { CHAIN } from '../chain/config';
  // Wallets (WalletController and its activities, the wallet menu of chains-qt): create, open, close,
  // restore, migrate; encrypt, change the passphrase, unlock for a while, back up, export watch-only.
  import SafeText from '../components/SafeText.svelte';
  import Dialog from '../components/Dialog.svelte';
  import Icon from '../components/Icon.svelte';
  import { ask, askPassphrase, confirm, errorText, inform, prompt } from '../lib/dialogs.svelte';
  import { chooseFile, FILTERS, safeFileName } from '../lib/files';
  import { go, nav } from '../lib/nav.svelte';
  import { rpc } from '../lib/rpc';
  import { app, selectWallet, wallets as walletState } from '../lib/state.svelte';
  import { walletApi, walletDisplayName, wallets, WalletError, type MigrateResult, type WalletDirEntry } from '../lib/wallet';
  import { BridgeError } from '../lib/bridge';
  import { current, refreshInfo } from '../lib/walletdata.svelte';

  let dir: WalletDirEntry[] = $state([]);
  let busy: string | null = $state(null);
  let create = $state({ open: false, name: '', encrypt: false, disableKeys: false, blank: false, signer: false, signers: [] as string[], supported: false, signerError: '' });
  let unlockFor = $state({ open: false, minutes: 5 });
  let unlockedUntil = $state(0);
  let now = $state(Math.floor(Date.now() / 1000));

  const name = $derived(walletState.current);
  const summary = $derived(walletState.list.find((w) => w.name === name) ?? null);
  const info = $derived(current.info);
  const product = $derived(app.info?.name ?? CHAIN.name);

  async function loadDir() {
    try {
      dir = await wallets.list();
    } catch {
      dir = [];
    }
  }

  $effect(() => {
    void walletState.list.length;
    loadDir();
  });
  $effect(() => {
    void summary?.locked;
    void summary?.encrypted;
    if (name !== null) refreshInfo(true);
  });

  // When the wallet locks again by itself (walletpassphrase with a timeout).
  $effect(() => {
    void summary?.locked;
    if (name === null) return;
    rpc
      .raw<{ unlocked_until?: number }>('getwalletinfo', [], { wallet: name })
      .then((w) => (unlockedUntil = w.unlocked_until ?? 0))
      .catch(() => (unlockedUntil = 0));
  });
  $effect(() => {
    return every(1000, () => (now = Math.floor(Date.now() / 1000)));
  });

  // The actions of the menus arrive as tabs of this page.
  $effect(() => {
    const t = nav.tab;
    if (!t) return;
    nav.tab = '';
    const actions: Record<string, () => unknown> = {
      create: openCreate,
      open: () => document.getElementById('wallet-dir')?.focus(),
      close: closeCurrent,
      'close-all': closeAll,
      migrate: () => document.getElementById('wallet-dir')?.focus(),
      'restore-migrate': restoreMigrate,
      backup,
      restore,
      export: exportWatchOnly,
      encrypt,
      passphrase: changePassphrase,
      unlock: () => (unlockFor.open = true),
    };
    actions[t]?.();
  });

  async function run<T>(label: string, fn: () => Promise<T>): Promise<T | null> {
    busy = label;
    try {
      return await fn();
    } finally {
      busy = null;
    }
  }

  function warnings(list: string[] | undefined): string {
    return (list ?? []).join('\n');
  }

  // --- Create (CreateWalletDialog, CreateWalletActivity) ---

  async function openCreate() {
    create = { open: true, name: '', encrypt: false, disableKeys: false, blank: false, signer: false, signers: [], supported: false, signerError: '' };
    try {
      const s = await wallets.signers();
      create.supported = s.supported;
      create.signers = s.signers.map((x) => x.name);
      create.signerError = s.error ?? '';
      if (create.signers.length === 1) {
        // A device is there: an external signer wallet, named after it.
        create.signer = true;
        create.disableKeys = true;
        create.name = create.signers[0];
      }
    } catch {
      // No signers.
    }
  }

  function toggleEncrypt() {
    if (create.encrypt) {
      create.disableKeys = false;
      create.signer = false;
    }
  }
  function toggleDisableKeys() {
    if (create.disableKeys) {
      create.blank = false;
      create.encrypt = false;
    }
  }
  function toggleBlank() {
    if (create.blank) create.disableKeys = false;
  }
  function toggleSigner() {
    create.encrypt = false;
    create.blank = false;
    create.disableKeys = create.signer;
  }

  async function confirmEncryption(): Promise<boolean> {
    const r = await ask({
      title: t('Confirm wallet encryption'),
      message: t('Warning: If you encrypt your wallet and lose your passphrase, you will LOSE ALL OF YOUR COINS!'),
      informative: t('Are you sure you wish to encrypt your wallet?'),
      level: 'warning',
      choices: [
        { id: 'back', label: 'Back', kind: 'primary' },
        { id: 'continue', label: 'Continue', kind: 'danger' },
      ],
    });
    return r.choice === 'continue';
  }

  async function doCreate() {
    const options = { ...create };
    if (!options.name) return;
    create.open = false;
    let passphrase: string | undefined;
    if (options.encrypt) {
      const p = await askPassphrase('encrypt');
      if (!p) return;
      if (!(await confirmEncryption())) return;
      const sure = await confirm(
        t('Wallet to be encrypted'),
        t('Your wallet is about to be encrypted. Remember that encrypting your wallet cannot fully protect your coins from being stolen by malware infecting your computer. Are you sure you wish to encrypt your wallet?'),
        { level: 'warning' },
      );
      if (!sure) return;
      passphrase = p.next;
    }
    try {
      const r = await run(`Creating Wallet ${options.name}…`, () =>
        wallets.create({ name: options.name, passphrase, disablePrivateKeys: options.disableKeys, blank: options.blank, externalSigner: options.signer }),
      );
      if (!r) return;
      if (r.warnings.length) await inform(t('Create wallet warning'), warnings(r.warnings), 'warning');
      selectWallet(r.name);
      await loadDir();
    } catch (e) {
      await inform(t('Create wallet failed'), errorText(e), 'error');
    }
  }

  // --- Open, close (OpenWalletActivity, WalletController::closeWallet) ---

  async function openWallet(w: WalletDirEntry) {
    try {
      const r = await run(`Opening Wallet ${walletDisplayName(w.name)}…`, () => wallets.open(w.name));
      if (!r) return;
      if (r.warnings.length) await inform(t('Open wallet warning'), warnings(r.warnings), 'warning');
      selectWallet(r.name);
      await loadDir();
    } catch (e) {
      await inform(t('Open wallet failed'), errorText(e), 'error');
    }
  }

  async function closeWallet(walletName: string) {
    const yes = await confirm(t('Close wallet'), t('Are you sure you wish to close the wallet %1?', walletDisplayName(walletName)), {
      informative: t('Closing the wallet for too long can result in having to resync the entire chain if pruning is enabled.'),
      yes: 'Yes',
    });
    if (!yes) return;
    try {
      await wallets.close(walletName);
      await loadDir();
    } catch (e) {
      await inform(t('Close wallet'), errorText(e), 'error');
    }
  }

  async function closeCurrent() {
    if (name !== null) await closeWallet(name);
  }

  async function closeAll() {
    if (!(await confirm(t('Close all wallets'), t('Are you sure you wish to close all wallets?')))) return;
    for (const w of [...walletState.list]) await wallets.close(w.name).catch(() => undefined);
    await loadDir();
  }

  // --- Restore, migrate (RestoreWalletActivity, MigrateWalletActivity) ---

  async function restore() {
    const file = await chooseFile({ title: 'Load Wallet Backup', filters: [FILTERS.walletBackup, FILTERS.all] });
    if (!file) return;
    const walletName = await prompt(t('Restore Wallet'), t('Wallet Name'));
    if (walletName === null) return;
    if (!walletName) {
      await inform(t('Invalid Wallet Name'), t('Wallet name cannot be empty'), 'error');
      return;
    }
    try {
      const r = await run(`Restoring Wallet ${walletName}…`, () => wallets.restore(file, walletName));
      if (!r) return;
      if (r.warnings.length) await inform(t('Restore wallet warning'), warnings(r.warnings), 'warning');
      else await inform(t('Restore wallet message'), t('Wallet restored successfully'));
      selectWallet(r.name);
      await loadDir();
    } catch (e) {
      await inform(t('Restore wallet failed'), errorText(e), 'error');
    }
  }

  const MIGRATE_TEXT =
    'Migrating the wallet will convert this wallet to one or more descriptor wallets. A new wallet backup will need to be made.\n' +
    'If this wallet contains any watchonly scripts, a new wallet will be created which contains those watchonly scripts.\n' +
    'If this wallet contains any solvable but not watched scripts, a different and new wallet will be created which contains those scripts.\n\n' +
    'The migration process will create a backup of the wallet before migrating. This backup file will be named <wallet name>-<timestamp>.legacy.bak and can be found in the directory for this wallet. In the event of an incorrect migration, the backup can be restored with the "Restore Wallet" functionality.';

  function migrated(r: MigrateResult): string {
    let text = t("The wallet '%1' was migrated successfully.", walletDisplayName(r.name));
    if (r.watchonlyName) text += ' ' + t("Watchonly scripts have been migrated to a new wallet named '%1'.", walletDisplayName(r.watchonlyName));
    if (r.solvablesName) text += ' ' + t("Solvable but not watched scripts have been migrated to a new wallet named '%1'.", walletDisplayName(r.solvablesName));
    if (!r.loaded) text += ' ' + t('The wallet was not loaded after migration. You can open it from the "Your wallet > Open wallet" menu.');
    return text;
  }

  async function doMigrate(walletName: string, load: boolean) {
    let passphrase: string | undefined;
    if (await wallets.isEncrypted(walletName).catch(() => false)) {
      const p = await askPassphrase('migrate', { wallet: walletName });
      if (!p) return;
      passphrase = p.passphrase;
    }
    try {
      const r = await run(`Migrating Wallet ${walletDisplayName(walletName)}…`, () => wallets.migrate(walletName, passphrase, load));
      if (!r) return;
      await inform(t('Migration Successful'), migrated(r));
      if (r.loaded) selectWallet(r.name);
      await loadDir();
    } catch (e) {
      await inform(t('Migration failed'), errorText(e), 'error');
    }
  }

  async function migrate(w: WalletDirEntry) {
    const r = await ask({
      title: t('Migrate wallet'),
      message: t('Are you sure you wish to migrate the wallet %1?', walletDisplayName(w.name)),
      informative: MIGRATE_TEXT,
      level: 'question',
      checkbox: { label: 'Load wallet after migration', checked: true, tip: 'If the node is pruned and the wallet was created before the pruned height, the migration process may fail trying to load the migrated wallet.' },
      choices: [
        { id: 'cancel', label: 'Cancel' },
        { id: 'yes', label: 'Yes', kind: 'primary' },
      ],
    });
    if (r.choice === 'yes') await doMigrate(w.name, r.checked);
  }

  async function restoreMigrate() {
    const file = await chooseFile({ title: 'Restore and Migrate Wallet Backup', filters: [FILTERS.wallet] });
    if (!file) return;
    const walletName = await prompt(t('Restore and Migrate Wallet'), t('Wallet Name'));
    if (!walletName) return;
    const yes = await ask({
      title: t('Restore and Migrate wallet'),
      message: t('Are you sure you wish to restore the wallet file %1 to %2 and migrate it?', file, walletDisplayName(walletName)),
      informative: 'Restoring the wallet will copy the backup file to the wallets directory and place it in the standard wallet directory layout. The original file will not be modified.\n\n' + MIGRATE_TEXT,
      level: 'question',
      choices: [
        { id: 'cancel', label: 'Cancel' },
        { id: 'yes', label: 'Yes', kind: 'primary' },
      ],
    });
    if (yes.choice !== 'yes') return;
    try {
      const r = await run(`Restoring Wallet ${walletDisplayName(walletName)}…`, () => wallets.restoreMigrate(file, walletName));
      if (!r) return;
      if ((r as MigrateResult & { needsPassphrase?: boolean }).needsPassphrase) {
        await doMigrate(walletName, true);
        return;
      }
      await inform(t('Migration Successful'), migrated(r));
      if (r.loaded) selectWallet(r.name);
      await loadDir();
    } catch (e) {
      await inform(t('Migration failed'), errorText(e), 'error');
    }
  }

  // --- The current wallet: encryption, backups (AskPassphraseDialog, WalletView) ---

  async function encrypt() {
    if (name === null) return;
    const p = await askPassphrase('encrypt', { wallet: name });
    if (!p?.next) return;
    if (!(await confirmEncryption())) return;
    try {
      await run('Encrypting the wallet…', () => walletApi(name).encrypt(p.next!));
      await inform(
        t('Wallet encrypted'),
        t('Your wallet is now encrypted. Remember that encrypting your wallet cannot fully protect your coins from being stolen by malware infecting your computer.'),
        'warning',
        t('IMPORTANT: Any previous backups you have made of your wallet file should be replaced with the newly generated, encrypted wallet file. For security reasons, previous backups of the unencrypted wallet file will become useless as soon as you start using the new, encrypted wallet.'),
      );
      await refreshInfo(true);
    } catch (e) {
      await inform(t('Wallet encryption failed'), errorText(e), 'error');
    }
  }

  async function changePassphrase() {
    if (name === null) return;
    let error: string | undefined;
    for (;;) {
      const p = await askPassphrase('change', { wallet: name, error });
      if (!p?.next) return;
      try {
        await walletApi(name).changePassphrase(p.passphrase, p.next);
        await inform(t('Wallet encrypted'), t('Wallet passphrase was successfully changed.'));
        return;
      } catch (e) {
        if (e instanceof BridgeError && e.code === WalletError.PASSPHRASE_INCORRECT) {
          error = e.message;
          continue;
        }
        await inform(t('Passphrase change failed'), errorText(e), 'error');
        return;
      }
    }
  }

  async function unlockTimed() {
    if (name === null) return;
    unlockFor.open = false;
    let error: string | undefined;
    for (;;) {
      const p = await askPassphrase('unlock', { wallet: name, error });
      if (!p) return;
      try {
        await rpc.raw('walletpassphrase', [p.passphrase, Math.max(1, Math.round(unlockFor.minutes * 60))], { wallet: name });
        unlockedUntil = Math.floor(Date.now() / 1000) + Math.round(unlockFor.minutes * 60);
        return;
      } catch (e) {
        if (e instanceof BridgeError && e.code === WalletError.PASSPHRASE_INCORRECT) {
          error = 'The passphrase entered for the wallet decryption was incorrect.';
          continue;
        }
        await inform(t('Wallet unlock failed'), errorText(e), 'error');
        return;
      }
    }
  }

  async function lockNow() {
    if (name === null) return;
    await walletApi(name).lock().catch(() => undefined);
    unlockedUntil = 0;
  }

  async function backup() {
    if (name === null) return;
    const path = await chooseFile({ save: true, title: 'Backup Wallet', name: safeFileName(`${name || 'wallet'}-backup.dat`), filters: [FILTERS.wallet] });
    if (!path) return;
    try {
      await run('Backing up the wallet…', () => walletApi(name).backup(path));
      await inform(t('Backup Successful'), t('The wallet data was successfully saved to %1.', path));
    } catch (e) {
      await inform(t('Backup Failed'), t('There was an error trying to save the wallet data to %1. %2', path, errorText(e)), 'error');
    }
  }

  async function exportWatchOnly() {
    if (name === null) return;
    const path = await chooseFile({ save: true, title: 'Save Watch-only Wallet Export', name: safeFileName(`${name || 'wallet'}-watchonly.dat`), filters: [FILTERS.wallet] });
    if (!path) return;
    try {
      const out = await run('Exporting…', () => walletApi(name).exportWatchOnly(path));
      await inform(t('Export Successful'), t('The wallet has been exported to %1', out ?? path));
    } catch (e) {
      await inform(t('Export Error'), errorText(e), 'error');
    }
  }

  function fmtLeft(secs: number): string {
    const m = Math.floor(secs / 60);
    const s = secs % 60;
    return m ? `${m} min ${String(s).padStart(2, '0')} s` : `${s} s`;
  }
</script>

<div class="page wallets">
  <header class="page-head">
    <h1>Wallets</h1>
    <div class="row">
      <button type="button" class="btn primary" onclick={openCreate}><Icon name="plus" size={16} /> Create wallet…</button>
      <button type="button" class="btn" onclick={restore}><Icon name="upload" size={16} /> Restore wallet…</button>
    </div>
  </header>

  {#if busy}
    <div class="busy panel" role="status"><span class="spinner" aria-hidden="true"></span> {busy}</div>
  {/if}

  <div class="grid">
    <section class="panel current" aria-labelledby="current-title">
      <div class="panel-head">
        <h2 id="current-title">{#if name === null}No wallet loaded{:else}<SafeText text={walletDisplayName(name)} />{/if}</h2>
        {#if summary}
          <div class="badges">
            {#if summary.privateKeysDisabled}<span class="badge">watch-only</span>{/if}
            {#if info?.externalSigner}<span class="badge">hardware wallet</span>{/if}
            {#if summary.hd}<span class="badge accent" title="HD key generation is enabled">HD</span>{/if}
            {#if summary.encrypted}
              <span class="badge" class:warning={!summary.locked} title={summary.locked ? 'Wallet is encrypted and currently locked' : 'Wallet is encrypted and currently unlocked'}>
                <Icon name={summary.locked ? 'lock' : 'unlock'} size={13} />
                {summary.locked ? 'locked' : 'unlocked'}
              </span>
            {:else if !summary.privateKeysDisabled}
              <span class="badge warning">not encrypted</span>
            {/if}
          </div>
        {/if}
      </div>
      {#if name !== null && summary}
        <div class="panel-body actions">
          {#if !summary.encrypted && !summary.privateKeysDisabled}
            <button type="button" class="action" onclick={encrypt}>
              <Icon name="lock" /><span><strong>Encrypt wallet…</strong><small>Encrypt the private keys that belong to your wallet</small></span>
            </button>
          {/if}
          {#if summary.encrypted}
            <button type="button" class="action" onclick={changePassphrase}>
              <Icon name="key" /><span><strong>Change passphrase…</strong><small>Change the passphrase used for wallet encryption</small></span>
            </button>
            {#if summary.locked}
              <button type="button" class="action" onclick={() => (unlockFor.open = true)}>
                <Icon name="unlock" /><span><strong>Unlock for a while…</strong><small>Unlock the wallet for a number of minutes, then lock it again by itself</small></span>
              </button>
            {:else}
              <button type="button" class="action" onclick={lockNow}>
                <Icon name="lock" /><span><strong>Lock now</strong><small>{unlockedUntil > now ? `Locks by itself in ${fmtLeft(unlockedUntil - now)}` : 'The wallet is unlocked'}</small></span>
              </button>
            {/if}
          {/if}
          <button type="button" class="action" onclick={backup}>
            <Icon name="download" /><span><strong>Backup wallet…</strong><small>Backup wallet to another location</small></span>
          </button>
          {#if !summary.privateKeysDisabled}
            <button type="button" class="action" onclick={exportWatchOnly}>
              <Icon name="eye" /><span><strong>Export watch-only wallet…</strong><small>Export a watch-only version of the current wallet that can be restored onto another node.</small></span>
            </button>
          {/if}
          <button type="button" class="action" onclick={() => go('receive', 'addresses')}>
            <Icon name="book" /><span><strong>Receiving addresses</strong><small>Show the list of used receiving addresses and labels</small></span>
          </button>
          <button type="button" class="action danger" onclick={closeCurrent}>
            <Icon name="x" /><span><strong>Close wallet</strong><small>Close the wallet (it is opened again with "Open")</small></span>
          </button>
        </div>
      {:else}
        <div class="panel-body"><p class="muted">Create a wallet, or open one of the wallets of this node.</p></div>
      {/if}
    </section>

    <section class="panel dir" aria-labelledby="dir-title">
      <div class="panel-head">
        <h2 id="dir-title">Wallets of this node</h2>
        <div class="row">
          <button type="button" class="btn ghost small" onclick={restoreMigrate} title="Restore a legacy wallet file and migrate it">Restore and migrate…</button>
          <button type="button" class="btn ghost small" onclick={closeAll} disabled={walletState.list.length === 0}>Close all</button>
        </div>
      </div>
      <ul class="panel-body list" id="wallet-dir" tabindex="-1" aria-label="Wallets in the data directory">
        {#each dir as w (w.name)}
          <li>
            <Icon name="wallet" />
            <span class="grow">
              <strong><SafeText text={walletDisplayName(w.name)} /></strong>
              {#if w.needsMigration}<span class="badge warning">needs migration</span>{/if}
              {#if w.loaded}<span class="badge accent">open</span>{/if}
            </span>
            {#if w.loaded}
              {#if walletState.current !== w.name}<button type="button" class="btn small" onclick={() => selectWallet(w.name)}>Show</button>{/if}
              <button type="button" class="btn small" onclick={() => closeWallet(w.name)}>Close</button>
            {:else if w.needsMigration}
              <button type="button" class="btn small" onclick={() => migrate(w)}>Migrate…</button>
            {:else}
              <button type="button" class="btn small primary" onclick={() => openWallet(w)}>Open</button>
            {/if}
          </li>
        {:else}
          <li class="muted">No wallets available</li>
        {/each}
      </ul>
    </section>
  </div>
</div>

<Dialog bind:open={create.open} title="Create Wallet">
  <form
    id="create-form"
    class="stack"
    onsubmit={(e) => {
      e.preventDefault();
      doCreate();
    }}
  >
    <div class="field">
      <label for="wallet-name">Wallet Name</label>
      <!-- svelte-ignore a11y_autofocus -->
      <input id="wallet-name" type="text" bind:value={create.name} placeholder="Wallet" autocomplete="off" autofocus />
    </div>
    <label class="check option">
      <input type="checkbox" bind:checked={create.encrypt} onchange={toggleEncrypt} disabled={create.signer} />
      <span><strong>Encrypt Wallet</strong><small>Encrypt the wallet. The wallet will be encrypted with a passphrase of your choice.</small></span>
    </label>
    <details open={create.disableKeys || create.blank || create.signer}>
      <summary>Advanced Options</summary>
      <div class="stack inner">
        <label class="check option">
          <input type="checkbox" bind:checked={create.disableKeys} onchange={toggleDisableKeys} disabled={create.encrypt || create.blank || create.signer} />
          <span><strong>Disable Private Keys</strong><small>Disable private keys for this wallet. Wallets with private keys disabled will have no private keys and cannot have an HD seed or imported private keys. This is ideal for watch-only wallets.</small></span>
        </label>
        <label class="check option">
          <input type="checkbox" bind:checked={create.blank} onchange={toggleBlank} disabled={create.disableKeys || create.signer} />
          <span><strong>Make Blank Wallet</strong><small>Make a blank wallet. Blank wallets do not initially have private keys or scripts. Private keys and addresses can be imported, or an HD seed can be set, at a later time.</small></span>
        </label>
        <label class="check option" title={!create.supported ? 'Compiled without external signing support (required for external signing)' : create.signers.length === 0 ? 'No external signer found' : undefined}>
          <input type="checkbox" bind:checked={create.signer} onchange={toggleSigner} disabled={create.signers.length === 0 || create.encrypt} />
          <span><strong>External signer</strong><small>Use an external signing device such as a hardware wallet. Configure the external signer script in wallet preferences first.</small></span>
        </label>
        {#if create.signerError}<p class="error">{create.signerError}</p>{/if}
      </div>
    </details>
  </form>
  {#snippet actions()}
    <button type="button" class="btn" onclick={() => (create.open = false)}>Cancel</button>
    <button type="submit" form="create-form" class="btn primary" disabled={!create.name}>Create</button>
  {/snippet}
</Dialog>

<Dialog bind:open={unlockFor.open} title="Unlock wallet for a while">
  <form
    id="unlock-form"
    class="stack"
    onsubmit={(e) => {
      e.preventDefault();
      unlockTimed();
    }}
  >
    <p>The wallet stays unlocked for this time, then locks itself again. Anything that needs the keys meanwhile does not ask for the passphrase.</p>
    <div class="field">
      <label for="unlock-minutes">Minutes</label>
      <select id="unlock-minutes" bind:value={unlockFor.minutes}>
        {#each [1, 5, 15, 30, 60, 240] as m (m)}<option value={m}>{m} minute{m === 1 ? '' : 's'}</option>{/each}
      </select>
    </div>
  </form>
  {#snippet actions()}
    <button type="button" class="btn" onclick={() => (unlockFor.open = false)}>Cancel</button>
    <button type="submit" form="unlock-form" class="btn primary">Unlock…</button>
  {/snippet}
</Dialog>

<style>
  .wallets {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .grid {
    display: grid;
    grid-template-columns: minmax(0, 1.2fr) minmax(0, 1fr);
    gap: 1rem;
    align-items: start;
  }
  .badges {
    display: flex;
    flex-wrap: wrap;
    gap: 0.35rem;
  }
  .actions {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(15rem, 100%), 1fr));
    gap: 0.5rem;
  }
  .action {
    display: flex;
    gap: 0.7rem;
    align-items: flex-start;
    padding: 0.75rem 0.85rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .action:hover {
    border-color: var(--accent);
    background: var(--accent-soft);
  }
  .action span {
    display: flex;
    flex-direction: column;
    gap: 0.15rem;
  }
  .action small {
    color: var(--text-muted);
    font-size: 0.84rem;
  }
  .action.danger :global(svg) {
    color: var(--negative);
  }
  .list {
    margin: 0;
    list-style: none;
    display: flex;
    flex-direction: column;
  }
  .list li {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    padding: 0.5rem 0;
    border-bottom: 1px solid var(--line);
  }
  .list li:last-child {
    border-bottom: 0;
  }
  .list .badge {
    margin-left: 0.4rem;
  }
  .busy {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    padding: 0.7rem 1rem;
  }
  .spinner {
    width: 1rem;
    height: 1rem;
    border: 2px solid var(--line-strong);
    border-top-color: var(--accent);
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }
  @keyframes spin {
    to {
      transform: rotate(360deg);
    }
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.8rem;
  }
  .inner {
    margin-top: 0.7rem;
  }
  .option {
    align-items: flex-start;
  }
  .option span {
    display: flex;
    flex-direction: column;
    gap: 0.1rem;
  }
  .option small {
    color: var(--text-muted);
  }
  summary {
    cursor: pointer;
    font-weight: 600;
  }
  @media (max-width: 980px) {
    .grid {
      grid-template-columns: minmax(0, 1fr);
    }
  }
</style>
