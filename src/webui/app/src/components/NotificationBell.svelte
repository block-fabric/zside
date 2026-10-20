<script lang="ts">
  // The bell of the top bars: the new transactions since the app started, a dot while some are not
  // seen. A notice of the wallet shown opens its transaction in the detail panel.
  import { t } from '../lib/i18n.svelte';
  import { CHAIN } from '../chain/config';
  import SafeText from './SafeText.svelte';
  import Amount from './Amount.svelte';
  import Icon from './Icon.svelte';
  import { showDetail } from '../lib/detail.svelte';
  import { formatTime } from '../lib/format';
  import { go } from '../lib/nav.svelte';
  import { markAllRead, noticeList, systemList, type SystemNotice } from '../lib/notices.svelte';
  import { mainLink } from '../lib/mainlink.svelte';
  import { prefs, wallets } from '../lib/state.svelte';
  import { addressText, noticeRecordOf, typeText, walletDisplayName } from '../lib/wallet';
  import { current } from '../lib/walletdata.svelte';

  let open = $state(false);
  let root: HTMLElement | undefined = $state();
  const list = $derived(noticeList());
  // Notices of the app itself (the link to the mainchain node), above the transactions.
  const system = $derived(systemList());
  const unread = $derived(list.filter((n) => n.unread).length + system.filter((n) => n.unread).length);
  const label = $derived(unread ? `Notifications: ${unread} new` : 'Notifications');

  function toggle() {
    open = !open;
  }
  function close() {
    if (open) markAllRead();
    open = false;
  }
  function record(wallet: string, txid: string) {
    // One per transaction: its halves to the wallet itself merged (a send with its own outputs is a send).
    return wallet === wallets.current ? noticeRecordOf(current.records, txid) : undefined;
  }
  function visit(wallet: string, txid: string) {
    const r = record(wallet, txid);
    close();
    if (r && wallets.current !== null && !prefs.mask) showDetail({ kind: 'tx', wallet: wallets.current, txid, idx: r.idx, address: r.address });
    else go('transactions');
  }
  function visitSystem(n: SystemNotice) {
    close();
    // The banner says what to do while the link is down; once it is back, the mainchain's settings.
    if (n.kind !== 'mainchain-restored' && mainLink.banner) mainLink.expanded = true;
    else go('settings', 'mainchain');
  }
</script>

<svelte:window
  onmousedown={(e) => {
    if (open && root && !root.contains(e.target as Node)) close();
  }}
  onkeydown={(e) => {
    if (open && e.key === 'Escape') {
      close();
      root?.querySelector<HTMLElement>('.bell')?.focus();
    }
  }}
/>

<div class="notices" bind:this={root}>
  <button type="button" class="btn round bell" aria-label={label} title={label} aria-expanded={open} aria-haspopup="dialog" onclick={toggle}>
    <Icon name="bell" size={17} />
    {#if unread}<span class="dot" aria-hidden="true"></span>{/if}
  </button>
  {#if open}
    <div class="pop" role="dialog" aria-label="Notifications">
      <header>
        <h2>Notifications</h2>
        {#if unread}<button type="button" class="btn ghost small" onclick={markAllRead}>Mark all as read</button>{/if}
      </header>
      {#if system.length}
        <ul class="system" aria-label="Notices of the app">
          {#each system.slice(0, 6) as n (n.id)}
            <li>
              <button type="button" class="item" class:unread={n.unread} onclick={() => visitSystem(n)}>
                <span class="ic" class:bad={n.kind === 'mainchain-down'} class:warn={n.kind === 'mainchain-syncing'} class:in={n.kind === 'mainchain-restored'}><Icon name={n.kind === 'mainchain-restored' ? 'check' : 'alert'} size={15} /></span>
                <span class="what">
                  <span class="title"><SafeText text={n.title} /></span>
                  <span class="faint small"><SafeText text={n.text} /> · {formatTime(n.time)}</span>
                </span>
              </button>
            </li>
          {/each}
        </ul>
      {/if}
      {#if list.length === 0}
        <p class="muted empty">No new transactions since {CHAIN.name} started. They show here as they come.</p>
      {:else}
        <ul>
          {#each list.slice(0, 12) as n (n.wallet + n.txid)}
            {@const r = record(n.wallet, n.txid)}
            <li>
              <button type="button" class="item" class:unread={n.unread} onclick={() => visit(n.wallet, n.txid)}>
                <span class="ic" class:in={r && r.amount >= 0n}><Icon name={r && r.amount < 0n ? 'send' : 'receive'} size={15} /></span>
                <span class="what">
                  <span class="title">{r ? (r.amount >= 0n ? 'Incoming transaction' : 'Sent transaction') : 'New transaction'}</span>
                  <span class="faint small">{#if r}{#if addressText(r)}<SafeText text={addressText(r)} />{:else}{typeText(r.type, r.role)}{/if} · {formatTime(r.time)}{:else}In wallet <SafeText text={walletDisplayName(n.wallet)} />{/if}</span>
                </span>
                {#if r}<Amount sats={r.amount} colored plus showUnit={false} />{/if}
              </button>
            </li>
          {/each}
        </ul>
      {/if}
    </div>
  {/if}
</div>

<style>
  .notices {
    position: relative;
  }
  .dot {
    position: absolute;
    top: 0.42rem;
    right: 0.48rem;
    width: 0.5rem;
    height: 0.5rem;
    border-radius: 50%;
    background: var(--accent);
    box-shadow:
      0 0 0 2px var(--surface-2),
      0 0 8px var(--accent);
  }
  .pop {
    position: absolute;
    z-index: 45;
    top: calc(100% + 0.5rem);
    right: 0;
    width: min(23rem, 90vw);
    padding: 0.4rem;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-l);
    background: var(--surface);
    box-shadow: var(--shadow);
  }
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0.45rem 0.6rem;
  }
  h2 {
    font-size: 0.95rem;
  }
  ul {
    margin: 0;
    padding: 0;
    list-style: none;
    max-height: 22rem;
    overflow-y: auto;
  }
  .item {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    width: 100%;
    padding: 0.5rem 0.6rem;
    border: 0;
    border-radius: var(--radius-m);
    background: none;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .item:hover {
    background: var(--raised);
  }
  .item.unread .title::after {
    content: '';
    display: inline-block;
    width: 0.4rem;
    height: 0.4rem;
    margin-left: 0.4rem;
    border-radius: 50%;
    background: var(--accent);
    vertical-align: middle;
  }
  .ic {
    display: inline-grid;
    place-items: center;
    width: 2rem;
    height: 2rem;
    flex: none;
    border-radius: 50%;
    background: var(--raised);
    color: var(--text-muted);
  }
  .ic.bad {
    background: var(--danger-soft);
    color: var(--negative);
  }
  .ic.warn {
    background: var(--warning-soft);
    color: var(--warning);
  }
  ul.system {
    margin-bottom: 0.3rem;
    padding-bottom: 0.3rem;
    border-bottom: 1px solid var(--line);
  }
  .ic.in {
    background: var(--accent-soft);
    color: var(--positive);
  }
  .what {
    display: flex;
    flex-direction: column;
    flex: 1;
    min-width: 0;
  }
  .what > span {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .title {
    font-weight: 600;
  }
  .small {
    font-size: 0.82rem;
  }
  .empty {
    margin: 0;
    padding: 0.3rem 0.6rem 0.8rem;
    font-size: 0.9rem;
  }
</style>
