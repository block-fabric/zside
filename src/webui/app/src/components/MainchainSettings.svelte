<script lang="ts">
  // The Mainchain tab of Settings on a sidechain: how the node reaches the node of its mainchain
  // (-mainchainrpc*): its address and RPC port, the wallet that pays for merged mining, and the
  // credentials: its cookie file, or a user name with a password file. Files are chosen in the file
  // chooser of the desktop, never typed. Like the other settings, they wait for Apply, and take effect
  // after a restart.
  import { t } from '../lib/i18n.svelte';
  import SafeText from './SafeText.svelte';
  import Icon from './Icon.svelte';
  import { CHAIN } from '../chain/config';
  import { errorText } from '../lib/dialogs.svelte';
  import { host, type NodeOptions, type NodeOptionValues } from '../lib/rpc';
  import { toast } from '../lib/state.svelte';

  let {
    options,
    set,
    changed,
  }: {
    options: NodeOptions;
    /** Set an option (the Settings page: it keeps the restart flag). */
    set: <K extends keyof NodeOptionValues>(name: K, value: NodeOptionValues[K]) => Promise<void>;
    /** The options after a file was chosen. */
    changed: (next: NodeOptions) => void;
  } = $props();

  const mainName = CHAIN.mainchain?.name ?? '';
  // Typed in: kept here, set when the field is left.
  let connect = $state('');
  let port = $state('');
  let walletName = $state('');
  let user = $state('');
  $effect(() => {
    connect = options.mainchainConnect ?? '';
    port = options.mainchainPort ?? '';
    walletName = options.mainchainWallet ?? '';
    user = options.mainchainUser ?? '';
  });
  let portError = $state('');

  const fixed = (key: string) => options.mainchainIgnored?.[key];

  async function setPort() {
    const text = port.trim();
    if (text && !(/^\d+$/.test(text) && Number(text) >= 1 && Number(text) <= 65535)) {
      portError = t('The port is a number from 1 to 65535.');
      return;
    }
    portError = '';
    await set('mainchainPort', text);
  }

  async function choose(name: 'mainchainrpccookiefile' | 'mainchainrpcpasswordfile') {
    try {
      const next = await host.chooseFileOption(name);
      if (next) changed(next);
    } catch (e) {
      toast('error', t('Settings'), errorText(e));
    }
  }
</script>

<section class="panel">
  <div class="panel-body form">
    <p class="hint">
      This node follows the {mainName} node below: it asks it for its blocks, which decide which blocks of this chain are valid, and its wallet pays for merged mining. The changes take effect after a restart.
    </p>

    <div class="field-row" title={fixed('mainchainConnect') !== undefined ? t('Set on the command line or in the configuration file.') : undefined}>
      <label for="opt-main-connect">Address of the {mainName} node</label>
      <input id="opt-main-connect" type="text" class="mono" spellcheck="false" autocomplete="off" placeholder={options.mainchainDefaults?.connect ?? '127.0.0.1'} bind:value={connect} disabled={fixed('mainchainConnect') !== undefined} onchange={() => set('mainchainConnect', connect.trim())} />
    </div>
    <div class="field-row" title={fixed('mainchainPort') !== undefined ? t('Set on the command line or in the configuration file.') : undefined}>
      <label for="opt-main-port">RPC port</label>
      <input id="opt-main-port" class="small-num" type="text" inputmode="numeric" placeholder={String(options.mainchainDefaults?.port ?? '')} bind:value={port} disabled={fixed('mainchainPort') !== undefined} onchange={setPort} aria-invalid={portError ? 'true' : undefined} />
      <span class="muted">empty for the default, {options.mainchainDefaults?.port ?? '–'}</span>
    </div>
    {#if portError}<p class="error" role="alert">{portError}</p>{/if}
    <div class="field-row">
      <label for="opt-main-wallet">Wallet that pays for merged mining</label>
      <input id="opt-main-wallet" type="text" spellcheck="false" autocomplete="off" placeholder="Its only wallet loaded" bind:value={walletName} disabled={fixed('mainchainWallet') !== undefined} onchange={() => set('mainchainWallet', walletName.trim())} />
    </div>

    <h3>Credentials</h3>
    <p class="hint">Either the cookie file of the {mainName} node (the default: .cookie in its data directory), or a user name with a file holding its password (owner-only, one line).</p>
    <div class="field">
      <label for="opt-main-cookie">Cookie file</label>
      <div class="row">
        <input id="opt-main-cookie" type="text" class="mono grow" readonly placeholder="The .cookie file of the default data directory of {mainName}" value={options.mainchainCookieFile ?? ''} disabled={fixed('mainchainCookieFile') !== undefined} />
        <button type="button" class="btn" disabled={fixed('mainchainCookieFile') !== undefined} onclick={() => choose('mainchainrpccookiefile')}>Choose…</button>
        <button type="button" class="btn ghost" disabled={fixed('mainchainCookieFile') !== undefined || !options.mainchainCookieFile} onclick={() => set('mainchainCookieFile', '')}>Clear</button>
      </div>
    </div>
    <div class="field-row">
      <label for="opt-main-user">RPC user name</label>
      <input id="opt-main-user" type="text" spellcheck="false" autocomplete="off" placeholder="None: the cookie file" bind:value={user} disabled={fixed('mainchainUser') !== undefined} onchange={() => set('mainchainUser', user.trim())} />
    </div>
    <div class="field">
      <label for="opt-main-password">Password file</label>
      <div class="row">
        <input id="opt-main-password" type="text" class="mono grow" readonly placeholder="No file chosen" value={options.mainchainPasswordFile ?? ''} disabled={fixed('mainchainPasswordFile') !== undefined} />
        <button type="button" class="btn" disabled={fixed('mainchainPasswordFile') !== undefined} onclick={() => choose('mainchainrpcpasswordfile')}>Choose…</button>
        <button type="button" class="btn ghost" disabled={fixed('mainchainPasswordFile') !== undefined || !options.mainchainPasswordFile} onclick={() => set('mainchainPasswordFile', '')}>Clear</button>
      </div>
      <span class="hint">The password itself is never written in the settings.</span>
    </div>
    {#if options.mainchainIgnored && Object.keys(options.mainchainIgnored).length}
      <p class="hint warn"><Icon name="alert" size={14} /> Set on the command line or in the configuration file: <SafeText text={Object.entries(options.mainchainIgnored).map(([k, v]) => `${k}=${v}`).join(', ')} /></p>
    {/if}
  </div>
</section>

<style>
  .form {
    display: flex;
    flex-direction: column;
    gap: 1.1rem;
    padding-top: 1.1rem;
    max-width: 56rem;
  }
  .field-row {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    flex-wrap: wrap;
  }
  .field-row label {
    min-width: 16rem;
  }
  .field-row input[type='text']:not(.small-num) {
    width: 18rem;
  }
  .small-num {
    width: 7rem;
  }
  h3 {
    margin: 0.4rem 0 0;
    font-size: 1rem;
  }
  .hint {
    margin: 0;
    color: var(--text-muted);
    font-size: 0.86rem;
  }
  .hint.warn {
    display: flex;
    align-items: center;
    gap: 0.35rem;
    color: var(--warning);
  }
  .error {
    margin: 0;
    color: var(--negative);
  }
</style>
