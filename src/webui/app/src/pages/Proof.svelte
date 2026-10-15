<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  // Proof of funds (ProofOfFundsDialog of chains-qt): a statement signed with every address of the
  // wallet that holds coins, and the check of such a proof against the chain as it is now. Optionally
  // (user decision, 2026-10-15) only some of them: chosen by address, or by coin (the coins are then listed
  // in the proof and signed with the statement: lib/tools.ts PROOF_COINS).
  import Amount from '../components/Amount.svelte';
  import CoinPanel from '../components/CoinPanel.svelte';
  import CopyButton from '../components/CopyButton.svelte';
  import SafeText from '../components/SafeText.svelte';
  import { errorText, inform, withKeys } from '../lib/dialogs.svelte';
  import { ownTab, setOwnTab } from '../lib/nav.svelte';
  import { rpc } from '../lib/rpc';
  import { noWalletText, wallets } from '../lib/state.svelte';
  import { checkProofCoins, formatProof, parseProof, proofCoinLine, proofMessage, proofVerdict, simplified, unitsToCoins, type ProofCoin, type TxOutNow } from '../lib/tools';
  import { walletApi, type Coin, type Outpoint } from '../lib/wallet';
  import { node } from '../lib/state.svelte';

  // A tab of a page that holds it (Tools, Advanced transactions): without a title of its own.
  let { embedded = false }: { embedded?: boolean } = $props();

  const tab = $derived(ownTab('proof') === 'verify' ? 'verify' : 'prove');
  const today = new Date().toLocaleDateString(undefined, { year: 'numeric', month: 'long', day: 'numeric' });

  let statement = $state(`These funds are mine. ${today}`);
  let proof = $state('');
  let proving = $state(false);
  let input = $state('');
  let verdict = $state('');
  let verdictKind = $state<'valid' | 'partly' | 'invalid' | ''>('');
  let checking = $state(false);

  // --- What to prove: all the addresses holding coins (as chains-qt), or some, by address or by coin ---
  let choose = $state(false);
  let by = $state<'address' | 'coin'>('address');
  let coins = $state.raw<Coin[]>([]);
  let pickedAddresses = $state<string[]>([]);
  let pickedCoins = $state<Outpoint[]>([]);
  let addressFilter = $state('');
  const keyOf = (o: Outpoint) => `${o.txid}:${o.vout}`;

  $effect(() => {
    const wallet = wallets.current;
    void wallets.txVersion;
    void node.tip?.height;
    if (!choose || wallet === null) return;
    walletApi(wallet)
      .coins()
      .then((groups) => (coins = groups.flatMap((g) => g.coins)))
      .catch(() => (coins = []));
  });

  /** The addresses holding coins: their balance, label and coins. */
  const holders = $derived.by(() => {
    const by = new Map<string, { address: string; label: string; sats: bigint; count: number }>();
    for (const c of coins) {
      const h = by.get(c.address) ?? { address: c.address, label: c.change ? '' : c.label, sats: 0n, count: 0 };
      h.sats += c.sats;
      h.count++;
      if (!h.label && c.label && !c.change) h.label = c.label;
      by.set(c.address, h);
    }
    return [...by.values()].sort((a, b) => (a.sats === b.sats ? (a.address < b.address ? -1 : 1) : a.sats > b.sats ? -1 : 1));
  });
  const shownHolders = $derived.by(() => {
    const q = addressFilter.trim().toLowerCase();
    return q ? holders.filter((h) => h.address.toLowerCase().includes(q) || h.label.toLowerCase().includes(q)) : holders;
  });
  const pickedSet = $derived(new Set(pickedAddresses));
  const pickedCoinSet = $derived(new Set(pickedCoins.map(keyOf)));
  /** What the proof covers now: the addresses that sign, the coins listed (by coin), and their total. */
  const plan = $derived.by(() => {
    if (!choose) return null;
    if (by === 'address') {
      const chosen = holders.filter((h) => pickedSet.has(h.address));
      return { addresses: chosen.map((h) => h.address), coins: [] as Coin[], total: chosen.reduce((s, h) => s + h.sats, 0n) };
    }
    const chosen = coins.filter((c) => pickedCoinSet.has(keyOf(c)));
    return { addresses: [...new Set(chosen.map((c) => c.address))], coins: chosen, total: chosen.reduce((s, c) => s + c.sats, 0n) };
  });

  function toggleAddress(address: string) {
    pickedAddresses = pickedSet.has(address) ? pickedAddresses.filter((a) => a !== address) : [...pickedAddresses, address];
  }

  async function prove() {
    const text = simplified(statement);
    if (!text) {
      await inform(t('Proof of Funds'), t('Enter the statement to sign.'));
      return;
    }
    const wallet = wallets.current;
    if (wallet === null) {
      await inform(t('Proof of Funds'), noWalletText());
      return;
    }
    proving = true;
    try {
      let addresses: string[];
      let listed: Coin[] = [];
      if (plan) {
        if (plan.addresses.length === 0) {
          await inform(t('Proof of Funds'), by === 'address' ? t('Choose the addresses to prove.') : t('Choose the coins to prove.'));
          return;
        }
        addresses = [...plan.addresses].sort();
        listed = [...plan.coins].sort((a, b) => (a.txid === b.txid ? a.vout - b.vout : a.txid < b.txid ? -1 : 1));
      } else {
        // Whoever checks the proof looks the coins up on the chain: unconfirmed ones count here.
        const unspent = await rpc.raw<{ address?: string }[]>('listunspent', [0], { wallet });
        addresses = [...new Set(unspent.map((c) => c.address).filter((a): a is string => !!a))].sort();
      }
      // With coins chosen, what is signed lists them; the coins of an address that cannot sign are left out.
      let proofCoins: ProofCoin[] = listed.map((c) => ({ txid: c.txid, vout: c.vout, sats: c.sats }));
      const message = (list: ProofCoin[]) => proofMessage(text, list);
      let failed = 0;
      let lastError = '';
      // Every address signs in one call of the host, with the keys unlocked for it alone.
      const answers =
        addresses.length === 0
          ? []
          : await withKeys<string>(
              wallet,
              addresses.map((address) => ({ method: 'signmessage', params: [address, message(proofCoins)] })),
            );
      const signed =
        answers === null
          ? null
          : answers.flatMap((a, i) => {
              if (a.error || typeof a.result !== 'string') {
                failed++;
                lastError = a.error?.message ?? 'no signature';
                return [];
              }
              return [{ address: addresses[i], signature: a.result }];
            });
      if (signed === null) return;
      let signatures = signed;
      let coinsLeftOut = 0;
      if (listed.length && signatures.length < addresses.length) {
        // Some addresses could not sign: their coins are not listed, and the others sign the shorter list.
        const signers = new Set(signatures.map((s) => s.address));
        const kept = listed.filter((c) => signers.has(c.address));
        coinsLeftOut = listed.length - kept.length;
        proofCoins = kept.map((c) => ({ txid: c.txid, vout: c.vout, sats: c.sats }));
        if (proofCoins.length) {
          const again = await withKeys<string>(wallet, [...signers].sort().map((address) => ({ method: 'signmessage', params: [address, message(proofCoins)] })));
          if (again === null) return;
          const sorted = [...signers].sort();
          signatures = again.flatMap((a, i) => (a.error || typeof a.result !== 'string' ? [] : [{ address: sorted[i], signature: a.result }]));
        } else signatures = [];
      }
      if (signatures.length === 0) {
        proof = '';
        await inform(t('Proof of Funds'), addresses.length === 0 ? t('The wallet holds no coins on an address.') : t('None of the addresses could sign: %1', lastError), 'warning');
        return;
      }
      proof = formatProof(text, signatures, proofCoins);
      if (failed > 0) {
        await inform(
          t('Proof of Funds'),
          coinsLeftOut
            ? t('%1 chosen coin(s) are left out of the proof: they are on addresses that cannot sign a message (only addresses that stand for a single key can): %2', coinsLeftOut, lastError)
            : t('%1 address(es) holding coins are left out of the proof, because they cannot sign a message (only addresses that stand for a single key can): %2', failed, lastError),
        );
      }
    } catch (e) {
      await inform(t('Proof of Funds'), errorText(e), 'warning');
    } finally {
      proving = false;
    }
  }

  async function verify() {
    const parsed = parseProof(input);
    if (!parsed) {
      verdict = 'This is not a proof of funds.';
      verdictKind = 'invalid';
      return;
    }
    checking = true;
    try {
      const valid = new Set<string>();
      let invalid = 0;
      let report = '';
      for (const entry of parsed.entries) {
        let ok = false;
        if ('address' in entry) {
          ok = await rpc.raw<boolean>('verifymessage', [entry.address, entry.signature, parsed.message]).catch(() => false);
        }
        if (ok && 'address' in entry) valid.add(entry.address);
        else {
          invalid++;
          report += `INVALID signature for ${'address' in entry ? entry.address : entry.invalid}\n`;
        }
      }
      let total = '0';
      if (valid.size) {
        // Looking through all coins can take a while on a long chain.
        const scan = await rpc.raw<{ height: number; total_amount: number; unspents: unknown[] }>('scantxoutset', ['start', [...valid].map((a) => `addr(${a})`)]);
        total = scan.total_amount.toFixed(8);
        report += `Coins on these addresses now, at block ${scan.height}: ${total} in ${scan.unspents.length} output(s)\n`;
      }
      // The coins listed (the extension of the format): each still unspent on the chain now, paid to an address
      // that signed, of the amount listed.
      let coinsBad = 0;
      let coinsGood = 0;
      if (parsed.coins.length) {
        const now: (TxOutNow | null)[] = [];
        for (const c of parsed.coins) now.push(await rpc.raw<TxOutNow | null>('gettxout', [c.txid, c.vout, false]).catch(() => null));
        const check = checkProofCoins(parsed.coins, now, valid);
        coinsGood = check.good.length;
        coinsBad = parsed.coins.length - coinsGood;
        report += `\nCoins listed in the proof: ${parsed.coins.length}. Unspent now, on addresses that signed: ${check.good.length}, holding ${unitsToCoins(check.total)} CHN.\n`;
        for (const c of check.spent) report += `SPENT or not on the chain: ${proofCoinLine(c)}\n`;
        for (const c of check.unsigned) report += `NOT ON A SIGNED ADDRESS: ${proofCoinLine(c)} (paid to ${c.address || 'no address'})\n`;
        for (const c of check.wrongAmount) report += `WRONG AMOUNT: ${proofCoinLine(c)} (it holds ${unitsToCoins(c.now)})\n`;
      }
      verdict = `${proofVerdict(valid.size, invalid, total)}\n\n    ${parsed.statement}\n\n${report}`;
      verdictKind = invalid === 0 && valid.size > 0 && coinsBad === 0 ? 'valid' : valid.size === 0 || (parsed.coins.length > 0 && coinsGood === 0) ? 'invalid' : 'partly';
    } catch (e) {
      verdict = errorText(e);
      verdictKind = 'invalid';
    } finally {
      checking = false;
    }
  }
