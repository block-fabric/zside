<script lang="ts">
  // Memos: the inbox of the shielded account. Every memo a shielded payment brought it (listshieldedtransactions:
  // those of payments to its addresses, never of its change), newest first, with what the payment brought and
  // when; read in a pane of its own, the unread ones marked (for this viewer). A memo is text of the payer:
  // shown exactly as written (SafeText), never as anything else. zside-qt showed them in a column of its
  // history only.
  import Amount from '../../components/Amount.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { formatTime, shortHash } from '../../lib/format';
  import { t, tn } from '../../lib/i18n.svelte';
  import { go, nav, PAGES } from '../../lib/nav.svelte';
  import { wallets, walletsEnabled } from '../../lib/state.svelte';
  import '../zs/zs.css';
  import Privacy from '../zs/Privacy.svelte';
  import { memosOf } from '../zs/model';
  import { isRead, markRead, memoState, startZs, zs } from '../zs/store.svelte';

  $effect(() => startZs());

  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  const all = $derived(memosOf(zs.txs));
  let query = $state('');
  let onlyUnread = $state(false);
  const list = $derived.by(() => {
    void memoState.version;
    const q = query.trim().toLowerCase();
    return all.filter((m) => (!onlyUnread || !isRead(m.key)) && (!q || m.text.toLowerCase().includes(q)));
  });
  const unread = $derived.by(() => {
    void memoState.version;
    return all.filter((m) => !isRead(m.key)).length;
  });

  // The memo open: from the tab (go('memos', key)), or the newest.
  let openKey = $state('');
  $effect(() => {
    if (nav.tab && all.some((m) => m.key === nav.tab)) openKey = nav.tab;
  });
  const open = $derived(all.find((m) => m.key === openKey) ?? null);
  function show(key: string) {
    openKey = key;
    markRead([key]);
  }
  function onKey(e: KeyboardEvent) {
    if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
    const i = list.findIndex((m) => m.key === openKey);
    const next = list[Math.max(0, Math.min(list.length - 1, i + (e.key === 'ArrowDown' ? 1 : -1)))];
    if (next) {
      e.preventDefault();
      show(next.key);
      document.getElementById(`zs-memo-${next.key}`)?.focus();
    }
  }
</script>

<div class="page memos zs">
  <header class="page-head">
    <h1>{t(PAGES.memos.title)}</h1>
    {#if !noWallet}<span class="muted">{tn('%n unread', unread)}</span>{/if}
  </header>

  {#if noWallet}
    <NoWallet page={PAGES.memos} />
  {:else}
    <div class="bar">
      <input type="search" class="grow" placeholder="Search the memos" aria-label="Search the memos" bind:value={query} />
      <label class="check"><input type="checkbox" bind:checked={onlyUnread} /> Unread only</label>
      <button type="button" class="btn ghost small" disabled={unread === 0} onclick={() => markRead(all.map((m) => m.key))}><Icon name="check" size={15} /> Mark all read</button>
    </div>

    {#if all.length === 0}
      <section class="panel empty">
        <Icon name="memo" size={34} />
        <p>No memo yet. A memo comes with a shielded payment: only the sender and you read it.</p>
        <button type="button" class="btn" onclick={() => go('private', 'receive')}><Icon name="qr" size={16} /> Your shielded address</button>
      </section>
    {:else}
      <div class="inbox">
        <section class="panel list" aria-label="Memos">
          {#if list.length === 0}
            <p class="muted pad">No memo matches.</p>
          {:else}
            <ul class="zs-list" role="listbox" aria-label="Memos" tabindex="-1" onkeydown={onKey}>
              {#each list as m (m.key)}
                {@const read = isRead(m.key)}
                <li>
                  <button type="button" id={`zs-memo-${m.key}`} class="item" class:active={m.key === openKey} class:unread={!read} role="option" aria-selected={m.key === openKey} onclick={() => show(m.key)}>
                    <span class="line1">
                      {#if !read}<span class="zs-dot" aria-label="Unread"></span>{/if}
                      <SafeText class="snippet" text={m.text.split('\n')[0]} />
                    </span>
                    <span class="line2 faint small">{formatTime(m.time, true)} · <Amount sats={m.amount} />{#if m.confirmations === 0} · {t('waiting for a block')}{/if}</span>
                  </button>
                </li>
              {/each}
            </ul>
          {/if}
        </section>

        <section class="panel reader" aria-label="The memo">
          {#if open}
            <div class="panel-head">
              <h2>{formatTime(open.time)}</h2>
              <Privacy kind="private" why={t('A memo is encrypted to the recipient: nobody else reads it.')} />
            </div>
            <div class="panel-body">
              <div class="text"><SafeText class="zs-memo" text={open.text} /></div>
              <dl class="facts">
                <dt>Came with</dt><dd><Amount sats={open.amount} /></dd>
                <dt>Confirmations</dt><dd class="num">{open.confirmations}</dd>
                <dt>Transaction</dt><dd class="mono"><SafeText text={shortHash(open.txid, 16)} title={open.txid} /> <CopyButton text={open.txid} label="Copy the transaction id" /></dd>
              </dl>
              <div class="zs-actions">
                <CopyButton text={open.text} label="Copy the memo" small={false} iconOnly={false} />
                <button type="button" class="btn ghost" onclick={() => markRead([open.key], false)}>Mark unread</button>
                <button type="button" class="btn ghost" onclick={() => go('zhistory', `tx-${open.txid}`)}>The transaction</button>
              </div>
              <p class="faint small">The memo is shown as the payer wrote it. Links in it are not opened: copy one if you trust it.</p>
            </div>
          {:else}
            <div class="panel-body muted">Choose a memo to read it.</div>
          {/if}
        </section>
      </div>
    {/if}
  {/if}
</div>

<style>
  .memos {
    display: grid;
    gap: 1rem;
    align-content: start;
  }
  .bar {
    display: flex;
    flex-wrap: wrap;
    gap: 0.6rem;
    align-items: center;
  }
  .empty {
    display: grid;
    justify-items: center;
    gap: 0.8rem;
    padding: 2.4rem 1rem;
    color: var(--text-muted);
    text-align: center;
  }
  .empty p {
    margin: 0;
    max-width: 30rem;
  }
  .inbox {
    display: grid;
    grid-template-columns: minmax(16rem, 2fr) minmax(0, 3fr);
    gap: 1rem;
    align-items: start;
  }
  @media (max-width: 760px) {
    .inbox {
      grid-template-columns: minmax(0, 1fr);
    }
  }
  .list {
    max-height: 70vh;
    overflow: auto;
  }
  .pad {
    padding: 1rem;
  }
  .item {
    display: flex;
    flex-direction: column;
    gap: 0.2rem;
    width: 100%;
    padding: 0.7rem 0.95rem;
    border: 0;
    background: none;
    color: var(--text);
    font: inherit;
    text-align: start;
    cursor: pointer;
  }
  .item:hover {
    background: var(--raised);
  }
  .item.active {
    background: color-mix(in srgb, var(--brand, var(--accent)) 12%, transparent);
    box-shadow: inset 3px 0 0 var(--brand, var(--accent));
  }
  .item.unread :global(.snippet) {
    font-weight: 650;
  }
  .line1 {
    display: flex;
    align-items: center;
    gap: 0.45rem;
    min-width: 0;
  }
  .line1 :global(.snippet) {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .text {
    padding: 1rem 1.1rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m, 10px);
    background: var(--raised);
    font-size: 1.02rem;
  }
  .reader .panel-body {
    display: grid;
    gap: 0.9rem;
  }
  .reader p {
    margin: 0;
  }
</style>
