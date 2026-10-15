// The notices of the bell in the top bars: the new transactions of the wallets since the app
// started (wallets.newTxs), and which of them were seen.
import { wallets } from './state.svelte';

export const notices = $state({ read: [] as string[] });

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
  return noticeList().filter((n) => n.unread).length;
}

export function markAllRead(): void {
  notices.read = wallets.newTxs.map(key).slice(-200);
}
