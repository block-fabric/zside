// Peers and bans of the node window (PeerTableModel, BanTableModel and RPCConsole::updateDetailWidget
// of chains-qt), from getpeerinfo and listbanned.
import { formatBytes, shortDuration } from './format';
import { t } from './i18n.svelte';

export interface PeerInfo {
  id: number;
  addr: string;
  addrlocal?: string;
  network: string;
  mapped_as?: number;
  services: string;
  servicesnames?: string[];
  relaytxes: boolean;
  lastsend: number;
  lastrecv: number;
  last_transaction: number;
  last_block: number;
  bytessent: number;
  bytesrecv: number;
  conntime: number;
  timeoffset: number;
  pingtime?: number;
  minping?: number;
  pingwait?: number;
  version: number;
  subver: string;
  inbound: boolean;
  bip152_hb_to: boolean;
  bip152_hb_from: boolean;
  startingheight?: number;
  presynced_headers?: number;
  synced_headers: number;
  synced_blocks: number;
  connection_type: string;
  transport_protocol_type?: string;
  session_id?: string;
  permissions?: string[];
  addr_relay_enabled?: boolean;
  addr_processed?: number;
  addr_rate_limited?: number;
}

export interface BanEntry {
  address: string;
  ban_created: number;
  banned_until: number;
  ban_duration: number;
  time_remaining: number;
}

const NETWORKS: Record<string, string> = { ipv4: 'IPv4', ipv6: 'IPv6', onion: 'Onion', i2p: 'I2P', cjdns: 'CJDNS', not_publicly_routable: 'Unroutable', internal: 'Internal' };

/** The network of a peer as chains-qt names it (GUIUtil::NetworkToQString). */
export function networkName(network: string): string {
  return NETWORKS[network] ?? network;
}

const TYPES: Record<string, string> = {
  'outbound-full-relay': 'Full Relay',
  'block-relay-only': 'Block Relay',
  manual: 'Manual',
  feeler: 'Feeler',
  'addr-fetch': 'Address Fetch',
  inbound: '',
};

/** The type of a connection (GUIUtil::ConnectionTypeToQString), with its direction when asked. */
export function connectionType(type: string, inbound: boolean, withDirection = false): string {
  const name = TYPES[type] ?? type;
  if (!withDirection) return name || 'Inbound';
  const direction = inbound ? 'Inbound' : 'Outbound';
  return name ? `${direction} ${name}` : direction;
}

/** A ping time (GUIUtil::formatPingTime): "N/A" when there is none. */
export function pingText(seconds: number | undefined): string {
  if (seconds === undefined || seconds <= 0 || !Number.isFinite(seconds)) return t('N/A');
  return t('%1 ms', Math.round(seconds * 1000));
}

/** How long ago something happened, "Never" if it did not (RPCConsole TimeDurationField). */
export function sinceText(now: number, time: number): string {
  return time ? shortDuration(now - time) : 'Never';
}

export interface DetailRow {
  label: string;
  value: string;
}

/** The detail panel of a peer, in the order of chains-qt. */
export function peerDetails(p: PeerInfo, now: number): DetailRow[] {
  const hb = [p.bip152_hb_to ? 'To' : '', p.bip152_hb_from ? 'From' : ''].filter(Boolean).join('/') || 'No';
  const rows: DetailRow[] = [
    { label: 'Whitelisted', value: p.permissions && p.permissions.length ? p.permissions.join(' & ') : 'N/A' },
    { label: 'Direction/Type', value: connectionType(p.connection_type, p.inbound, true) },
    { label: 'Transport', value: p.transport_protocol_type ?? 'N/A' },
  ];
  if (p.transport_protocol_type === 'v2' && p.session_id) rows.push({ label: 'Session ID', value: p.session_id });
  rows.push(
    { label: 'Network', value: networkName(p.network) },
    { label: 'Version', value: p.version ? String(p.version) : 'N/A' },
    { label: 'User Agent', value: p.subver || 'N/A' },
    { label: 'Services', value: p.servicesnames && p.servicesnames.length ? p.servicesnames.map((s) => s.charAt(0) + s.slice(1).toLowerCase().replace(/_/g, ' ')).join(' & ') : 'None' },
    { label: 'Transaction Relay', value: p.relaytxes ? 'Yes' : 'No' },
    { label: 'High Bandwidth', value: hb },
    { label: 'Starting Block', value: p.startingheight !== undefined && p.startingheight >= 0 ? String(p.startingheight) : 'Unknown' },
    { label: 'Synced Headers', value: p.synced_headers > -1 ? String(p.synced_headers) : 'Unknown' },
    { label: 'Synced Blocks', value: p.synced_blocks > -1 ? String(p.synced_blocks) : 'Unknown' },
    { label: 'Address Relay', value: p.addr_relay_enabled ? 'Yes' : 'No' },
    { label: 'Addresses Processed', value: String(p.addr_processed ?? 0) },
    { label: 'Addresses Rate-Limited', value: String(p.addr_rate_limited ?? 0) },
    { label: 'Mapped AS', value: p.mapped_as ? String(p.mapped_as) : 'N/A' },
    { label: 'Connection Time', value: shortDuration(now - p.conntime) },
    { label: 'Last Block', value: sinceText(now, p.last_block) },
    { label: 'Last Transaction', value: sinceText(now, p.last_transaction) },
    { label: 'Last Send', value: sinceText(now, p.lastsend) },
    { label: 'Last Receive', value: sinceText(now, p.lastrecv) },
    { label: 'Sent', value: formatBytes(p.bytessent) },
    { label: 'Received', value: formatBytes(p.bytesrecv) },
    { label: 'Ping Time', value: pingText(p.pingtime) },
    { label: 'Min Ping', value: pingText(p.minping) },
    { label: 'Ping Wait', value: pingText(p.pingwait) },
    { label: 'Time Offset', value: `${p.timeoffset} seconds` },
  );
  return rows;
}

/** Ban times of the context menu of a peer (RPCConsole: 1 hour, 1 day, 1 week, 1 year). */
export const BAN_TIMES = [
  { label: '1 hour', seconds: 60 * 60 },
  { label: '1 day', seconds: 60 * 60 * 24 },
  { label: '1 week', seconds: 60 * 60 * 24 * 7 },
  { label: '1 year', seconds: 60 * 60 * 24 * 365 },
] as const;
