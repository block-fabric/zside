<script lang="ts">
  // The peers tab of the node window: the peers (PeerTableModel) with the details of the one
  // selected, a menu to disconnect or ban it, and the banned addresses (BanTableModel) to unban.
  import { every } from '../lib/poll';
  import SafeText from './SafeText.svelte';
  import { t } from '../lib/i18n.svelte';
  import ContextMenu, { type ContextItem } from './ContextMenu.svelte';
  import DataTable from './DataTable.svelte';
  import Icon from './Icon.svelte';
  import { untrack } from 'svelte';
  import { detail, showDetail } from '../lib/detail.svelte';
  import { confirm, errorText } from '../lib/dialogs.svelte';
  import { takeFocus } from '../lib/nav.svelte';
  import { formatBytes, formatTime, shortDuration } from '../lib/format';
  import { BAN_TIMES, connectionType, networkName, peerDetails, pingText, type BanEntry, type PeerInfo } from '../lib/peers';
  import { host, rpc } from '../lib/rpc';
  import { node, toast } from '../lib/state.svelte';
  import type { Column } from '../lib/table';

  let peers = $state<PeerInfo[]>([]);
  let bans = $state<BanEntry[]>([]);
  // The peer the tab was opened for (the detail panel, the dashboard), else none.
  let selected = $state<string | null>(takeFocus('node'));
  // The workspace layout shows the peer selected in its detail panel.
  $effect(() => {
    const id = selected;
    if (id !== null && detail.follow) untrack(() => showDetail({ kind: 'peer', id: Number(id) }));
  });
  let selectedBan = $state<string | null>(null);
  let sortKey = $state<string | null>('id');
  let sortDir = $state<'asc' | 'desc'>('asc');
  let now = $state(Math.floor(Date.now() / 1000));
  let menu = $state({ open: false, x: 0, y: 0, peer: null as PeerInfo | null });
  let banMenu = $state({ open: false, x: 0, y: 0, ban: null as BanEntry | null });

  async function refresh() {
    try {
      peers = await rpc.raw<PeerInfo[]>('getpeerinfo');
      now = Math.floor(Date.now() / 1000);
    } catch {
      // The next refresh tries again.
    }
  }
  async function refreshBans() {
    try {
      bans = await rpc.raw<BanEntry[]>('listbanned');
    } catch {
      bans = [];
    }
  }

  $effect(() => {
    return every(2000, refresh, { now: true });
  });
  $effect(() => {
    void node.bansChanged;
    refreshBans();
  });

  const peer = $derived(peers.find((p) => String(p.id) === selected) ?? null);

  const columns: Column<PeerInfo>[] = [
    { key: 'id', label: 'Peer', value: (p) => p.id, width: '4.5rem', align: 'end' },
    { key: 'age', label: 'Age', value: (p) => now - p.conntime, width: '6rem', align: 'end' },
    { key: 'addr', label: 'Address', value: (p) => p.addr, width: 'minmax(12rem, 1.4fr)', mono: true },
    { key: 'direction', label: 'Direction', value: (p) => (p.inbound ? 'Inbound' : 'Outbound'), width: '6.5rem' },
    { key: 'type', label: 'Type', value: (p) => connectionType(p.connection_type, p.inbound), width: '8rem' },
    { key: 'network', label: 'Network', value: (p) => networkName(p.network), width: '6.5rem' },
    { key: 'ping', label: 'Ping', value: (p) => p.pingtime ?? -1, width: '5.5rem', align: 'end' },
    { key: 'sent', label: 'Sent', value: (p) => p.bytessent, width: '6rem', align: 'end' },
    { key: 'recv', label: 'Received', value: (p) => p.bytesrecv, width: '6rem', align: 'end' },
    { key: 'subver', label: 'User Agent', value: (p) => p.subver, width: 'minmax(10rem, 1fr)' },
  ];

  const banColumns: Column<BanEntry>[] = [
    { key: 'address', label: 'IP/Netmask', value: (b) => b.address, width: 'minmax(12rem, 1fr)', mono: true },
    { key: 'until', label: 'Banned Until', value: (b) => b.banned_until, width: '12rem' },
  ];

  async function disconnect(p: PeerInfo) {
    try {
      await host.disconnectPeer(p.id);
      selected = null;
      refresh();
    } catch (e) {
      toast('error', t('Disconnect'), errorText(e));
    }
  }

  async function ban(p: PeerInfo, seconds: number, label: string) {
    try {
      await host.banPeer(p.id, seconds);
      toast('info', t('Peer banned'), t('%1 for %2', p.addr, label));
      selected = null;
      refresh();
      refreshBans();
    } catch (e) {
      toast('error', t('Ban'), errorText(e));
    }
  }

  async function unban(b: BanEntry) {
    try {
      await host.unban(b.address);
      selectedBan = null;
      refreshBans();
    } catch (e) {
      toast('error', t('Unban'), errorText(e));
    }
  }

  const menuItems = $derived<ContextItem[]>(
    menu.peer
      ? [
          { label: 'Copy address', action: () => host.copy(menu.peer!.addr) },
          { label: 'Disconnect', action: () => disconnect(menu.peer!), separator: true },
          ...BAN_TIMES.map((b, i) => ({ label: `Ban for ${b.label}`, action: () => ban(menu.peer!, b.seconds, t(b.label)), danger: true, separator: i === 0 })),
        ]
      : [],
  );
  const banItems = $derived<ContextItem[]>(
    banMenu.ban
      ? [
          { label: 'Copy IP/Netmask', action: () => host.copy(banMenu.ban!.address) },
          { label: 'Unban', action: () => unban(banMenu.ban!), separator: true },
        ]
      : [],
  );
