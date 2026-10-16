<script lang="ts">
  // The cards of zSide on the home of the dashboard: the shielded balance beside the transparent one (each
  // with its privacy, and how much of the wallet is shielded), the memos received (the unread ones first), and
  // the melt/cast plan while it has steps to go. Each opens its page.
  import Amount from '../components/Amount.svelte';
  import Icon from '../components/Icon.svelte';
  import SafeText from '../components/SafeText.svelte';
  import Card from '../views/home/Card.svelte';
  import { formatTime } from '../lib/format';
  import { t, tn } from '../lib/i18n.svelte';
  import { go } from '../lib/nav.svelte';
  import { node, wallets } from '../lib/state.svelte';
  import { coinsToSats } from '../lib/units';
  import './zs/zs.css';
  import Privacy from './zs/Privacy.svelte';
  import Ring from './zs/Ring.svelte';
  import { memosOf, sortPlan } from './zs/model';
  import { isRead, memoState, startZs, zs } from './zs/store.svelte';
  import { simpleMode } from '../lib/uimode.svelte';

  $effect(() => startZs());

  const available = $derived(coinsToSats(zs.balance?.available ?? 0));
  const pending = $derived(coinsToSats(zs.balance?.pending ?? 0));
  const transparent = $derived(coinsToSats((zs.transparent?.trusted ?? 0) + (zs.transparent?.pending ?? 0) + (zs.transparent?.immature ?? 0)));
  const shielded = $derived(available + pending);
  const share = $derived(shielded + transparent > 0n ? Number(shielded) / Number(shielded + transparent) : 0);
  const memos = $derived(memosOf(zs.txs));
  const unread = $derived.by(() => {
    void memoState.version;
    return memos.filter((m) => !isRead(m.key));
  });
  const recent = $derived.by(() => {
    void memoState.version;
    return [...unread, ...memos.filter((m) => isRead(m.key))].slice(0, 3);
  });
  const toGo = $derived(sortPlan(zs.plan).filter((s) => s.status !== 'sent'));
  // Simple mode: the private balance and its two moves; the memos and melt & cast are Advanced's.
  const simple = $derived(simpleMode());
</script>

{#if wallets.current !== null && zs.loaded && zs.balance}
  <div class="zs zs-cards">
    <Card title="Private" icon="private" open={() => go('private')} openLabel="Open the shielded wallet" stat>
      <div class="pair">
        <Ring {share} size={64} label={t('%1% of the wallet is shielded', String(Math.round(share * 100)))} />
        <dl class="pairs">
          <dt><Privacy kind="private" bare /></dt><dd class="num"><Amount sats={available} /></dd>
          <dt><Privacy kind="public" bare /></dt><dd class="num"><Amount sats={transparent} /></dd>
          {#if pending > 0n}<dt class="muted">Waiting</dt><dd class="num"><Amount sats={pending} /></dd>{/if}
        </dl>
      </div>
      {#snippet actions()}
        {#if simple}
          <button type="button" class="btn grow" onclick={() => go('send')}><Icon name="private" size={15} /> Send privately</button>
          <button type="button" class="btn ghost" onclick={() => go('private', 'move')}>Make private</button>
        {:else}
          <button type="button" class="btn grow" onclick={() => go('private', 'pay')}><Icon name="private" size={15} /> Pay privately</button>
          <button type="button" class="btn ghost" onclick={() => go('private', 'move')}>Shield</button>
        {/if}
      {/snippet}
    </Card>

    {#if !simple}
    <Card title="Memos" icon="memo" open={() => go('memos')} openLabel="Open the memos" stat>
      {#if memos.length === 0}
        <p class="muted">No memo yet: they come with shielded payments.</p>
      {:else}
        <span class="value num">{unread.length}</span><span class="muted small">{t('unread, of %1', String(memos.length))}</span>
        <ul class="memos">
          {#each recent as m (m.key)}
            <li>
              <button type="button" class="link" onclick={() => go('memos', m.key)}>
                {#if !isRead(m.key)}<span class="zs-dot" aria-label="Unread"></span>{/if}
                <SafeText class="snippet" text={m.text.split('\n')[0]} />
              </button>
              <span class="faint small">{formatTime(m.time, true)}</span>
            </li>
          {/each}
        </ul>
      {/if}
    </Card>

    {/if}
    {#if toGo.length && !simple}
      <Card title="Melt & cast" icon="meltcast" open={() => go('meltcast')} openLabel="Open melt and cast" stat>
        <span class="muted">{tn('%n step(s) to go', toGo.length)}</span>
        {#if toGo[0]?.height !== undefined}
          <p class="small">{t('Next at block %1', String(toGo[0].height))}{#if node.tip && (toGo[0].height ?? 0) > node.tip.height} · {tn('in %n block(s)', (toGo[0].height ?? 0) - node.tip.height)}{/if}</p>
        {/if}
      </Card>
    {/if}
  </div>
{/if}

<style>
  .zs-cards {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(17rem, 100%), 1fr));
    gap: var(--gap, 1rem);
  }
  .pair {
    display: flex;
    align-items: center;
    gap: 0.9rem;
  }
  .pairs {
    display: grid;
    grid-template-columns: auto auto;
    gap: 0.3rem 0.8rem;
    align-items: center;
    margin: 0;
  }
  .pairs dd {
    margin: 0;
    text-align: end;
  }
  .value {
    font-size: 1.6rem;
    font-weight: 700;
    margin-right: 0.4rem;
  }
  .memos {
    display: grid;
    gap: 0.35rem;
    margin: 0.5rem 0 0;
    padding: 0;
    list-style: none;
  }
  .memos li {
    display: flex;
    justify-content: space-between;
    gap: 0.6rem;
    min-width: 0;
  }
  .memos .link {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    min-width: 0;
  }
  .memos :global(.snippet) {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  p {
    margin: 0.3rem 0 0;
  }
</style>
