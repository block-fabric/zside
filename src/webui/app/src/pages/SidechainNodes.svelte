<script lang="ts">
  // The Sidechain Nodes window of chains-qt: run the nodes of sidechains from here. Their programs
  // are located on this computer or downloaded (with a hash check); a node started here follows
  // this node, mines its blocks through the miners of this chain, and holds what is deposited.
  import { every } from '../lib/poll';
  import { t } from '../lib/i18n.svelte';
  import SafeText from '../components/SafeText.svelte';
  import Icon from '../components/Icon.svelte';
  import { confirm, errorText, inform, prompt } from '../lib/dialogs.svelte';
  import { formatCount } from '../lib/format';
  import { host, type SidechainNodeEntry, type SidechainNodeList, type SidechainNodeMethod, type SidechainNodeStatus } from '../lib/rpc';
  import { wallets } from '../lib/state.svelte';

  // Inside the Sidechains page (its Nodes tab): without a title of its own.
  let { embedded = false }: { embedded?: boolean } = $props();

  const REFRESH_MS = 4000;
  const TITLE = $derived(t('Sidechain Nodes'));

  let list = $state<SidechainNodeList | null>(null);
  let status = $state<Record<string, SidechainNodeStatus>>({});
  let selected = $state<string | null>(null);
  let log = $state<string[]>([]);
  let busy = $state(false);

  const chosen = $derived(list?.sidechains.find((s) => s.stem === selected) ?? null);
  const chosenStatus = $derived(selected ? status[selected] : undefined);

  function say(lines: string[]) {
    const d = new Date();
    const t = `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}:${String(d.getSeconds()).padStart(2, '0')}`;
    log = [...log, ...lines.map((l, i) => (i === 0 ? `${t}  ${l}` : l))].slice(-500);
  }

  async function refresh() {
    try {
      list = await host.sidechainNodes();
      // The answers of the nodes come one by one.
      for (const s of list.sidechains) {
        if (!s.installed) {
          status[s.stem] = { stem: s.stem, installed: false, running: false };
          continue;
        }
        host
          .sidechainStatus(s.stem)
          .then((st) => (status[s.stem] = st))
          .catch(() => undefined);
      }
    } catch {
      // The next refresh tries again.
    }
  }
  $effect(() => {
    return every(REFRESH_MS, refresh, { now: true });
  });

  async function run(method: SidechainNodeMethod, params: Record<string, unknown> = {}) {
    if (!chosen) {
      await inform(TITLE, t('Select a sidechain in the list first.'));
      return null;
    }
    busy = true;
    try {
      const r = await host.sidechainNode(method, { stem: chosen.stem, ...params });
      if (r?.log) say(r.log);
      if (r?.error) await inform(TITLE, r.error, 'warning');
      return r;
    } catch (e) {
      await inform(TITLE, errorText(e), 'warning');
      return null;
    } finally {
      busy = false;
      setTimeout(refresh, 1500);
    }
  }

  const walletParam = () => (wallets.current === null ? {} : { wallet: wallets.current });

  async function startNode() {
    if (!chosen?.installed) return inform(TITLE, t('Select a sidechain whose programs are on this computer.'));
    if (chosenStatus?.running) return say([`${chosen.name} is running already.`]);
    await run('start', walletParam());
  }
  async function startWallet() {
    if (!chosen?.installed) return inform(TITLE, t('Select a sidechain whose programs are on this computer.'));
    if (chosenStatus?.running) return inform(TITLE, t('The node of %1 is running in the background. Stop it first: the wallet window is a node itself, and there can be only one.', chosen.name));
    await run('startWallet', walletParam());
  }
  async function toggleMining() {
    if (!chosen || !chosenStatus?.running) return inform(TITLE, t('Select a sidechain whose node is running.'));
    if (!chosenStatus.mining && wallets.current === null) return inform(TITLE, t('Open a wallet of this node first: it pays the miners of this chain for each block of the sidechain.'));
    await run('mining', { on: !chosenStatus.mining });
  }
  async function mineOnce() {
    if (!chosen || !chosenStatus?.running) return inform(TITLE, t('Select a sidechain whose node is running.'));
    if (wallets.current === null) return inform(TITLE, t('Open a wallet of this node first: it pays the miners of this chain for the block of the sidechain.'));
    const amount = await prompt(TITLE, t('What to offer the miners of this chain for one block of %1, in coins of this chain:', chosen.name), '0.0001');
    if (!amount?.trim()) return;
    await run('mineOnce', { amount: amount.trim() });
  }
  // The app asks the user itself, in a dialog of the desktop showing the address, the hash and the folder,
  // before anything is downloaded: this page only says what to download.
  async function download() {
    if (!chosen) return inform(TITLE, t('Select a sidechain in the list first.'));
    if (chosen.release) {
      say([`${chosen.name}: the release ${chosen.release.version} built into this app`]);
      await run('download', { release: true });
      return;
    }
    // The app takes secure web addresses only, and says so.
    const url = (await prompt(TITLE, t('Address of the release of %1 to download (a .tar.gz archive, at a secure web address):', chosen.name)))?.trim();
    if (!url) return;
    const sha256 = (await prompt(TITLE, t('SHA-256 hash that the publisher gives for it. The download is thrown away if it does not match:')))?.trim().toLowerCase();
    if (sha256 === undefined) return;
    if (!/^[0-9a-f]{64}$/.test(sha256)) return inform(TITLE, t('A SHA-256 hash is 64 hexadecimal characters. Programs that hold coins are not installed unchecked.'), 'warning');
    await run('download', { url, sha256 });
  }
  async function addSidechain() {
    const name = (await prompt(TITLE, t('Name of the sidechain:')))?.trim();
    if (!name) return;
    const stem = (await prompt(TITLE, t('What its programs are named after: if its node is "examplechaind", enter "examplechain".'), name.toLowerCase().replace(/ /g, '')))?.trim();
    if (!stem) return;
    const slotText = await prompt(TITLE, t('The slot of the sidechain on this chain:'), '0');
    if (slotText === null) return;
    const slot = Number(slotText);
    if (!Number.isInteger(slot) || slot < 0 || slot > 65535) return inform(TITLE, t('The slot is a number from 0 to 65535.'), 'warning');
    try {
      const r = await host.sidechainNode('add', { name, stem, slot });
      if (r?.log) say(r.log);
      selected = stem;
      refresh();
    } catch (e) {
      await inform(TITLE, errorText(e));
    }
  }
  async function removeSidechain() {
    if (!chosen) return inform(TITLE, t('Select the sidechain to remove.'));
    if (chosen.builtin) return inform(TITLE, t('%1 comes with this wallet and stays in the list. Only a sidechain that was added by hand can be removed.', chosen.name));
    if (chosenStatus?.running) return inform(TITLE, t('The node of %1 is running. Stop it first.', chosen.name));
    if (!(await confirm(TITLE, t('Take %1 off the list? Its programs and its data stay on this computer.', chosen.name), { yes: 'Remove', danger: true }))) return;
    await run('remove');
    selected = null;
  }

  /** What the list says of the node, in the language shown. */
  function nodeWords(s: SidechainNodeEntry): string {
    const state = nodeText(s);
    return state === 'running' ? t('running') : state === 'stopped' ? t('stopped') : state;
  }

  /** The state of the node: its class in the list too. */
  function nodeText(s: SidechainNodeEntry): string {
    const st = status[s.stem];
    if (!s.installed) return '–';
    if (!st) return '…';
    return st.running ? 'running' : 'stopped';
  }
