<script lang="ts">
  // Blind merged mining of a sidechain (the Mine tab of the template's Qt Mainchain page and its mining bar,
  // redone): the miners of the mainchain mine the blocks of this chain for a fee, which the wallet of the
  // mainchain node pays. Automatic mining (setbmm: on or off, the address of the fees, "always" and its
  // offer), one block by hand (requestbmmblock), what happened (the requests and their results, the
  // candidates kept), the blocks of the mainchain that committed to this chain, and the connection.
  import { t } from '../lib/i18n.svelte';
  import { stacked } from '../lib/actions';
  import SafeText from '../components/SafeText.svelte';
  import Amount from '../components/Amount.svelte';
  import AmountInput from '../components/AmountInput.svelte';
  import Icon from '../components/Icon.svelte';
  import { CHAIN } from '../chain/config';
  import { errorText, inform } from '../lib/dialogs.svelte';
  import { formatCount, formatTime, shortHash } from '../lib/format';
  import { go, PAGES } from '../lib/nav.svelte';
  import { rpc } from '../lib/rpc';
  import { node, prefs, savePrefs, toast, wallets, walletsEnabled } from '../lib/state.svelte';
  import { coinsToSats, formatAmount, satsToCoins } from '../lib/units';
  import { bmmBlocks, bmmHost, bmmShare, declined, type BmmRequest } from '../lib/side';
  import { noteManualRequest, refreshSide, side, startSide, type BmmLogEntry } from '../lib/sidestate.svelte';

  const mainName = CHAIN.mainchain?.name ?? '';
  /** Mainchain blocks shown as marks, and in the table. */
  const MARKS = 48;
  const ROWS = 20;

  $effect(() => {
    startSide();
    void refreshSide(true);
  });
  $effect(() => {
    void node.tip?.height;
    void refreshSide(true);
  });

  const bmm = $derived(side.bmm);
  const info = $derived(side.info);
  const connected = $derived(!!info?.connected);
  const mining = $derived(!!bmm?.mining);

  let address = $state(prefs.miningAddress);
  let always = $state(false);
  let alwaysOffer = $state<bigint | null>(10_000n);
  let onceOffer = $state<bigint | null>(10_000n);
  let busy = $state(false);
  let lastManual = $state<BmmRequest | null>(null);
  // While mining, the form shows what the node mines with.
  $effect(() => {
    if (bmm?.mining) {
      if (bmm.address) address = bmm.address;
      always = !!bmm.always;
      if (bmm.amount !== undefined) alwaysOffer = coinsToSats(bmm.amount);
    }
  });

  /** The address of the fees: the one entered, or a new one of the wallet (labelled "mining"). */
  async function feeAddress(): Promise<string | null> {
    if (address.trim()) return address.trim();
    if (!walletsEnabled() || wallets.current === null) {
      await inform(t('Merged mining'), t('Open or create a wallet first, or enter an address of this chain for the fees.'), 'warning');
      return null;
    }
    try {
      address = await rpc.call('getnewaddress', ['mining'], { wallet: wallets.current });
      return address;
    } catch (e) {
      await inform(t('Merged mining'), errorText(e), 'warning');
      return null;
    }
  }

  async function setMining(on: boolean) {
    if (busy) return;
    busy = true;
    try {
      if (on) {
        const to = await feeAddress();
        if (!to) return;
        // The host checks the offer and asks the user on the desktop (the wallet of the mainchain node pays).
        await bmmHost.set(true, to, always, satsToCoins(alwaysOffer ?? 10_000n));
        savePrefs({ miningAddress: to });
        toast('success', t('Merged mining on'), always ? t('A block is asked for with every block of %1.', mainName) : t('A block is asked for whenever there are fees to pay for it.'));
      } else {
        await bmmHost.set(false);
        toast('info', t('Merged mining off'), t('No more blocks are asked for.'));
      }
    } catch (e) {
      if (!declined(e)) await inform(t('Merged mining'), errorText(e), 'warning');
    } finally {
      busy = false;
      void refreshSide(true);
    }
  }

  async function mineOnce() {
    if (busy) return;
    if (onceOffer === null || onceOffer <= 0n) {
      await inform(t('Merged mining'), t('Enter the offer for the miners of %1.', mainName), 'warning');
      return;
    }
    const to = await feeAddress();
    if (!to) return;
    busy = true;
    try {
      lastManual = await bmmHost.request(to, satsToCoins(onceOffer));
      noteManualRequest(lastManual.fees, lastManual.amount);
      toast('success', t('Block asked for'), t('It is mined if the next block of %1 takes the offer.', mainName));
    } catch (e) {
      if (!declined(e)) await inform(t('Merged mining'), errorText(e), 'warning');
    } finally {
      busy = false;
      void refreshSide(true);
    }
  }

  // --- The blocks of the mainchain ---
  const marks = $derived(bmmBlocks(side.main.events, MARKS).reverse());
  const share = $derived(bmmShare(side.main.events, MARKS));
  const rows = $derived(bmmBlocks(side.main.events, ROWS));
  /** Whether the block of this chain a mainchain block committed to is in the chain of this node. */
  let inChain = $state<Record<string, boolean>>({});
  $effect(() => {
    const hashes = rows.map((r) => r.bmm).filter((h): h is string => !!h && !(h in inChain));
    if (!hashes.length) return;
    (async () => {
      const found: Record<string, boolean> = {};
      for (const h of hashes) {
        found[h] = await rpc
          .raw<{ confirmations: number }>('getblockheader', [h, true])
          .then((b) => b.confirmations >= 0)
          .catch(() => false);
      }
      inChain = { ...inChain, ...found };
    })();
  });

  function describeEntry(e: BmmLogEntry): string {
    switch (e.kind) {
      case 'on':
        return t('Automatic mining turned on');
      case 'off':
        return t('Automatic mining turned off');
      case 'request':
        return e.fees !== undefined && e.offer !== undefined
          ? t('Asked for %1 block(s): %2 of fees, %3 offered', e.count ?? 1, formatAmount(coinsToSats(e.fees), prefs.unit, { withUnit: true }), formatAmount(coinsToSats(e.offer), prefs.unit, { withUnit: true }))
          : t('Asked for %1 block(s)', e.count ?? 1);
      case 'manual':
        return t('Asked for one block by hand: %1 of fees, %2 offered', formatAmount(coinsToSats(e.fees ?? 0), prefs.unit, { withUnit: true }), formatAmount(coinsToSats(e.offer ?? 0), prefs.unit, { withUnit: true }));
      case 'mined':
        return t('%1 block(s) of this node mined', e.count ?? 1);
      case 'outbid':
        return t('Outbid by another node for %1 block(s)', e.count ?? 1);
      default:
        return t('A request failed');
    }
  }
  function logTone(e: BmmLogEntry): '' | 'ok' | 'wait' | 'bad' {
    return e.kind === 'mined' ? 'ok' : e.kind === 'error' || e.kind === 'outbid' ? 'bad' : e.kind === 'off' ? '' : 'wait';
  }
