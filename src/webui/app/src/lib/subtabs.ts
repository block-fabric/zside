// The tabs of a page shown inside another one's tab (Tools, Advanced transactions): the page that holds
// it shows them on its own tab row, after the tab and an arrow (user decision, 2026-10-15), and the
// page inside hides its own row. One list for both.
import { ownTab, type PageId } from './nav.svelte';

export interface SubTab {
  id: string;
  label: string;
}
export interface SubTabs {
  /** For screen readers: the name of the row of sub-tabs. */
  label: string;
  tabs: SubTab[];
  /** The one shown when none is chosen. */
  first: string;
}

export const SUBTABS: Partial<Record<PageId, SubTabs>> = {
  crypto: {
    label: 'Crypto tools',
    tabs: [
      { id: 'hash', label: 'Hash Calculator' },
      { id: 'merkle', label: 'Merkle Tree' },
      { id: 'address', label: 'Address Decoder' },
    ],
    first: 'hash',
  },
  proof: {
    label: 'Prove or verify',
    tabs: [
      { id: 'prove', label: 'Prove' },
      { id: 'verify', label: 'Verify' },
    ],
    first: 'prove',
  },
  messages: {
    label: 'Sign or verify',
    tabs: [
      { id: 'sign', label: 'Sign Message' },
      { id: 'verify', label: 'Verify Message' },
    ],
    first: 'sign',
  },
  multisig: {
    label: 'Multisig lounge',
    tabs: [
      { id: 'keys', label: 'Keys' },
      { id: 'addresses', label: 'Shared addresses' },
      { id: 'spend', label: 'Spend' },
    ],
    first: 'keys',
  },
};

/** The sub-tab of a page shown inside another: its own tab, or the first (an address after ":" aside). */
export function subTabOf(page: PageId): string {
  const sub = SUBTABS[page];
  if (!sub) return '';
  const own = ownTab(page).split(':')[0];
  return sub.tabs.some((tb) => tb.id === own) ? own : sub.first;
}
