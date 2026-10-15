<script lang="ts">
  // The multisig lounge (MultisigDialog of chains-qt): collect the keys of partners, make shared
  // addresses that need several signatures, and spend from them by passing a PSBT around.
  import { t } from '../lib/i18n.svelte';
  import { CHAIN } from '../chain/config';
  import CopyButton from '../components/CopyButton.svelte';
  import Icon from '../components/Icon.svelte';
  import { confirm, errorText, inform, prompt, walletRpc } from '../lib/dialogs.svelte';
  import { ownTab, setOwnTab } from '../lib/nav.svelte';
  import { rpc } from '../lib/rpc';
  import { noWalletText, prefs, savePrefs, wallets } from '../lib/state.svelte';
  import { bareDescriptor, coinsToUnits, jsonCoinsToUnits, keyOfDescriptor, psbtStatus, sharedDescriptor, signersText, unitsToCoins } from '../lib/tools';
  import { walletDisplayName } from '../lib/wallet';

  // A tab of a page that holds it (Tools, Advanced transactions): without a title of its own.
  let { embedded = false }: { embedded?: boolean } = $props();

  const TABS = [
    { id: 'keys', label: 'Keys' },
    { id: 'addresses', label: 'Shared addresses' },
    { id: 'spend', label: 'Spend' },
  ];
  const tab = $derived(TABS.some((t) => t.id === ownTab('multisig')) ? ownTab('multisig') : 'keys');

  const warn = (text: string) => inform(t('Multisig Lounge'), text, 'warning');

  // --- Keys ---
  let keyName = $state('');
  let partnerKey = $state('');
  let selectedKey = $state<number | null>(null);

  async function addOwnKey() {
    const wallet = wallets.current;
    if (wallet === null) return warn(noWalletText());
    // The name as it is at the click: what is typed while the wallet answers is for the next key.
    const typed = keyName;
    const name = typed.trim() || `Me (${walletDisplayName(wallet)})`;
    try {
      const address = await rpc.call('getnewaddress', ['multisig key', 'bech32'], { wallet });
      const info = await rpc.raw<{ desc: string }>('getaddressinfo', [address], { wallet });
      // The key with the path it has in the wallet, which is what lets the wallet find it again to sign.
      const key = keyOfDescriptor(info.desc);
      if (!key) return warn(t('The wallet did not give a key that can be used here.'));
      savePrefs({ multisigKeys: [...prefs.multisigKeys, { name, value: key }] });
      if (keyName === typed) keyName = '';
    } catch (e) {
      return warn(errorText(e));
    }
  }

  async function addPartnerKey() {
    const name = keyName.trim();
    const key = partnerKey.trim();
    if (!name || !key) return warn(t('Enter the name of the partner and their public key.'));
    try {
      await rpc.raw('getdescriptorinfo', [`wpkh(${key})`]);
    } catch (e) {
      return warn(t('This is not a public key: %1', errorText(e)));
    }
    savePrefs({ multisigKeys: [...prefs.multisigKeys, { name, value: key }] });
    keyName = '';
    partnerKey = '';
  }

  function removeKey() {
    if (selectedKey === null) return;
    savePrefs({ multisigKeys: prefs.multisigKeys.filter((_, i) => i !== selectedKey) });
    selectedKey = null;
  }

  // --- Shared addresses ---
  let ticked = $state<Record<number, boolean>>({});
  let required = $state(2);
  let addressName = $state('');
  let addressOf = $state<Record<string, string>>({});
  let balances = $state<Record<string, string>>({});
  let selectedAddress = $state<number | null>(null);
  let scanning = $state(false);

  // The address of each descriptor (deriveaddresses).
  $effect(() => {
    for (const entry of prefs.multisigAddresses) {
      if (addressOf[entry.value] !== undefined) continue;
      rpc
        .raw<string[]>('deriveaddresses', [entry.value])
        .then((a) => (addressOf[entry.value] = a.length === 1 ? a[0] : ''))
        .catch(() => (addressOf[entry.value] = ''));
    }
  });

  async function addAddress(name: string, descriptor: string): Promise<boolean> {
    let checked: string;
    try {
      const info = await rpc.raw<{ checksum: string }>('getdescriptorinfo', [bareDescriptor(descriptor)]);
      checked = `${bareDescriptor(descriptor)}#${info.checksum}`;
    } catch (e) {
      await warn(errorText(e));
      return false;
    }
    const known = prefs.multisigAddresses.find((a) => a.value === checked);
    if (known) {
      await warn(t('This shared address is already in the list, as "%1".', known.name));
      return false;
    }
    savePrefs({ multisigAddresses: [...prefs.multisigAddresses, { name, value: checked }] });
    return true;
  }

  async function createAddress() {
    const keys = prefs.multisigKeys.filter((_, i) => ticked[i]).map((k) => k.value);
    if (keys.length < 2) return warn(t('Tick at least two keys. Add them on the Keys tab first.'));
    if (required > keys.length) return warn(t('More signatures are needed than there are keys.'));
    const name = addressName.trim();
    if (!name) return warn(t('Give the shared address a name.'));
    // The keys are sorted by the descriptor, so that all partners get the same address whatever their order.
    if (await addAddress(name, sharedDescriptor(required, keys))) {
      addressName = '';
      ticked = {};
    }
  }

  async function importAddress() {
    const descriptor = (await prompt(t('Multisig Lounge'), t('Descriptor of the shared address, as a partner copied it:')))?.trim();
    if (!descriptor) return;
    if (!descriptor.startsWith('wsh(sortedmulti(')) return warn(t('This is not the descriptor of a shared address made in this window.'));
    const name = (await prompt(t('Multisig Lounge'), t('Name for this shared address:')))?.trim();
    if (!name) return;
    await addAddress(name, descriptor);
  }

  async function removeAddress() {
    if (selectedAddress === null) return;
    const entry = prefs.multisigAddresses[selectedAddress];
    if (!entry) return;
    if (!(await confirm(t('Multisig Lounge'), t('Remove "%1" from the list? Coins on the address stay there, but spending them needs its descriptor; make sure it is kept somewhere.', entry.name), { yes: 'Remove', danger: true })))
      return;
    savePrefs({ multisigAddresses: prefs.multisigAddresses.filter((_, i) => i !== selectedAddress) });
    selectedAddress = null;
  }

  async function refreshBalances() {
    scanning = true;
    try {
      for (const entry of prefs.multisigAddresses) {
        try {
          const scan = await rpc.raw<{ total_amount: number }>('scantxoutset', ['start', [entry.value]]);
          balances[entry.value] = scan.total_amount.toFixed(8);
        } catch {
          balances[entry.value] = '?';
        }
      }
    } finally {
      scanning = false;
    }
  }

  // --- Spend ---
  let from = $state(0);
  let destination = $state('');
  let amount = $state('');
  let fee = $state('0.0001');
  let psbt = $state('');
  let status = $state('');
  let preparing = $state(false);

  async function prepare() {
    const entry = prefs.multisigAddresses[from];
    if (!entry) return warn(t('There is no shared address to spend from. Make one on the Shared addresses tab.'));
    const amountUnits = coinsToUnits(amount);
    const feeUnits = coinsToUnits(fee);
    if (amountUnits === null || feeUnits === null) return warn(t('Enter the amount and the fee as numbers, like 1.5'));
    preparing = true;
    try {
      const [scan, addresses] = await Promise.all([
        rpc.raw<{ unspents: { txid: string; vout: number; amount: number }[] }>('scantxoutset', ['start', [entry.value]]),
        rpc.raw<string[]>('deriveaddresses', [entry.value]),
      ]);
      // All coins of the address are spent, and what is left goes back to it, counted in satoshis.
      const inputs = scan.unspents.map((c) => ({ txid: c.txid, vout: c.vout }));
      const available = scan.unspents.reduce((sum, c) => sum + jsonCoinsToUnits(c.amount), 0n);
      if (inputs.length === 0) return warn(t('The shared address holds no coins.'));
      const change = available - amountUnits - feeUnits;
      if (change < 0n) return warn(t('The shared address holds %1 %2, which is less than the amount and the fee.', unitsToCoins(available), CHAIN.coin.ticker));
      const outputs: Record<string, string>[] = [{ [destination.trim()]: amount.trim() }];
      // Change too small to be worth an output goes to the fee.
      if (change >= 1000n) outputs.push({ [addresses[0]]: unitsToCoins(change) });
      const created = await rpc.raw<string>('createpsbt', [inputs, outputs]);
      // Adds what the signers need to know about the coins and about the keys.
      psbt = await rpc.raw<string>('utxoupdatepsbt', [created, [entry.value]]);
    } catch (e) {
      await warn(errorText(e));
    } finally {
      preparing = false;
    }
  }

  // How far the transaction is, whenever it changes.
  $effect(() => {
    const text = psbt.trim();
    if (!text) {
      status = '';
      return;
    }
    const t = setTimeout(async () => {
      try {
        status = psbtStatus(await rpc.raw('decodepsbt', [text]));
      } catch (e) {
        status = `This is not a transaction to sign: ${errorText(e)}`;
      }
    }, 150);
    return () => clearTimeout(t);
  });

  async function sign() {
    const wallet = wallets.current;
    if (wallet === null) return warn(t('Open the wallet that holds your key first.'));
    try {
      // Signs, without finalizing, so that the next partner can still add a signature.
      const r = await walletRpc<{ psbt: string }>(wallet, 'walletprocesspsbt', [psbt.trim(), true, 'ALL', true, false]);
      if (!r) return;
      const before = psbt.trim();
      psbt = r.psbt;
      if (before === r.psbt) await inform(t('Multisig Lounge'), t('This wallet added no signature: either it already signed, or it holds none of the keys of the shared address.'));
    } catch (e) {
      await warn(errorText(e));
    }
  }

  async function broadcast() {
    try {
      const final = await rpc.raw<{ complete: boolean; hex?: string }>('finalizepsbt', [psbt.trim()]);
      if (!final.complete) return warn(t('The transaction does not have enough signatures yet.'));
      if (!(await confirm(t('Multisig Lounge'), t('Send this transaction? %1', status), { yes: 'Send' }))) return;
      const txid = await rpc.raw<string>('sendrawtransaction', [final.hex]);
      psbt = '';
      await inform(t('Multisig Lounge'), t('Sent as transaction %1', txid));
    } catch (e) {
      await warn(errorText(e));
    }
  }