</script>

<div class="page bmm">
  <header class="page-head"><h1>{t(PAGES.bmm.title)}</h1></header>
  <p class="side-note">
    The blocks of this chain are mined by the miners of {mainName}, who commit to them in their own blocks for a fee, which the wallet of the {mainName} node pays (blind merged mining). Automatic mining asks for a block whenever
    there are transactions whose fees pay for one: it offers the miners of {mainName} 99% of those fees, and you keep the fees, so one hundredth is yours. Without fees it asks for nothing and costs nothing.
  </p>

  <section class="side-stats">
    <div class="panel side-stat">
      <span class="muted">Automatic mining</span>
      <strong class="state" class:on={mining}><span class="dot"></span>{mining ? (bmm?.idle ? 'On, waiting for fees' : 'On') : 'Off'}</strong>
      <span class="faint">{mining ? (bmm?.always ? 'a block with every block of the mainchain' : 'when fees pay for a block') : 'no blocks asked for'}</span>
    </div>
    <div class="panel side-stat"><span class="muted">Blocks mined</span><strong class="num" id="bmm-blocks">{bmm ? formatCount(bmm.blocks) : '–'}</strong><span class="faint">by this node</span></div>
    <div class="panel side-stat"><span class="muted">Requests</span><strong class="num" id="bmm-requests">{bmm ? formatCount(bmm.requests) : '–'}</strong><span class="faint">{bmm ? `outbid ${formatCount(bmm.outbid)} times` : ''}</span></div>
    <div class="panel side-stat"><span class="muted">Candidates</span><strong class="num">{bmm ? formatCount(bmm.candidates) : '–'}</strong><span class="faint">blocks built, waiting for a commitment</span></div>
  </section>

  {#if bmm?.error}<p class="side-note side-bad" role="alert">Last attempt failed: <SafeText text={bmm.error} /></p>{/if}

  <div class="side-cols">
    <section class="panel">
      <div class="panel-head"><h2>Automatic mining</h2><span class="side-link" class:on={mining}><span class="dot"></span>{mining ? 'Mining' : 'Not mining'}</span></div>
      <form class="panel-body side-form" onsubmit={(e) => { e.preventDefault(); setMining(true); }}>
        <div class="field">
          <label for="bmm-address">Pay the fees to</label>
          <div class="row">
            <input id="bmm-address" type="text" class="mono grow" spellcheck="false" autocomplete="off" placeholder="An address of this chain; empty for a new one of the wallet" bind:value={address} disabled={mining} />
          </div>
        </div>
        <label class="check">
          <input id="bmm-always" type="checkbox" bind:checked={always} disabled={mining} />
          <span>Always: a block with every block of {mainName}, with or without fees</span>
        </label>
        {#if always}
          <AmountInput id="bmm-always-amount" label="Offer for each block" bind:value={alwaysOffer} disabled={mining} />
          <p class="side-note side-warn">For tests: every block of {mainName} costs the offer, fees or not.</p>
        {/if}
        <div class="row">
          <button type="submit" class="btn primary" id="bmm-start" disabled={mining || busy}><Icon name="merged" size={16} /> Start mining</button>
          <button type="button" class="btn" id="bmm-stop" disabled={!mining || busy} onclick={() => setMining(false)}>Stop mining</button>
        </div>
        {#if mining && bmm?.lastoffer !== undefined}
          <p class="side-note">Last block asked for: <Amount sats={coinsToSats(bmm.lastfees ?? 0)} /> of fees, <Amount sats={coinsToSats(bmm.lastoffer)} /> offered.</p>
        {/if}
      </form>
    </section>

    <section class="panel">
      <div class="panel-head"><h2>One block by hand</h2></div>
      <form class="panel-body side-form" onsubmit={(e) => { e.preventDefault(); mineOnce(); }}>
        <p class="side-note">A block of this chain, for an offer you choose: how a deposit gets credited, or a withdrawal bundle started, when no transactions pay for a block.</p>
        <AmountInput id="bmm-once-amount" label="Offer to the miners of {mainName}" bind:value={onceOffer} />
        <div class="row"><button type="submit" class="btn" id="bmm-once" disabled={busy}><Icon name="block" size={16} /> Mine one block</button></div>
        {#if lastManual}
          <p class="side-note" id="bmm-once-result">
            Asked for block <span class="mono" translate="no">{shortHash(lastManual.hash, 10)}</span> with {lastManual.transactions - 1} transaction(s) and <Amount sats={coinsToSats(lastManual.fees)} /> of fees, offering <Amount sats={coinsToSats(lastManual.amount)} />, on block
            {formatCount(lastManual.mainchainheight)} of {mainName}.
          </p>
        {/if}
      </form>
    </section>
  </div>

  <div class="side-cols">
    <section class="panel">
      <div class="panel-head"><h2>Blocks of {mainName}</h2><span class="muted small">{share.of ? `${share.committed} of the last ${share.of} committed to this chain` : ''}</span></div>
      <div class="panel-body">
        {#if marks.length === 0}
          <p class="muted">{side.main.error ? side.main.error : `No block of ${mainName} on record yet.`}</p>
        {:else}
          <div class="side-blocks" role="img" aria-label="{share.committed} of the last {share.of} blocks of {mainName} committed to a block of this chain">
            {#each marks as m (m.hash)}<span class:hit={!!m.bmm} class:miss={!m.bmm} title={m.bmm ? `Block ${m.height}: committed to ${shortHash(m.bmm, 8)}` : `Block ${m.height}: no commitment`}></span>{/each}
          </div>
        {/if}
      </div>
      {#if rows.length}
        <div class="side-table-wrap">
          <table class="side-table" use:stacked aria-label="Commitments of the blocks of {mainName}">
            <thead><tr><th>Block of {mainName}</th><th>Time</th><th>Block of this chain</th><th>Result</th></tr></thead>
            <tbody>
              {#each rows as r (r.hash)}
                <tr>
                  <td class="num">{formatCount(r.height)}</td>
                  <td>{formatTime(r.time, true)}</td>
                  <td class="mono">{#if r.bmm}<button type="button" class="link mono" onclick={() => go('explorer', `search:${r.bmm}`)} translate="no">{shortHash(r.bmm, 10)}</button>{:else}<span class="muted">none</span>{/if}</td>
                  <td>{#if r.bmm}<span class="status {inChain[r.bmm] ? 'ok' : inChain[r.bmm] === false ? 'bad' : ''}">{inChain[r.bmm] ? 'In this chain' : inChain[r.bmm] === false ? 'Not in this chain' : '…'}</span>{/if}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>

    <div class="stack">
      <section class="panel">
        <div class="panel-head"><h2>{mainName} node</h2><span class="side-link" class:on={connected} class:off={info !== null && !connected}><span class="dot"></span>{connected ? 'Connected' : info ? 'Not reachable' : '…'}</span></div>
        <dl class="facts panel-body">
          <dt>Node</dt><dd class="mono"><span translate="no">{side.connection?.node ?? info?.node ?? '–'}</span></dd>
          <dt>Block</dt><dd class="num">{info && info.height >= 0 ? formatCount(info.height) : '–'}</dd>
          <dt>Wallet that pays</dt><dd><span translate="no">{side.connection ? (side.connection.walletSet ? side.connection.wallet || '(default wallet)' : 'its only wallet loaded') : '–'}</span></dd>
          <dt>Credentials</dt><dd>{side.connection ? (side.connection.auth === 'cookie' ? 'its cookie file' : 'a user name and password') : '–'}</dd>
          {#if side.connection?.cookieFile}<dt>Cookie file</dt><dd class="mono"><span translate="no">{side.connection.cookieFile}</span></dd>{/if}
          {#if info && !connected}<dt>Error</dt><dd class="side-bad"><SafeText text={info.error ?? ''} /></dd>{/if}
        </dl>
        <div class="panel-body"><button type="button" class="btn small" onclick={() => go('settings', 'mainchain')}><Icon name="settings" size={14} /> Connection settings</button></div>
      </section>

      <section class="panel">
        <div class="panel-head"><h2>Activity</h2><span class="muted small">since the app started</span></div>
        {#if side.log.length === 0}
          <p class="side-empty">Nothing yet: requests, blocks mined and outbids show here as they happen.</p>
        {:else}
          <ul class="log" aria-label="Merged mining activity">
            {#each side.log.slice(0, 60) as e, i (i)}
              <li><span class="status {logTone(e)}">{formatTime(e.time, true)}</span> {#if e.kind === 'error'}<SafeText text={e.text ?? ''} />{:else}{describeEntry(e)}{/if}</li>
            {/each}
          </ul>
        {/if}
      </section>
    </div>
  </div>
</div>

<style>
  .state {
    display: inline-flex;
    align-items: center;
    gap: 0.45rem;
    color: var(--text-muted);
  }
  .state .dot {
    width: 0.65rem;
    height: 0.65rem;
    border-radius: 50%;
    background: var(--text-faint);
  }
  .state.on {
    color: var(--positive);
  }
  .state.on .dot {
    background: var(--positive);
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    min-width: 0;
  }
  .log {
    list-style: none;
    margin: 0;
    padding: 0.4rem 1.2rem 1rem;
    display: flex;
    flex-direction: column;
    gap: 0.45rem;
    max-height: 22rem;
    overflow-y: auto;
    font-size: 0.9rem;
  }
  .link {
    padding: 0;
    border: 0;
    background: none;
    color: var(--link);
    cursor: pointer;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
