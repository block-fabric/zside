// The detail panel: what a page or a card selected (a transaction, a block, a peer, a sidechain),
// shown at the right of the page instead of a dialog. A page publishes its selection with
// showDetail when detail.follow is on (the workspace layout); the cards of the dashboard always do.
export type DetailItem =
  | { kind: 'tx'; wallet: string; txid: string; idx: number; address: string }
  | { kind: 'block'; hash: string }
  | { kind: 'peer'; id: number }
  | { kind: 'sidechain'; slot: number };

export const detail = $state({
  item: null as DetailItem | null,
  /** Pages show what is selected in the panel (the workspace layout); otherwise their dialogs. */
  follow: false,
});

function same(a: DetailItem | null, b: DetailItem): boolean {
  return !!a && JSON.stringify(a) === JSON.stringify(b);
}

export function showDetail(item: DetailItem): void {
  if (!same(detail.item, item)) detail.item = item;
}

export function closeDetail(): void {
  detail.item = null;
}
