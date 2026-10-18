<script lang="ts">
  // An address in the block explorer. The explorer keeps no record of addresses and scans nothing for them: an address
  // of the current wallet is shown from what the wallet knows (host wallet.address: its label, what it received and
  // spent, the coins it holds now, its transactions, a page at a time); any other address only from itself (its kind,
  // its script, from validateaddress). Each transaction and block links to its view.
  import Amount from './Amount.svelte';
  import AddressLink from './AddressLink.svelte';
  import CopyButton from './CopyButton.svelte';
  import Pager from './Pager.svelte';
  import SafeText from './SafeText.svelte';
  import { scriptKindText, type AddressActivity, type ScriptInfo } from '../lib/explorer';
  import { formatCount, formatTime, shortHash } from '../lib/format';
  import { t } from '../lib/i18n.svelte';
  import { prefs, savePrefs, wallets } from '../lib/state.svelte';
  import { simpleMode } from '../lib/uimode.svelte';
  import { walletApi } from '../lib/wallet';
  import { untrack, type Snippet } from 'svelte';

  let {
    address,
    info,
    ontx,
    onblock,
    actions,
  }: {
    address: string;
    info: ScriptInfo;
    /** Open a transaction (with the height of its block if known: -1 when not in one). */
    ontx: (txid: string, height: number) => void;
    onblock: (height: number) => void;
    /** The buttons of its head (Back). */
    actions?: Snippet;
  } = $props();

  const SIZES = [10, 25, 50, 100];
  const simple = $derived(simpleMode());
  let activity = $state<AddressActivity | null>(null);
  let loading = $state(false);
  let failed = $state(false);
  let historyPage = $state(0);
  let coinsPage = $state(0);
  // Another address: from its first pages.
  $effect(() => {
    void address;
    historyPage = 0;
    coinsPage = 0;
  });
  // Read again with each transaction of the wallet, another wallet, another page.
  let asked = 0;
  $effect(() => {
    const wallet = wallets.current;
    void wallets.txVersion;
    const page = { skip: historyPage * prefs.explorerPageSize, count: prefs.explorerPageSize, coinSkip: coinsPage * prefs.explorerPageSize, coinCount: prefs.explorerPageSize };
    const a = address;
    untrack(() => load(wallet, a, page));
  });
  async function load(wallet: string | null, a: string, page: { skip: number; count: number; coinSkip: number; coinCount: number }) {
    const mine = ++asked;
    if (wallet === null) {
      activity = null;
      failed = false;
      return;
    }
    loading = true;
    try {
      const r = await walletApi(wallet).address(a, page);
      if (mine === asked) {
        activity = r;
        failed = false;
      }
    } catch {
      if (mine === asked) {
        activity = null;
        failed = true;
      }
    } finally {
      if (mine === asked) loading = false;
    }
  }
  const ours = $derived(activity?.mine === true);
</script>

