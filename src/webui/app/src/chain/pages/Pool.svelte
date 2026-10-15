<script lang="ts">
  // The shielded pool of the chain (getshieldedinfo): what it holds (all that went in, less all that came
  // out, which no rule lets go below zero), the notes it has made (the size of the tree of note commitments),
  // the nullifiers revealed (one per action, padding included), the root of the tree; and the index of the
  // whole tree that wallets need to spend (-orchardindex), synced or not. zside-qt showed the value of the
  // pool and its notes on its Private page.
  import Amount from '../../components/Amount.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { formatCount } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { PAGES } from '../../lib/nav.svelte';
  import { node } from '../../lib/state.svelte';
  import { coinsToSats } from '../../lib/units';
  import '../zs/zs.css';
  import Privacy from '../zs/Privacy.svelte';
  import { refreshZs, startZs, zs } from '../zs/store.svelte';

  $effect(() => startZs());
  const pool = $derived(zs.pool);
  const indexMatches = $derived(!!pool?.index && pool.index.root === pool.root);
</script>

<div class="page pool zs">
  <header class="page-head">
    <h1>{t(PAGES.pool.title)}</h1>
    <button type="button" class="btn ghost small" onclick={() => void refreshZs()}><Icon name="refresh" size={15} /> Refresh</button>
  </header>

  {#if !pool}
    <section class="panel"><p class="muted pad">The node has not answered yet.</p></section>
  {:else}
    <section class="zs-tiles" aria-label="The pool">
      <div class="zs-tile">
        <span class="k">In the pool</span>
        <span class="v"><Amount sats={coinsToSats(pool.pool)} /></span>
        <span class="n">All that was shielded, less all that was unshielded</span>
      </div>
      <div class="zs-tile">
        <span class="k">Notes made</span>
        <span class="v num">{formatCount(pool.notes)}</span>
        <span class="n">The size of the tree of note commitments</span>
      </div>
      <div class="zs-tile">
        <span class="k">Nullifiers revealed</span>
        <span class="v num">{formatCount(pool.spent)}</span>
        <span class="n">One per action, those of padding included</span>
      </div>
      <div class="zs-tile">
        <span class="k">At block</span>
        <span class="v num">{node.tip ? formatCount(node.tip.height) : '–'}</span>
        <span class="n">The tip of this node</span>
      </div>
    </section>

    <div class="zs-cols">
      <section class="panel">
        <div class="panel-head"><h2>The tree of notes</h2></div>
        <div class="panel-body grid">
          <span class="muted small">Its root, which every spend proves its notes against</span>
          <div class="row"><SafeText class="zs-code" text={pool.root} /><CopyButton text={pool.root} label="Copy the root" /></div>
          {#if pool.index}
            <div class="zs-note" class:private={pool.index.synced && indexMatches} class:public={!pool.index.synced}>
              <Icon name={pool.index.synced ? 'check' : 'refresh'} size={18} />
              <span>
                {#if pool.index.synced && indexMatches}
                  {t('The index of the whole tree is synced: %1 notes, the same root. Wallets of this node can spend.', formatCount(pool.index.notes))}
                {:else if pool.index.synced}
                  {t('The index of the whole tree holds %1 notes; its root is not that of the tip yet.', formatCount(pool.index.notes))}
                {:else}
                  {t('The index of the whole tree is catching up: %1 notes so far. A payment waits for it.', formatCount(pool.index.notes))}
                {/if}
              </span>
            </div>
          {:else}
            <div class="zs-note danger"><Icon name="alert" size={18} /><span>This node keeps no index of the tree (-orchardindex=0): its wallets cannot spend shielded notes.</span></div>
          {/if}
        </div>
      </section>

      <section class="panel">
        <div class="panel-head"><h2>What the chain shows</h2></div>
        <div class="panel-body">
          <ul class="zs-list shows">
            <li><Privacy kind="public" /><span>How much goes into the pool and comes out, and the transparent addresses at either end.</span></li>
            <li><Privacy kind="private" /><span>Inside the pool: who pays whom, how much, and the memos. Each note is encrypted to its recipient.</span></li>
            <li><Privacy kind="private" /><span>Which note a payment spends: it reveals a nullifier, which nobody but the owner can link to the note.</span></li>
          </ul>
          <p class="faint small">Every proof (Halo 2, no trusted setup) and signature is checked by every node, and the pool can never pay out more than went in.</p>
        </div>
      </section>
    </div>
  {/if}
</div>

<style>
  .pool {
    display: grid;
    gap: 1rem;
    align-content: start;
  }
  .page-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 1rem;
  }
  .pad {
    padding: 1rem;
    margin: 0;
  }
  .grid {
    display: grid;
    gap: 0.6rem;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 0.4rem;
  }
  .row :global(.zs-code) {
    flex: 1;
    min-width: 0;
  }
  .shows li {
    display: flex;
    gap: 0.7rem;
    align-items: flex-start;
    padding: 0.6rem 0;
  }
  p {
    margin: 0.4rem 0 0;
  }
</style>