</script>

<div class="peers" class:with-detail={peer}>
  <div class="tables">
    <div class="table-box">
      <DataTable rows={peers} {columns} key={(p) => String(p.id)} bind:selected bind:sortKey bind:sortDir label="Peers" empty="No peers connected" oncontext={(p, e) => (menu = { open: true, x: e.clientX, y: e.clientY, peer: p })}>
        {#snippet cell(p, column)}
          {#if column.key === 'age'}{shortDuration(now - p.conntime)}
          {:else if column.key === 'ping'}{pingText(p.pingtime)}
          {:else if column.key === 'sent'}{formatBytes(p.bytessent)}
          {:else if column.key === 'recv'}{formatBytes(p.bytesrecv)}
          {:else if column.key === 'direction'}<span class="dir" class:inbound={p.inbound}><Icon name={p.inbound ? 'down' : 'up'} size={13} /> {p.inbound ? 'Inbound' : 'Outbound'}</span>
          {:else}<SafeText text={String(column.value(p) ?? '')} />{/if}
        {/snippet}
      </DataTable>
    </div>
    {#if bans.length}
      <h3 class="ban-head">Banned peers</h3>
      <div class="table-box bans">
        <DataTable rows={bans} columns={banColumns} key={(b) => b.address} bind:selected={selectedBan} label="Banned peers" oncontext={(b, e) => (banMenu = { open: true, x: e.clientX, y: e.clientY, ban: b })}>
          {#snippet cell(b, column)}
            {#if column.key === 'until'}{formatTime(b.banned_until)}{:else}{b.address}{/if}
          {/snippet}
        </DataTable>
      </div>
      <div class="row">
        <button type="button" class="btn small" disabled={!selectedBan} onclick={() => { const b = bans.find((x) => x.address === selectedBan); if (b) unban(b); }}>Unban</button>
      </div>
    {/if}
  </div>
  {#if peer}
    <aside class="panel detail" aria-label="Peer details">
      <div class="panel-head">
        <div>
          <h2 class="mono break">{peer.addr}</h2>
          <p class="muted small">(peer: {peer.id}){peer.addrlocal ? ` via ${peer.addrlocal}` : ''}</p>
        </div>
        <button type="button" class="btn ghost small icon" aria-label="Hide the details" title="Hide" onclick={() => (selected = null)}><Icon name="x" size={16} /></button>
      </div>
      <dl class="facts panel-body">
        {#each peerDetails(peer, now) as row (row.label)}<dt>{t(row.label)}</dt><dd translate="no">{row.value}</dd>{/each}
      </dl>
      <div class="panel-body row actions">
        <button type="button" class="btn small" onclick={() => disconnect(peer)}>Disconnect</button>
        <button type="button" class="btn small danger" onclick={async () => { if (await confirm(t('Ban peer'), t('Ban %1 for 1 day?', peer.addr), { yes: 'Ban', danger: true })) ban(peer, 86400, t('1 day')); }}>Ban…</button>
      </div>
    </aside>
  {:else}
    <p class="muted hint">Select a peer to view detailed information. Right click a peer to disconnect or ban it.</p>
  {/if}
</div>

<ContextMenu items={menuItems} x={menu.x} y={menu.y} bind:open={menu.open} label="Peer" />
<ContextMenu items={banItems} x={banMenu.x} y={banMenu.y} bind:open={banMenu.open} label="Banned peer" />

<style>
  .peers {
    display: grid;
    grid-template-columns: minmax(0, 1fr);
    gap: 1rem;
  }
  .peers.with-detail {
    grid-template-columns: minmax(0, 1fr) minmax(20rem, 26rem);
  }
  .tables {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    min-width: 0;
  }
  .table-box {
    height: 22rem;
  }
  .table-box.bans {
    height: 10rem;
  }
  .ban-head {
    margin-top: 0.6rem;
  }
  .detail {
    max-height: 40rem;
    overflow: auto;
  }
  .detail h2 {
    font-size: 1rem;
  }
  .small {
    font-size: 0.85rem;
    margin: 0.2rem 0 0;
  }
  .detail .facts {
    grid-template-columns: max-content minmax(0, 1fr);
    font-size: 0.9rem;
  }
  .actions {
    padding-top: 0;
  }
  .dir {
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
    color: var(--accent-fg);
  }
  .dir.inbound {
    color: var(--positive);
  }
  .hint {
    font-size: 0.9rem;
  }
  @media (max-width: 900px) {
    .peers.with-detail {
      grid-template-columns: minmax(0, 1fr);
    }
  }
</style>
