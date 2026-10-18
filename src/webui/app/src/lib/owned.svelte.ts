// Which addresses the explorer shows are the current wallet's (it marks them "yours", with their labels). Only the
// addresses of what is on the page are asked, those asked in the same moment in one call of the host (wallet.owned:
// the wallet's own scripts, no scan of the chain), at most MAX_ASKED a call, and each answer kept until the wallet or
// its address book changes. Without a wallet nothing is asked and nothing is marked.
import { request } from './bridge';
import { wallets } from './state.svelte';

/** The most addresses one call asks (wallet.owned: MAX_OWNED_ASKED of walletapi.cpp). */
export const MAX_ASKED = 500;
/** The most answers kept: past it, they are asked again as they are shown. */
const MAX_KEPT = 20_000;

/** Of the wallet: its label ('' without one); null: not the wallet's. */
type Answer = string | null;

class Owned {
  /** Bumped when answers arrive: what shows the marks reads it. */
  version = $state(0);
}
const owned = new Owned();

let answers = new Map<string, Answer>();
/** The wallet and address book the answers are for. */
let answersFor = '';
const waiting = new Set<string>();
let asking = false;

function walletKey(): string {
  return wallets.current === null ? '' : `${wallets.current}\u0000${wallets.addressVersion}`;
}

async function askWaiting(): Promise<void> {
  try {
    while (waiting.size) {
      const key = answersFor;
      const wallet = wallets.current;
      const batch = [...waiting].slice(0, MAX_ASKED);
      for (const a of batch) waiting.delete(a);
      if (wallet === null) break;
      let mine: [string, string][] = [];
      try {
        mine = await request<[string, string][]>('host', 'wallet.owned', { wallet, addresses: batch });
      } catch {
        // Not known (the wallet went away): asked again when shown next.
        continue;
      }
      if (key !== answersFor) continue;
      const labels = new Map(mine);
      for (const a of batch) answers.set(a, labels.has(a) ? (labels.get(a) ?? '') : null);
      owned.version++;
    }
  } finally {
    asking = false;
  }
}

/**
 * Whether an address is the current wallet's: { label } if it is, null if it is not or not known yet (then it is
 * asked, with the others shown at the same moment). Reactive: read in a template, the mark comes with the answer.
 */
export function ownerOf(address: string | null | undefined): { label: string } | null {
  void owned.version;
  if (!address) return null;
  const key = walletKey();
  if (key !== answersFor) {
    answers = new Map();
    answersFor = key;
    waiting.clear();
  }
  if (!key) return null;
  const answer = answers.get(address);
  if (answer !== undefined) return answer === null ? null : { label: answer };
  if (answers.size >= MAX_KEPT) answers = new Map();
  if (!waiting.has(address)) {
    waiting.add(address);
    if (!asking) {
      asking = true;
      queueMicrotask(() => void askWaiting());
    }
  }
  return null;
}

/** Forget the answers (tests). */
export function resetOwned(): void {
  answers = new Map();
  answersFor = '';
  waiting.clear();
  asking = false;
}