</script>

<div class="nodes" class:page={!embedded}>
  {#if embedded}
    {#if list}<p class="muted">{t('Nodes of sidechains on this computer')} <span class="badge">{list.network}</span></p>{/if}
  {:else}
    <header class="page-head"><h1>Sidechain nodes {#if list}<span class="badge">{list.network}</span>{/if}</h1></header>
  {/if}
  <p class="muted">Run the nodes of sidechains from here. A sidechain node follows this node, mines its blocks through the miners of this chain, and holds the coins you deposit to the sidechain.</p>
  {#if list && !list.server}
    <p class="notice" role="alert" id="sidechain-nodes-notice">
      <Icon name="alert" size={16} />
      <span><strong>This node does not accept RPC connections</strong>, which sidechain nodes need to follow it. Turn on "Enable RPC server" in Settings → Main and restart, or start with -server.</span>
    </p>
  {/if}

  <div class="cards" role="radiogroup" aria-label="Sidechains">
    {#each list?.sidechains ?? [] as s (s.stem)}
      {@const st = status[s.stem]}
      <button type="button" role="radio" aria-checked={selected === s.stem} class="card panel" class:chosen={selected === s.stem} onclick={() => (selected = s.stem)} title={s.description}>
        <span class="top">
          <strong><SafeText text={s.name} /></strong>
          <span class="state {nodeText(s)}"><span class="dot"></span>{nodeWords(s)}</span>
        </span>
        <span class="muted desc">{t(s.description)}</span>
        <span class="facts-row">
          <span><span class="faint">Slot</span> <span class="num">{s.slot}</span></span>
          {#if st?.running}<span><span class="faint">Blocks</span> <span class="num">{formatCount(st.blocks ?? 0)}</span></span>{/if}
        </span>
        {#if st?.running}
          <span class="line"><span class="faint">Follows this node:</span> {st.mainchain ?? '…'}</span>
          <span class="line"><span class="faint">Mining:</span> {st.miningText ?? '…'}</span>
        {/if}
        <span class="line programs" class:missing={!s.installed}><span class="faint">Programs:</span> <span class="mono">{s.programs}</span></span>
      </button>
    {/each}
  </div>

  <div class="actions">
    <div class="row wrap">
      <button type="button" class="btn primary" disabled={busy} title="Start the node of the selected sidechain in the background" onclick={startNode}>Start node</button>
      <button type="button" class="btn" disabled={busy} title="Start the selected sidechain with its wallet window, in place of the background node" onclick={startWallet}>Open wallet</button>
      <button type="button" class="btn" disabled={busy} title="Stop the node of the selected sidechain" onclick={() => run('stop')}>Stop</button>
      <button type="button" class="btn" disabled={busy} title="Have the selected sidechain ask for a block whenever its transactions carry fees that pay for one. It offers the miners of this chain 99% of those fees, from the open wallet of this node, and keeps the fees; without fees it asks for nothing." onclick={toggleMining}>Auto mining on / off</button>
      <button type="button" class="btn" disabled={busy} title="Have the selected sidechain mine one block now, empty or not, for a fee you choose, paid by the open wallet of this node. This is how a deposit gets paid when the sidechain has no transactions." onclick={mineOnce}>Mine one block…</button>
      <button type="button" class="btn" disabled={busy} title="Show the end of the log of the node" onclick={() => run('log')}>Log</button>
    </div>
    <div class="row wrap">
      <button type="button" class="btn ghost" disabled={busy} title="Tell where the programs of the selected sidechain are on this computer" onclick={() => run('locate')}><Icon name="folder" size={15} /> Locate programs…</button>
      <button type="button" class="btn ghost" disabled={busy} title="Download a release of the selected sidechain and check it" onclick={download}><Icon name="down" size={15} /> Download…</button>
      <button type="button" class="btn ghost" disabled={busy} title="List a sidechain that is not in the list" onclick={addSidechain}><Icon name="plus" size={15} /> Add sidechain…</button>
      <button type="button" class="btn ghost" disabled={busy} title="Take the selected sidechain off this list. Only for a sidechain that was added by hand; its programs and its data stay on the computer." onclick={removeSidechain}><Icon name="trash" size={15} /> Remove</button>
    </div>
  </div>

  <section class="panel">
    <div class="panel-head"><h2>Log</h2>{#if log.length}<button type="button" class="btn ghost small" onclick={() => (log = [])}>Clear</button>{/if}</div>
    <pre id="sidechain-nodes-log" class="log mono" aria-live="polite">{log.join('\n') || 'What happens shows here.'}</pre>
  </section>
</div>

<style>
  .notice {
    display: flex;
    gap: 0.6rem;
    align-items: flex-start;
    max-width: none;
    padding: 0.7rem 0.9rem;
    border-radius: var(--radius-s);
    background: var(--warning-soft);
    color: var(--warning);
  }
  .cards {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(17rem, 100%), 1fr));
    gap: 0.8rem;
    margin: 1rem 0;
  }
  .card {
    display: flex;
    flex-direction: column;
    align-items: stretch;
    gap: 0.35rem;
    padding: 0.85rem 1rem;
    text-align: left;
    font: inherit;
    color: var(--text);
    cursor: pointer;
  }
  .card.chosen {
    border-color: var(--accent);
    box-shadow: inset 0 0 0 1px var(--accent);
  }
  .top {
    display: flex;
    justify-content: space-between;
    align-items: center;
  }
  .desc {
    font-size: 0.88rem;
  }
  .facts-row {
    display: flex;
    gap: 1rem;
    font-size: 0.9rem;
  }
  .line {
    font-size: 0.85rem;
    overflow-wrap: anywhere;
  }
  .programs.missing {
    color: var(--text-muted);
  }
  .state {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
    font-size: 0.82rem;
    color: var(--text-muted);
  }
  .state .dot {
    width: 0.55rem;
    height: 0.55rem;
    border-radius: 50%;
    background: var(--text-faint);
  }
  .state.running {
    color: var(--positive);
  }
  .state.running .dot {
    background: var(--positive);
  }
  .actions {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    margin-bottom: 1rem;
  }
  .wrap {
    flex-wrap: wrap;
  }
  .log {
    margin: 0;
    padding: 0.4rem 1.1rem 1rem;
    max-height: 16rem;
    overflow: auto;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }
</style>