</script>

<div class="multisig" class:page={!embedded}>
  {#if !embedded}<header class="page-head"><h1>Multisig lounge</h1></header>{/if}
  <!-- Inside another page (Tools, Advanced transactions), its tabs are on that page's row. -->
  {#if !embedded}
  <div class="tabs" role="tablist" aria-label="Multisig lounge">
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (setOwnTab('multisig', tb.id))}>{t(tb.label)}</button>
    {/each}
  </div>
  {/if}

  {#if tab === 'keys'}
    <section class="panel">
      <div class="panel-body form">
        <p class="muted">
          Collect the public keys of the people who will share an address: one of your own wallet, which you send to your partners, and theirs, which they send to you. Public keys are safe to share.
        </p>
        <ul class="list" aria-label="Keys">
          {#each prefs.multisigKeys as key, i (i)}
            <li class:chosen={selectedKey === i}>
              <label class="pick"><input type="radio" name="key" checked={selectedKey === i} onchange={() => (selectedKey = i)} /> <strong translate="no">{key.name}</strong></label>
              <code class="break">{key.value}</code>
              <CopyButton text={key.value} label="Copy key" />
            </li>
          {:else}
            <li class="muted empty">No key yet.</li>
          {/each}
        </ul>
        <div class="row wrap">
          <input id="multisig-key-name" type="text" class="name" placeholder="Name" bind:value={keyName} aria-label="Name" />
          <input id="multisig-key" type="text" class="mono grow" placeholder="Public key of a partner, as they copied it from this window" bind:value={partnerKey} aria-label="Public key" />
          <button type="button" class="btn" onclick={addPartnerKey}>Add partner</button>
        </div>
        <div class="row">
          <button type="button" class="btn primary" disabled={wallets.current === null} onclick={addOwnKey}><Icon name="plus" size={15} /> Add a key of my wallet</button>
          <button type="button" class="btn" disabled={selectedKey === null} onclick={removeKey}>Remove</button>
        </div>
      </div>
    </section>
  {:else if tab === 'addresses'}
    <section class="panel">
      <div class="panel-head"><h2>New shared address</h2></div>
      <form class="panel-body create" onsubmit={(e) => { e.preventDefault(); createAddress(); }}>
        <fieldset class="signers">
          <legend class="label">Tick the keys that share the address</legend>
          {#each prefs.multisigKeys as key, i (i)}
            <label class="check"><input type="checkbox" bind:checked={ticked[i]} /> <span translate="no">{key.name}</span></label>
          {:else}
            <p class="muted">Add keys on the Keys tab first.</p>
          {/each}
        </fieldset>
        <div class="form">
          <div class="field">
            <label for="multisig-required">Signatures needed to spend</label>
            <input id="multisig-required" type="number" min="1" max="15" bind:value={required} />
          </div>
          <div class="field">
            <label for="multisig-address-name">Name</label>
            <input id="multisig-address-name" type="text" bind:value={addressName} />
          </div>
          <button type="submit" class="btn primary">Create address</button>
        </div>
      </form>
    </section>
    <p class="muted note">Every partner needs the same shared address in this window: send them its descriptor, which they add with "Add from descriptor". Pay to the address like to any other.</p>
    <section class="panel">
      <ul class="list addresses" aria-label="Shared addresses">
        {#each prefs.multisigAddresses as entry, i (entry.value)}
          <li class:chosen={selectedAddress === i}>
            <label class="pick"><input type="radio" name="address" checked={selectedAddress === i} onchange={() => (selectedAddress = i)} /> <strong translate="no">{entry.name}</strong></label>
            <span class="badge accent">{signersText(entry.value)}</span>
            <span class="balance num">{balances[entry.value] ?? '?'} {CHAIN.coin.ticker}</span>
            <code class="break">{addressOf[entry.value] ?? '…'}</code>
            <span class="copies">
              <CopyButton text={addressOf[entry.value] ?? ''} label="Copy address" />
              <CopyButton text={entry.value} label="Copy descriptor" />
            </span>
          </li>
        {:else}
          <li class="muted empty">No shared address yet.</li>
        {/each}
      </ul>
      <div class="panel-body row wrap">
        <button type="button" class="btn" onclick={importAddress}>Add from descriptor…</button>
        <button type="button" class="btn" disabled={scanning} onclick={refreshBalances}>{scanning ? 'Looking…' : 'Refresh balances'}</button>
        <button type="button" class="btn" disabled={selectedAddress === null} onclick={removeAddress}>Remove</button>
      </div>
    </section>
  {:else}
    <section class="panel">
      <div class="panel-head"><h2>1. One partner prepares the transaction</h2></div>
      <form class="panel-body spend" onsubmit={(e) => { e.preventDefault(); prepare(); }}>
        <div class="field">
          <label for="multisig-from">Spend from</label>
          <select id="multisig-from" bind:value={from}>
            {#each prefs.multisigAddresses as entry, i (entry.value)}<option value={i} translate="no">{entry.name} ({signersText(entry.value)})</option>{/each}
          </select>
        </div>
        <div class="field">
          <label for="multisig-destination">Pay to address</label>
          <input id="multisig-destination" type="text" class="mono" bind:value={destination} />
        </div>
        <div class="field">
          <label for="multisig-amount">Amount ({CHAIN.coin.ticker})</label>
          <input id="multisig-amount" type="text" class="num" placeholder="0.00000000" bind:value={amount} />
        </div>
        <div class="field">
          <label for="multisig-fee">Fee ({CHAIN.coin.ticker})</label>
          <input id="multisig-fee" type="text" class="num" bind:value={fee} />
        </div>
        <div class="row"><button type="submit" class="btn primary" disabled={preparing}>Prepare transaction</button></div>
      </form>
    </section>
    <section class="panel sign">
      <div class="panel-head"><h2>2. Each partner signs it and passes it on, until it has enough signatures</h2></div>
      <div class="panel-body form">
        <textarea id="multisig-psbt" class="mono" rows="6" placeholder="The transaction, as text to pass around (PSBT). Paste the one you received here." bind:value={psbt}></textarea>
        {#if status}<p id="multisig-status" class="status" class:done={status.startsWith('Fully signed')}>{status}</p>{/if}
        <div class="row">
          <button type="button" class="btn" disabled={!psbt.trim()} onclick={sign}>Sign with my wallet</button>
          <CopyButton text={psbt.trim()} label="Copy" small={false} />
          <span class="grow"></span>
          <button type="button" class="btn primary" disabled={!psbt.trim()} onclick={broadcast}>Send transaction</button>
        </div>
      </div>
    </section>
  {/if}
</div>

<style>
  .form {
    display: flex;
    flex-direction: column;
    gap: 0.9rem;
    padding-top: 1rem;
  }
  .wrap {
    flex-wrap: wrap;
  }
  .name {
    width: 12rem;
  }
  .list {
    margin: 0;
    padding: 0;
    list-style: none;
    border: 1px solid var(--line);
    border-radius: var(--radius-s);
  }
  .list li {
    display: flex;
    align-items: center;
    gap: 0.8rem;
    padding: 0.55rem 0.8rem;
    border-bottom: 1px solid var(--line);
    flex-wrap: wrap;
  }
  .list li:last-child {
    border-bottom: 0;
  }
  .list li.chosen {
    background: var(--accent-soft);
  }
  .list code {
    flex: 1;
    min-width: 14rem;
    font-size: 0.85rem;
  }
  .pick {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    cursor: pointer;
  }
  .empty {
    justify-content: center;
  }
  .addresses {
    border: 0;
  }
  .balance {
    font-weight: 600;
  }
  .copies {
    display: inline-flex;
    gap: 0.2rem;
  }
  .create {
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(16rem, 22rem);
    gap: 1.5rem;
  }
  .signers {
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
    margin: 0;
    padding: 0;
    border: 0;
  }
  .note {
    margin: 1rem 0 0.6rem;
  }
  .spend {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 0.9rem 1.2rem;
  }
  .sign {
    margin-top: 1rem;
  }
  .status {
    margin: 0;
    padding: 0.6rem 0.8rem;
    border-radius: var(--radius-s);
    background: var(--bg);
    max-width: none;
  }
  .status.done {
    color: var(--positive);
    font-weight: 600;
  }
</style>