<section class="panel detail address" aria-label="Address">
  <div class="panel-head">
    <h2 id="explorer-title">Address</h2>
    <div class="row">{@render actions?.()}</div>
  </div>
  <div class="panel-body">
    <dl class="facts">
      <dt>Address</dt><dd class="addr-line"><AddressLink {address} /> <CopyButton text={address} label="Copy the address" /></dd>
      <dt>Kind</dt><dd>{scriptKindText(info)}</dd>
      {#if !simple && info.scriptPubKey}<dt>Script</dt><dd class="mono break faint">{info.scriptPubKey}</dd>{/if}
      {#if ours && activity}
        {#if activity.label}<dt>Label</dt><dd><SafeText text={activity.label} /></dd>{/if}
        <dt>Holds now</dt><dd><Amount sats={activity.balance} />{#if activity.immature}<span class="faint"> · <Amount sats={activity.immature} /> {t('maturing')}</span>{/if}</dd>
        <dt>Received</dt><dd><Amount sats={activity.received} /></dd>
        {#if !simple}<dt>Spent</dt><dd><Amount sats={activity.spent} /></dd>{/if}
        <dt>Transactions</dt><dd class="num">{formatCount(activity.txCount ?? 0)}</dd>
        {#if !simple}<dt>Coins</dt><dd class="num">{formatCount(activity.coinCount ?? 0)}</dd>{/if}
      {/if}
    </dl>
    {#if loading && !activity}
      <p class="faint">Reading the wallet…</p>
    {:else if failed}
      <p class="faint">The wallet could not be read.</p>
    {:else if !ours}
      <p class="muted not-ours">{wallets.current === null ? t('No wallet is open. The explorer keeps no record of addresses: it shows the transactions only of those of the open wallet.') : t('Not an address of this wallet. The explorer keeps no record of other addresses.')}</p>
    {/if}

    {#if ours && activity}
      <div class="lists" class:one={simple}>
        <div>
          <div class="list-head">
            <h3>{t('Transactions')} ({formatCount(activity.txCount ?? 0)})</h3>
            <Pager total={activity.txCount ?? 0} bind:page={historyPage} size={prefs.explorerPageSize} sizes={SIZES} label="transactions" onsize={(n) => { savePrefs({ explorerPageSize: n }); historyPage = 0; coinsPage = 0; }} />
          </div>
          <table class="grid" aria-label="Transactions of the address">
            <thead><tr><th>Block</th><th>Time</th><th>Transaction</th><th class="end">Amount</th></tr></thead>
            <tbody>
              {#each activity.history ?? [] as [txid, height, time, received, spent] (txid)}
                <tr>
                  <td class="num">{#if height >= 0}<button type="button" class="mono linkish" title={t('Show block %1', formatCount(height))} onclick={() => onblock(height)}>{formatCount(height)}</button>{:else}<span class="faint">waiting</span>{/if}</td>
                  <td>{formatTime(time, true)}</td>
                  <td><button type="button" class="mono linkish" title={txid} onclick={() => ontx(txid, height)}>{shortHash(txid, 10)}</button></td>
                  <td class="end"><Amount sats={received - spent} plus colored /></td>
                </tr>
              {:else}
                <tr><td colspan="4" class="faint">None</td></tr>
              {/each}
            </tbody>
          </table>
        </div>
        {#if !simple}
          <div>
            <div class="list-head">
              <h3>{t('Coins it holds')} ({formatCount(activity.coinCount ?? 0)})</h3>
              <Pager total={activity.coinCount ?? 0} bind:page={coinsPage} size={prefs.explorerPageSize} label="coins" />
            </div>
            <table class="grid" aria-label="Coins of the address">
              <thead><tr><th>Coin</th><th>Block</th><th class="end">Amount</th></tr></thead>
              <tbody>
                {#each activity.coins ?? [] as [txid, vout, amount, height, immature] (`${txid}:${vout}`)}
                  <tr>
                    <td><button type="button" class="mono linkish" title={`${txid}:${vout}`} onclick={() => ontx(txid, height)}>{shortHash(txid, 8)}:{vout}</button>{#if immature} <span class="badge warning">{t('maturing')}</span>{/if}</td>
                    <td class="num">{#if height >= 0}<button type="button" class="mono linkish" onclick={() => onblock(height)}>{formatCount(height)}</button>{:else}<span class="faint">waiting</span>{/if}</td>
                    <td class="end"><Amount sats={amount} /></td>
                  </tr>
                {:else}
                  <tr><td colspan="3" class="faint">None</td></tr>
                {/each}
              </tbody>
            </table>
          </div>
        {/if}
      </div>
    {/if}
  </div>
</section>

<style>
  .addr-line {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
  }
  .not-ours {
    margin: 0.8rem 0 0;
  }
  .lists {
    display: grid;
    grid-template-columns: minmax(0, 1.3fr) minmax(0, 1fr);
    gap: 1.5rem;
    margin-top: 1rem;
  }
  .lists.one {
    grid-template-columns: minmax(0, 1fr);
  }
  .list-head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
  }
  .list-head h3 {
    margin: 0;
    font-size: 0.95rem;
  }
  .list-head :global(.pager) {
    padding: 0;
  }
  table.grid {
    width: 100%;
    margin-top: 0.4rem;
    border-collapse: collapse;
    font-size: 0.9rem;
  }
  table.grid th {
    text-align: left;
    font-size: 0.8rem;
    font-weight: 600;
    color: var(--text-muted);
    padding: 0.35rem 0.6rem;
    border-bottom: 1px solid var(--line);
    white-space: nowrap;
  }
  table.grid td {
    padding: 0.3rem 0.6rem;
    border-bottom: 1px solid var(--line);
  }
  table.grid .end {
    text-align: right;
  }
  .linkish {
    padding: 0;
    border: 0;
    background: none;
    color: var(--link);
    cursor: pointer;
  }
  .linkish:hover {
    text-decoration: underline;
  }
  @media (max-width: 1000px) {
    .lists {
      grid-template-columns: minmax(0, 1fr);
    }
  }
</style>
