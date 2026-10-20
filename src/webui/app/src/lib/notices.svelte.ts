// The notices of the bell in the top bars: the new transactions of the wallets since the app started
// (wallets.newTxs), and notices of the app itself (the link to the mainchain node: lib/mainlink.svelte.ts);
// and which of them were seen.
import { wallets } from './state.svelte';

/** A notice of the app (not a transaction), newest last. */
export interface SystemNotice {
  id: number;
  kind: 'mainchain-down' | 'mainchain-syncing' | 'mainchain-restored';
  /** Words of the app, already in the language shown. */
  title: string;
  text: string;
  /** Unix seconds. */
  time: number;
  unread: boolean;
}

export const notices = $state({ read: [] as string[], system: [] as SystemNotice[] });

const SYSTEM_MAX = 50;
let nextId = 1;

/** Keep a notice of the app in the bell. */
export function addSystemNotice(kind: SystemNotice['kind'], title: string, text: string, time = Math.floor(Date.now() / 1000)): void {
  notices.system = [...notices.system, { id: nextId++, kind, title, text, time, unread: true }].slice(-SYSTEM_MAX);
}

/** The notices of the app, newest first. */
export function systemList(): SystemNotice[] {
  return [...notices.system].reverse();
}

const key = (n: { wallet: string; txid: string }) => `${n.wallet}:${n.txid}`;

/** The notices, newest first, with whether each was seen. */
export function noticeList(): { wallet: string; txid: string; unread: boolean }[] {
  const read = new Set(notices.read);
  const seen = new Set<string>();
  const out: { wallet: string; txid: string; unread: boolean }[] = [];
  for (let i = wallets.newTxs.length - 1; i >= 0; i--) {
    const n = wallets.newTxs[i];
    if (seen.has(key(n))) continue;
    seen.add(key(n));
    out.push({ ...n, unread: !read.has(key(n)) });
  }
  return out;
}

export function unreadCount(): number {
  return noticeList().filter((n) => n.unread).length + notices.system.filter((n) => n.unread).length;
}

export function markAllRead(): void {
  notices.read = wallets.newTxs.map(key).slice(-200);
  if (notices.system.some((n) => n.unread)) notices.system = notices.system.map((n) => ({ ...n, unread: false }));
}
