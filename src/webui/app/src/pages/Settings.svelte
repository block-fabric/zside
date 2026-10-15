<script lang="ts">
  // Settings (OptionsDialog and OptionsModel of chains-qt): the options of the node, kept in
  // settings.json as chains-qt keeps them; the window; the look. A change that only takes effect
  // after a restart says so, and the app can restart at once.
  import Icon from '../components/Icon.svelte';
  import { CHAIN } from '../chain/config';
  import { confirm, errorText, inform } from '../lib/dialogs.svelte';
  import { nav } from '../lib/nav.svelte';
  import { host, type NodeOptions, type NodeOptionValues } from '../lib/rpc';
  import { app, prefs, toast } from '../lib/state.svelte';
  import { applyChanges, askOnLeave, cancelChanges, draft, noteBaseline, pendingCount, shownTitleBar, stagePrefs, stageTitleBar } from '../lib/settingsdraft.svelte';
  import { loadLocale, t } from '../lib/i18n.svelte';

  const draftBaselineLang = () => draft.baseline?.lang ?? '';
  import { clampFontAdjust, FONTS, MAX_FONT_ADJUST, MIN_FONT_ADJUST, THEMES, type FontChoice, type ThemeId } from '../lib/themes';
  import { UNITS, type Unit } from '../lib/units';
  import LayoutPicker from '../components/LayoutPicker.svelte';
  import MainchainSettings from '../components/MainchainSettings.svelte';
  import { SIDECHAIN } from '../chain/config';
  import { frame } from '../lib/windowframe.svelte';

  // Appearance first: the look, the layout and the title bar (doc/webui-layouts.md).
  const TABS = [
    { id: 'display', label: 'Appearance' },
    { id: 'main', label: 'Main' },
    { id: 'wallet', label: 'Wallet' },
    { id: 'network', label: 'Network' },
    { id: 'window', label: 'Window' },
    // A sidechain: the connection to its mainchain node.
    ...(SIDECHAIN ? [{ id: 'mainchain', label: CHAIN.mainchain?.name ?? 'Mainchain' }] : []),
  ];
  const tab = $derived(TABS.some((t) => t.id === nav.tab) ? nav.tab : 'display');

  let options = $state<NodeOptions | null>(null);
  let loadError = $state('');
  let tray = $state<{ available: boolean } | null>(null);
  /** A change in this visit that needs a restart (the temporary message of OptionsDialog). */
  let changedNow = $state(false);

  async function load() {
    try {
      options = await host.options();
      noteBaseline(options);
      loadError = '';
    } catch (e) {
      loadError = errorText(e);
    }
  }
  $effect(() => {
    load();
    host.trayState().then((t) => (tray = t)).catch(() => (tray = { available: false }));
  });

  // Fields typed in: kept here until they are valid, then set.
  let proxyIp = $state('');
  let torIp = $state('');
  let proxyPort = $state('');
  let torPort = $state('');
  let signer = $state('');
  let thirdParty = $state(prefs.thirdPartyTxUrls);
  $effect(() => {
    if (!options) return;
    proxyIp = options.proxyIp;
    proxyPort = options.proxyPort;
    torIp = options.torIp;
    torPort = options.torPort;
    signer = options.signer;
  });

  async function chooseSigner() {
    if (!options) return;
    const before = options.restartRequired;
    try {
      const next = await host.chooseProgramOption('signer');
      if (!next) return;
      options = next;
      signer = next.signer;
      if (next.restartRequired && !before) changedNow = true;
      if (next.restartRequired) changedNow = true;
    } catch (e) {
      toast('error', t('Settings'), errorText(e));
      await load();
    }
  }

  async function set<K extends keyof NodeOptionValues>(name: K | 'proxyAddress' | 'torAddress', value: NodeOptionValues[K] | { ip: string; port: number }) {
    if (!options) return;
    const before = options.restartRequired;
    try {
      options = await host.setOption(name, value);
      // Declined in the confirmation of the desktop: the fields show what is still set.
      if (options.declined) {
        proxyIp = options.proxyIp;
        proxyPort = options.proxyPort;
        torIp = options.torIp;
        torPort = options.torPort;
        toast('info', t('Settings'), t('Not changed.'));
      }
      if (options.restartRequired && !before) changedNow = true;
      if (options.restartRequired) changedNow = true;
    } catch (e) {
      toast('error', t('Settings'), errorText(e));
      await load();
    }
  }

  // A switch whose change the desktop asks about (a proxy, the Tor proxy, the RPC server): declined, it
  // shows what is still set.
  async function setAsked(box: HTMLInputElement, name: 'server' | 'proxyUse' | 'torUse') {
    await set(name, box.checked);
    if (options) box.checked = options[name];
  }

  let proxyError = $state('');
  async function setProxy(tor: boolean) {
    const ip = (tor ? torIp : proxyIp).trim();
    const port = Number(tor ? torPort : proxyPort);
    if (!(await host.validProxy(ip).catch(() => false)) || !Number.isInteger(port) || port < 1 || port > 65535) {
      proxyError = 'The supplied proxy address is invalid.';
      return;
    }
    proxyError = '';
    // The address and the port at once: one confirmation of the desktop when the proxy is in use.
    await set(tor ? 'torAddress' : 'proxyAddress', { ip, port });
  }

  const ignored = (setting: string) => !!options?.ignored[setting];
  const overriddenTip = 'Set on the command line or in chains.conf: that value is used, and this one is kept for when it is not.';

  async function openConfig() {
    const r = await confirm(
      t('Configuration options'),
      t('The configuration file is used to specify advanced user options which override GUI settings. Additionally, any command-line options will override this configuration file.'),
      { yes: 'Continue', level: 'info' },
    );
    if (!r) return;
    try {
      await host.open('config');
    } catch (e) {
      await inform(t('Error'), errorText(e) || t('The configuration file could not be opened.'), 'error');
    }
  }

  async function reset() {
    const ok = await confirm(
      t('Confirm options reset'),
      t('Client restart required to activate changes.\n\nCurrent settings will be backed up at "%1".\n\nClient will be shut down. Do you want to proceed?', app.info?.datadir ?? ''),
      { yes: 'Reset and shut down', danger: true, level: 'warning' },
    );
    if (!ok) return;
    try {
      await host.resetOptions();
    } catch (e) {
      await inform(t('Error'), errorText(e), 'error');
    }
  }

  async function restart() {
    if (!(await confirm(t('Restart'), t('%1 will stop and start again with the new settings. Restart now?', app.info?.name ?? CHAIN.name), { yes: 'Restart' }))) return;
    if (pending > 0 && (await applyChanges(options)) === null) return;
    try {
      await host.restart();
    } catch (e) {
      await inform(t('Restart'), errorText(e), 'warning');
    }
  }

  function saveThirdParty() {
    if (thirdParty.trim() === prefs.thirdPartyTxUrls) return;
    stagePrefs({ thirdPartyTxUrls: thirdParty.trim() });
  }

  // Apply and Cancel (the OK and Cancel of the options dialog): nothing takes effect before Apply.
  const pending = $derived(pendingCount(options));
  let applying = $state(false);
  // The languages there are catalogs for (app.languages), by their own names.
  let languages = $state<{ code: string; name: string }[]>([]);
  $effect(() => {
    host
      .languages()
      .then((list) => (languages = [...list].sort((a, b) => a.name.localeCompare(b.name))))
      .catch(() => (languages = []));
  });

  async function apply() {
    applying = true;
    try {
      const before = options?.restartRequired ?? false;
      const langBefore = draftBaselineLang();
      const after = await applyChanges(options);
      if (after) {
        if (after.lang !== langBefore) {
          // The interface and the node follow the language at once.
          await host.updateLanguage().catch(() => undefined);
          await loadLocale();
        }
        options = after;
        if (after.restartRequired && !before) changedNow = true;
        toast('success', t('Settings'), t('The changes are applied.'));
      }
    } finally {
      applying = false;
    }
  }
  async function cancel() {
    applying = true;
    try {
      options = await cancelChanges(options);
      thirdParty = prefs.thirdPartyTxUrls;
      changedNow = false;
      await load();
    } finally {
      applying = false;
    }
  }
  // Leaving the page with changes waiting: apply them, or throw them away (asked once the page is gone).
  $effect(() => () => {
    const last = options;
    if (pendingCount(last) > 0) queueMicrotask(() => askOnLeave(last));
  });
  // What waits is known outside the page too (the sidebar marks Settings).
  $effect(() => {
    noteBaseline(options);
  });