</script>

<div class="proof" class:page={!embedded}>
  {#if !embedded}<header class="page-head"><h1>Proof of funds</h1></header>{/if}
  <!-- Inside another page (Tools, Advanced transactions), its tabs are on that page's row. -->
  {#if !embedded}
  <div class="tabs" role="tablist" aria-label="Prove or verify">
    <button type="button" role="tab" aria-selected={tab === 'prove'} onclick={() => (setOwnTab('proof', 'prove'))}>Prove</button>
    <button type="button" role="tab" aria-selected={tab === 'verify'} onclick={() => (setOwnTab('proof', 'verify'))}>Verify</button>
  </div>
  {/if}

  {#if tab === 'prove'}
    <section class="panel">
      <form class="panel-body form" onsubmit={(e) => { e.preventDefault(); prove(); }}>
        <p class="muted">
          Sign a statement with every address of the open wallet that holds coins (or, with "Choose what to prove", only the addresses or the coins you choose; chosen coins are listed in the proof). Whoever you give the proof to can check that the holder of those coins made the statement. It moves no coins and costs nothing,
          but it reveals which addresses are yours.
        </p>
        <div class="field">
          <label for="proof-statement">Statement</label>
          <div class="row">
            <input id="proof-statement" type="text" class="grow" bind:value={statement} />
            <button type="submit" class="btn primary" disabled={proving || wallets.current === null}>{proving ? 'Signing…' : 'Create proof'}</button>
          </div>
        </div>
        <div class="choose">
          <label class="switch" title="Prove only some addresses or some coins, not all the addresses holding coins">
            <input type="checkbox" role="switch" bind:checked={choose} />
            <span class="track" aria-hidden="true"></span>
            <span>Choose what to prove</span>
          </label>
          {#if choose}
            <div class="tabs" role="tablist" aria-label="Choose by">
              <button type="button" role="tab" aria-selected={by === 'address'} onclick={() => (by = 'address')}>By address</button>
              <button type="button" role="tab" aria-selected={by === 'coin'} onclick={() => (by = 'coin')}>By coin</button>
            </div>
          {/if}
        </div>
        {#if choose && wallets.current !== null}
          {#if by === 'address'}
            <div class="addresses">
              <div class="row toolbar">
                <input type="search" class="grow" placeholder="Search address or label" aria-label="Search the addresses" bind:value={addressFilter} />
                <button type="button" class="btn small" disabled={shownHolders.length === 0} onclick={() => (pickedAddresses = [...new Set([...pickedAddresses, ...shownHolders.map((h) => h.address)])])}>Select all</button>
                <button type="button" class="btn small" disabled={pickedAddresses.length === 0} onclick={() => (pickedAddresses = [])}>Select none</button>
              </div>
              <ul class="holders" aria-label="Addresses holding coins">
                {#each shownHolders as h (h.address)}
                  <li>
                    <label class="holder">
                      <input type="checkbox" checked={pickedSet.has(h.address)} onchange={() => toggleAddress(h.address)} aria-label="{t('Prove')} {h.address}" />
                      <span class="addr mono"><SafeText text={h.address} /></span>
                      <span class="label">{#if h.label}<SafeText text={h.label} />{:else}<span class="faint">{t('(no label)')}</span>{/if}</span>
                      <span class="faint count">{h.count} {t('coin(s)')}</span>
                      <span class="sum"><Amount sats={h.sats} /></span>
                    </label>
                  </li>
                {:else}
                  <li class="faint">{coins.length ? t('Nothing matches the search') : t('The wallet holds no coins on an address.')}</li>
                {/each}
              </ul>
            </div>
          {:else}
            <div class="coins-holder"><CoinPanel wallet={wallets.current} bind:selected={pickedCoins} purpose="prove" /></div>
          {/if}
          {#if plan}
            <p class="plan" aria-live="polite">
              {#if by === 'address'}{t('The proof will be signed with %1 address(es), holding', plan.addresses.length)}{:else}{t('The proof will list %1 coin(s), on %2 address(es), holding', plan.coins.length, plan.addresses.length)}{/if}
              <strong><Amount sats={plan.total} /></strong>.
            </p>
          {/if}
        {/if}
        <div class="field">
          <label for="proof-output">Proof</label>
          <textarea id="proof-output" class="mono" rows="10" readonly value={proof}></textarea>
        </div>
        <div class="row"><CopyButton text={proof} label="Copy proof" small={false} /></div>
      </form>
    </section>
  {:else}
    <section class="panel">
      <form class="panel-body form" onsubmit={(e) => { e.preventDefault(); verify(); }}>
        <div class="field">
          <label for="proof-input">Paste a proof to check it against the chain as it is now:</label>
          <p class="muted hint">A proof that lists coins (made with "Choose what to prove", by coin) is checked coin by coin too: each must be unspent on the chain now, paid to an address that signed, of the amount listed. Proofs of chains-qt, without coins, are checked as before.</p>
          <textarea id="proof-input" class="mono" rows="10" bind:value={input}></textarea>
        </div>
        <div class="row"><button type="submit" class="btn primary" disabled={checking}>{checking ? 'Checking…' : 'Check proof'}</button></div>
        {#if verdict}
          <pre id="proof-verdict" class="verdict {verdictKind}" aria-live="polite">{verdict}</pre>
        {/if}
      </form>
    </section>
  {/if}
</div>

<style>
  .choose {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.6rem 1.2rem;
  }
  .choose .tabs {
    margin: 0;
  }
  .addresses {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .holders {
    max-height: 18rem;
    margin: 0;
    padding: 0;
    overflow: auto;
    list-style: none;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
  }
  .holders li {
    border-bottom: 1px solid var(--line);
    padding: 0.4rem 0.7rem;
  }
  .holders li:last-child {
    border-bottom: 0;
  }
  .holder {
    display: grid;
    grid-template-columns: auto minmax(0, 2fr) minmax(0, 1fr) auto auto;
    align-items: center;
    gap: 0.7rem;
    cursor: pointer;
  }
  .holder .addr,
  .holder .label {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .holder .sum {
    text-align: right;
  }
  .coins-holder :global(.holder) {
    height: 18rem;
  }
  .plan {
    margin: 0;
  }
  @media (max-width: 760px) {
    .holder {
      grid-template-columns: auto minmax(0, 1fr) auto;
    }
    .holder .label,
    .holder .count {
      display: none;
    }
  }
  .form {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    padding-top: 1.1rem;
  }
  .verdict {
    margin: 0;
    padding: 0.9rem 1rem;
    border-radius: var(--radius-s);
    background: var(--bg);
    border-left: 4px solid var(--line-strong);
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    font-family: var(--font-ui);
  }
  .verdict.valid {
    border-left-color: var(--positive);
  }
  .verdict.partly {
    border-left-color: var(--warning);
  }
  .verdict.invalid {
    border-left-color: var(--negative);
  }
</style>