</script>

<div class="page">
  <header class="page-head"><h1>Settings</h1></header>
  <div class="tabs" role="tablist" aria-label="Settings">
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>{t(tb.label)}</button>
    {/each}
  </div>

  {#if options?.restartRequired}
    <div class="restart" role="status">
      <Icon name="alert" size={16} />
      <span>{changedNow ? 'This change would require a client restart.' : 'Client restart required to activate changes.'}</span>
      {#if !app.info?.test}<button type="button" class="btn small" onclick={restart}>Restart now</button>{/if}
    </div>
  {/if}
  {#if loadError}<p class="error">{loadError}</p>{/if}

  {#if tab === 'mainchain' && options}
    <MainchainSettings {options} {set} changed={(next) => { options = next; changedNow = true; }} />
  {:else if tab === 'main' && options}
    <section class="panel">
      <div class="panel-body form">
        {#if app.info?.platform !== 'macos'}
          <label class="check">
            <input type="checkbox" checked={options.startAtLogin} onchange={(e) => set('startAtLogin', (e.currentTarget as HTMLInputElement).checked)} />
            <span>Start {app.info?.name ?? CHAIN.name} on system login</span>
          </label>
          <p class="hint">Automatically start {app.info?.name ?? CHAIN.name} after logging in to the system.</p>
        {/if}

        <div class="field-row" title={ignored('prune') ? overriddenTip : undefined}>
          <label class="check">
            <input id="opt-prune" type="checkbox" checked={options.prune} disabled={ignored('prune')} onchange={(e) => set('prune', (e.currentTarget as HTMLInputElement).checked)} />
            <span>Prune block storage to</span>
          </label>
          <input id="opt-prune-size" class="small-num" type="number" min={options.limits.pruneMinGB} value={options.pruneSize} disabled={!options.prune || ignored('prune')} onchange={(e) => set('pruneSize', Number((e.currentTarget as HTMLInputElement).value))} aria-label="Prune size" />
          <span>GB</span>
        </div>
        {#if options.prune}<p class="hint warn">Reverting this setting requires re-downloading the entire blockchain.</p>{/if}
        <p class="hint">Disables some advanced features but all blocks will still be fully validated. Reverting this setting requires re-downloading the entire blockchain. Actual disk usage may be somewhat higher.</p>

        <div class="field-row" title={ignored('dbcache') ? overriddenTip : undefined}>
          <label for="opt-dbcache">Size of database cache</label>
          <input id="opt-dbcache" class="small-num" type="number" min={options.limits.dbcacheMin} value={options.dbcache} disabled={ignored('dbcache')} onchange={(e) => set('dbcache', Number((e.currentTarget as HTMLInputElement).value))} />
          <span>MiB</span>
        </div>
        <p class="hint">Maximum database cache size. A larger cache can contribute to faster sync, after which the benefit is less pronounced for most use cases. Lowering the cache size will reduce memory usage. Unused mempool memory is shared for this cache.</p>

        <div class="field-row" title={ignored('par') ? overriddenTip : undefined}>
          <label for="opt-par">Number of script verification threads</label>
          <input id="opt-par" class="small-num" type="number" min={options.limits.parMin} max={options.limits.parMax} value={options.par} disabled={ignored('par')} onchange={(e) => set('par', Number((e.currentTarget as HTMLInputElement).value))} />
          <span class="muted">(0 = auto, &lt;0 = leave that many cores free)</span>
        </div>

        <label class="check" title={ignored('server') ? overriddenTip : undefined}>
          <input id="opt-server" type="checkbox" checked={options.server} disabled={ignored('server')} onchange={(e) => setAsked(e.currentTarget as HTMLInputElement, 'server')} />
          <span>Enable RPC server</span>
        </label>
        <p class="hint">Enabling the RPC server allows you to control the node and the wallet with command-line tools or JSON-RPC requests. Sidechain nodes run from here need it to follow this node.</p>

        <div class="field">
          <span class="label">Options set in this dialog are overridden by the command line or in the configuration file:</span>
          <code id="opt-overridden" class="overridden">{options.overridden.trim() || 'none'}</code>
        </div>
        <div class="row">
          <button type="button" class="btn" onclick={openConfig}><Icon name="text" size={15} /> Open Configuration File</button>
          <span class="grow"></span>
          <button type="button" class="btn danger" onclick={reset}>Reset Options</button>
        </div>
      </div>
    </section>
  {:else if tab === 'wallet' && options}
    <section class="panel">
      <div class="panel-body form">
        <label class="check">
          <input type="checkbox" checked={prefs.coinControl} onchange={(e) => stagePrefs({ coinControl: (e.currentTarget as HTMLInputElement).checked })} />
          <span>Enable coin control features</span>
        </label>
        <p class="hint">"Choose coins" turned on in the Send page (it is there to turn on and off too): the coins of the wallet in the page, to choose the ones a transaction spends, and a custom change address.</p>
        <label class="check" title={ignored('spendzeroconfchange') ? overriddenTip : undefined}>
          <input type="checkbox" checked={options.spendZeroConf} disabled={ignored('spendzeroconfchange')} onchange={(e) => set('spendZeroConf', (e.currentTarget as HTMLInputElement).checked)} />
          <span>Spend unconfirmed change</span>
        </label>
        <p class="hint">If you disable the spending of unconfirmed change, the change from a transaction cannot be used until that transaction has at least one confirmation. This also affects how your balance is computed.</p>
        <label class="check">
          <input type="checkbox" checked={prefs.psbtControls} onchange={(e) => stagePrefs({ psbtControls: (e.currentTarget as HTMLInputElement).checked })} />
          <span>Enable PSBT controls</span>
        </label>
        <p class="hint">Whether to show PSBT controls (create unsigned transactions).</p>
        <label class="check">
          <input type="checkbox" checked={prefs.subtractFeeDefault} onchange={(e) => stagePrefs({ subtractFeeDefault: (e.currentTarget as HTMLInputElement).checked })} />
          <span>Subtract fee from amount by default</span>
        </label>
        <div class="field">
          <label for="signer-path">External signer script path</label>
          <!-- A program the node runs: chosen in the file chooser of the desktop and confirmed there, never typed here. -->
          <div class="row">
            <input id="signer-path" type="text" class="mono" readonly placeholder="No HWI compatible program chosen" disabled={!options.externalSigner || ignored('signer')} value={signer} />
            <button type="button" class="btn" disabled={!options.externalSigner || ignored('signer')} onclick={chooseSigner}>Choose…</button>
            <button type="button" class="btn ghost" disabled={!options.externalSigner || ignored('signer') || !signer} onclick={async () => { await set('signer', ''); signer = options?.signer ?? ''; }}>Clear</button>
          </div>
          <span class="hint">{options.externalSigner ? 'Full path to a Bitcoin Core compatible script (e.g. C:\\Downloads\\hwi.exe or /Users/you/Downloads/hwi.py). Beware: malware can steal your coins!' : 'Compiled without external signing support (required for external signing)'}</span>
        </div>
      </div>
    </section>
  {:else if tab === 'network' && options}
    <section class="panel">
      <div class="panel-body form">
        <label class="check" title={ignored('natpmp') ? overriddenTip : undefined}>
          <input id="opt-natpmp" type="checkbox" checked={options.natpmp} disabled={ignored('natpmp')} onchange={(e) => set('natpmp', (e.currentTarget as HTMLInputElement).checked)} />
          <span>Map port using PCP or NAT-PMP</span>
        </label>
        <p class="hint">Automatically open the client port on the router. This only works when your router supports PCP or NAT-PMP and it is enabled. The external port could be random.</p>
        <label class="check" title={ignored('listen') ? overriddenTip : undefined}>
          <input id="opt-listen" type="checkbox" checked={options.listen} disabled={ignored('listen')} onchange={(e) => set('listen', (e.currentTarget as HTMLInputElement).checked)} />
          <span>Allow incoming connections</span>
        </label>
        <p class="hint">Accept connections from outside.</p>

        <fieldset class="proxy" title={ignored('proxy') ? overriddenTip : undefined}>
          <label class="check">
            <input id="opt-proxy" type="checkbox" checked={options.proxyUse} disabled={ignored('proxy')} onchange={(e) => setAsked(e.currentTarget as HTMLInputElement, 'proxyUse')} />
            <span>Connect through SOCKS5 proxy (default proxy)</span>
          </label>
          <div class="field-row">
            <label for="opt-proxy-ip">Proxy IP</label>
            <input id="opt-proxy-ip" type="text" class="mono" disabled={!options.proxyUse || ignored('proxy')} bind:value={proxyIp} onchange={() => setProxy(false)} />
            <label for="opt-proxy-port">Port</label>
            <input id="opt-proxy-port" type="text" class="small-num mono" disabled={!options.proxyUse || ignored('proxy')} bind:value={proxyPort} onchange={() => setProxy(false)} />
          </div>
          <div class="reach">
            <span class="muted">Used for reaching peers via:</span>
            <span class="badge" class:accent={options.proxyReach.ipv4}>IPv4</span>
            <span class="badge" class:accent={options.proxyReach.ipv6}>IPv6</span>
            <span class="badge" class:accent={options.proxyReach.tor}>Tor</span>
          </div>
        </fieldset>
        <fieldset class="proxy" title={ignored('onion') ? overriddenTip : undefined}>
          <label class="check">
            <input id="opt-tor" type="checkbox" checked={options.torUse} disabled={ignored('onion')} onchange={(e) => setAsked(e.currentTarget as HTMLInputElement, 'torUse')} />
            <span>Use separate SOCKS5 proxy to reach peers via Tor onion services</span>
          </label>
          <div class="field-row">
            <label for="opt-tor-ip">Proxy IP</label>
            <input id="opt-tor-ip" type="text" class="mono" disabled={!options.torUse || ignored('onion')} bind:value={torIp} onchange={() => setProxy(true)} />
            <label for="opt-tor-port">Port</label>
            <input id="opt-tor-port" type="text" class="small-num mono" disabled={!options.torUse || ignored('onion')} bind:value={torPort} onchange={() => setProxy(true)} />
          </div>
        </fieldset>
        {#if proxyError}<p class="error" role="alert">{proxyError}</p>{/if}
      </div>
    </section>
  {:else if tab === 'window'}
    <section class="panel">
      <div class="panel-body form">
        {#if tray && !tray.available}<p class="hint warn">This desktop shows no tray icon now: the options of the tray take effect where it has one.</p>{/if}
        <label class="check">
          <input id="opt-show-tray" type="checkbox" checked={prefs.showTray} onchange={(e) => { const on = (e.currentTarget as HTMLInputElement).checked; stagePrefs(on ? { showTray: true } : { showTray: false, minimizeToTray: false }); }} />
          <span>Show tray icon</span>
        </label>
        <p class="hint">Show an icon in the tray, with a menu to show the window, send, receive, sign, the options and the node window.</p>
        <label class="check">
          <input id="opt-minimize-tray" type="checkbox" checked={prefs.minimizeToTray} disabled={!prefs.showTray} onchange={(e) => stagePrefs({ minimizeToTray: (e.currentTarget as HTMLInputElement).checked })} />
          <span>Minimize to the tray instead of the taskbar</span>
        </label>
        <p class="hint">Show only a tray icon after minimizing the window.</p>
        <label class="check">
          <input id="opt-minimize-close" type="checkbox" checked={prefs.minimizeOnClose} onchange={(e) => stagePrefs({ minimizeOnClose: (e.currentTarget as HTMLInputElement).checked })} />
          <span>Minimize on close</span>
        </label>
        <p class="hint">Minimize instead of exit the application when the window is closed. When this option is enabled, the application will be closed only after selecting Exit in the menu.</p>
      </div>
    </section>
  {:else if tab === 'display'}
    <section class="panel">
      <div class="panel-body form">
        <LayoutPicker staged />
        <fieldset class="field">
          <legend class="label">Theme</legend>
          <div class="themes">
            {#each THEMES as theme (theme.id)}
              <label class="theme" class:chosen={prefs.theme === theme.id} data-swatch={theme.id}>
                <input type="radio" name="theme" class="sr-only" checked={prefs.theme === theme.id} onchange={() => stagePrefs({ theme: theme.id as ThemeId })} />
                <span class="swatch" aria-hidden="true"><span></span><span></span><span></span></span>
                <span>{t(theme.name)}</span>
              </label>
            {/each}
          </div>
        </fieldset>

        <div class="cols">
          <div class="field">
            <label for="opt-font">Font</label>
            <select id="opt-font" value={prefs.fontFamily} onchange={(e) => stagePrefs({ fontFamily: (e.currentTarget as HTMLSelectElement).value as FontChoice })}>
              {#each FONTS as f (f.id)}<option value={f.id} translate="no">{f.name}</option>{/each}
            </select>
            <span class="hint">Any font goes with any colours.</span>
          </div>
          <div class="field">
            <label for="font-adjust">Text size</label>
            <div class="row">
              <input id="font-adjust" type="range" min={MIN_FONT_ADJUST} max={MAX_FONT_ADJUST} step="1" value={prefs.fontAdjust} oninput={(e) => stagePrefs({ fontAdjust: clampFontAdjust(Number((e.currentTarget as HTMLInputElement).value)) })} />
              <span class="num">{prefs.fontAdjust > 0 ? '+' : ''}{prefs.fontAdjust} pt</span>
            </div>
            <span class="hint">How much larger, or smaller, than the default. Also Ctrl+ and Ctrl− anywhere.</span>
          </div>
        </div>

        <div class="cols">
          <div class="field">
            <label for="unit">Unit to show amounts in</label>
            <select id="unit" value={prefs.unit} onchange={(e) => stagePrefs({ unit: (e.currentTarget as HTMLSelectElement).value as Unit })}>
              {#each UNITS as u (u.id)}<option value={u.id}>{u.short} — {u.long}</option>{/each}
            </select>
            <span class="hint">The default subdivision unit to show in the interface and when sending coins.</span>
          </div>
          <div class="field">
            <label for="money-font">Font in the Overview tab and the amounts</label>
            <select id="money-font" value={prefs.moneyFont} onchange={(e) => stagePrefs({ moneyFont: (e.currentTarget as HTMLSelectElement).value as 'embedded' | 'system' })}>
              <option value="embedded">Embedded (the font of the theme)</option>
              <option value="system">Default system monospaced font</option>
            </select>
            <span class="hint">Sample: <span class="num sample">21 000 000.00000000 {CHAIN.coin.ticker}</span></span>
          </div>
        </div>

        <div class="field">
          <label for="opt-lang">User Interface language</label>
          <select id="opt-lang" value={options?.lang ?? ''} disabled={!options || ignored('lang')} onchange={(e) => set('lang', (e.currentTarget as HTMLSelectElement).value)}>
            <option value="">(default)</option>
            <option value="en" translate="no">English (en)</option>
            {#each languages as l (l.code)}
              {#if l.code !== 'en'}<option value={l.code} translate="no">{l.name} ({l.code})</option>{/if}
            {/each}
          </select>
          <span class="hint">The user interface language can be set here. It takes effect once the changes are applied, for the messages of the node too. The translations are the ones of chains-qt: what is new in this interface shows in English until it is translated.</span>
        </div>

        {#if frame.supported}
          <label class="check">
            <!-- The title bar of the desktop, or the integrated one (doc/webui-layouts.md): waits for Apply, as the other settings. -->
            <input type="checkbox" checked={shownTitleBar() === 'system'} onchange={(e) => stageTitleBar((e.currentTarget as HTMLInputElement).checked ? 'system' : 'integrated')} />
            <span>Use the title bar of the desktop</span>
          </label>
          <span class="hint" role="status">
            {#if frame.restartNeeded}
              Applies after a restart of {CHAIN.name}.
            {:else}
              Instead of the title bar of {CHAIN.name}, with its own minimize, maximize and close buttons.
            {/if}
          </span>
        {/if}


        {#if app.info?.walletEnabled}
          <div class="field">
            <label for="opt-third-party">Third-party transaction URLs</label>
            <input id="opt-third-party" type="text" placeholder="The address of a block explorer, with %s for the transaction id" bind:value={thirdParty} onchange={saveThirdParty} />
            <span class="hint">Third-party URLs (e.g. a block explorer) that appear in the transactions tab as context menu items. %s in the URL is replaced by transaction hash. Multiple URLs are separated by vertical bar |.</span>
          </div>
        {/if}

        <label class="check">
          <input type="checkbox" checked={prefs.mask} onchange={(e) => stagePrefs({ mask: (e.currentTarget as HTMLInputElement).checked })} />
          <span>Mask values (privacy mode)</span>
        </label>
      </div>
    </section>
  {/if}

  <div class="apply-bar" class:waiting={pending > 0} role="group" aria-label="Apply the settings">
    <span class="grow" role="status">
      {#if pending > 0}
        <Icon name="alert" size={15} />
        {pending === 1 ? '1 change is' : `${pending} changes are`} not applied yet.
      {:else}
        <span class="muted">Changes wait here until you apply them.</span>
      {/if}
    </span>
    <button type="button" class="btn" disabled={pending === 0 || applying} onclick={cancel}>Cancel</button>
    <button type="button" class="btn primary" disabled={pending === 0 || applying} onclick={apply}>Apply</button>
  </div>
</div>

<style>
  .apply-bar {
    position: sticky;
    bottom: 0;
    z-index: 2;
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.6rem;
    margin-top: 1rem;
    padding: 0.7rem 1rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface);
    box-shadow: var(--shadow);
  }
  .apply-bar.waiting {
    border-color: var(--warning);
  }
  .apply-bar .grow {
    display: flex;
    align-items: center;
    gap: 0.4rem;
  }
  .form {
    display: flex;
    flex-direction: column;
    gap: 1.1rem;
    padding-top: 1.1rem;
    max-width: 56rem;
  }
  fieldset {
    margin: 0;
    padding: 0;
    border: 0;
  }
  .field-row {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    flex-wrap: wrap;
  }
  .small-num {
    width: 7rem;
  }
  .overridden {
    padding: 0.4rem 0.6rem;
    border-radius: var(--radius-s);
    background: var(--bg);
  }
  .proxy {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    padding: 0.8rem 1rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
  }
  .proxy .field-row input[type='text']:not(.small-num) {
    width: 16rem;
  }
  .reach {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    font-size: 0.9rem;
  }
  .cols {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(18rem, 100%), 1fr));
    gap: 1.1rem 1.5rem;
  }
  .themes {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(11rem, 100%), 1fr));
    gap: 0.6rem;
  }
  .theme {
    display: flex;
    flex-direction: column;
    gap: 0.45rem;
    padding: 0.6rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    cursor: pointer;
  }
  .theme:has(input:focus-visible) {
    outline: 2px solid var(--focus);
    outline-offset: 2px;
  }
  .theme.chosen {
    border-color: var(--accent);
    box-shadow: inset 0 0 0 1px var(--accent);
  }
  .swatch {
    display: flex;
    height: 2.4rem;
    border-radius: var(--radius-s);
    overflow: hidden;
    border: 1px solid var(--line);
  }
  .swatch span {
    flex: 1;
  }
  /* The colours of each look, shown whatever the look of the app. */
  [data-swatch='system'] .swatch span:nth-child(1) { background: #eef1f4; }
  [data-swatch='system'] .swatch span:nth-child(2) { background: #1b1e22; }
  [data-swatch='system'] .swatch span:nth-child(3) { background: #5aa6ff; }
  [data-swatch='midnight'] .swatch span:nth-child(1) { background: #111413; }
  [data-swatch='midnight'] .swatch span:nth-child(2) { background: #272d29; }
  [data-swatch='midnight'] .swatch span:nth-child(3) { background: #b5f27e; }
  [data-swatch='light'] .swatch span:nth-child(1) { background: #eef1f4; }
  [data-swatch='light'] .swatch span:nth-child(2) { background: #ffffff; }
  [data-swatch='light'] .swatch span:nth-child(3) { background: #087a52; }
  [data-swatch='dark'] .swatch span:nth-child(1) { background: #141619; }
  [data-swatch='dark'] .swatch span:nth-child(2) { background: #292d33; }
  [data-swatch='dark'] .swatch span:nth-child(3) { background: #5aa6ff; }
  [data-swatch='ocean'] .swatch span:nth-child(1) { background: #04202a; }
  [data-swatch='ocean'] .swatch span:nth-child(2) { background: #123f4e; }
  [data-swatch='ocean'] .swatch span:nth-child(3) { background: #2ec4b6; }
  [data-swatch='matrix'] .swatch span:nth-child(1) { background: #000000; }
  [data-swatch='matrix'] .swatch span:nth-child(2) { background: #061e0c; }
  [data-swatch='matrix'] .swatch span:nth-child(3) { background: #00ff41; }
  [data-swatch='qdarkstyle'] .swatch span:nth-child(1) { background: #19232d; }
  [data-swatch='qdarkstyle'] .swatch span:nth-child(2) { background: #32414b; }
  [data-swatch='qdarkstyle'] .swatch span:nth-child(3) { background: #1464a0; }
  input[type='range'] {
    width: 18rem;
    accent-color: var(--accent);
  }
  .hint {
    margin: -0.8rem 0 0 1.6rem;
    color: var(--text-muted);
    font-size: 0.86rem;
  }
  .field .hint {
    margin: 0;
  }
  .hint.warn,
  .warn {
    color: var(--warning);
  }
  .sample {
    font-weight: 600;
  }
  .restart {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    margin-bottom: 1rem;
    padding: 0.6rem 0.9rem;
    border-radius: var(--radius-s);
    background: var(--warning-soft);
    color: var(--warning);
    font-weight: 600;
  }
  .error {
    color: var(--negative);
  }
</style>
