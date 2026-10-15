// The drivechain state of Chains (BIP300/301) as the RPC methods give it, and what the sidechain
// pages show of it: slots, proposals and their acks, escrows and deposits, withdrawal bundles and
// their votes.
import { t } from './i18n.svelte';
import { rpc } from './rpc';

export interface DrivechainInfo {
  maxsidechains: number;
  activationperiod: number;
  activationmaxfailures: number;
  replacementperiod: number;
  withdrawalperiod: number;
  withdrawalminscore: number;
  maxpendingbundles: number;
  upvoteexpiryblocks: number;
  height: number;
  activesidechains: number;
  proposals: number;
  pendingbundles: number;
  escrowtotal: number;
  defaultwithdrawalvote: string;
}

export interface SidechainBase {
  slot: number;
  title: string;
  description: string;
  version: number;
  hashid1: string;
  hashid2: string;
  proposalhash: string;
}

export interface ActiveSidechain extends SidechainBase {
  activationheight: number;
  escrowscript: string;
  escrow?: { txid: string; n: number; amount: number };
  pendingbundles: number;
}

export interface PendingProposal extends SidechainBase {
  height: number;
  age: number;
  acks: number;
  failures: number;
  blocksleft: number;
  replacement: boolean;
  ack: boolean;
}

export interface Proposals {
  pending: PendingProposal[];
  queued: SidechainBase[];
}

export interface Bundle {
  slot: number;
  hash: string;
  score: number;
  lastupvote?: number;
  blocksleft: number;
  payable: boolean;
  known: boolean;
  vote: string;
  payouts?: { amount: number; script: string; address?: string }[];
  fee?: number;
}

export interface Deposit {
  slot: number;
  destination: string;
  amount: number;
  total: number;
  txid: string;
  burnindex: number;
  txindex: number;
  blockhash: string;
  confirmations: number;
}

export interface DrivechainState {
  info: DrivechainInfo;
  sidechains: ActiveSidechain[];
  proposals: Proposals;
  bundles: Bundle[];
}

/**
 * The active sidechains and the bundles of one, for the detail panel of a sidechain (the mainchain's: a sidechain
 * has no sidechains of its own, and its node does not take these methods).
 */
export async function loadSidechainDetail<S, B>(slot: number): Promise<[S[], B[]]> {
  return Promise.all([rpc.raw<S[]>('listactivesidechains'), rpc.raw<B[]>('listwithdrawalbundles', [slot])]);
}

/** Everything the sidechain page shows, fetched at once (SidechainPage::refresh). */
export async function loadDrivechain(): Promise<DrivechainState> {
  const [info, sidechains, proposals, bundles] = await Promise.all([
    rpc.raw<DrivechainInfo>('getdrivechaininfo'),
    rpc.raw<ActiveSidechain[]>('listactivesidechains'),
    rpc.raw<Proposals>('listsidechainproposals'),
    rpc.raw<Bundle[]>('listwithdrawalbundles'),
  ]);
  return { info, sidechains, proposals, bundles };
}

export type SlotState = 'active' | 'proposed' | 'queued' | 'free';

export interface SlotView {
  slot: number;
  state: SlotState;
  title: string;
  /** A proposal waits to replace the sidechain of the slot. */
  replacing: boolean;
}

/** What each slot holds: an active sidechain, a proposal (in the chain or queued here), or nothing. */
export function slotOverview(state: Pick<DrivechainState, 'info' | 'sidechains' | 'proposals'>): SlotView[] {
  const slots: SlotView[] = [];
  for (let slot = 0; slot < state.info.maxsidechains; slot++) slots.push({ slot, state: 'free', title: '', replacing: false });
  for (const q of state.proposals.queued) {
    if (slots[q.slot]) slots[q.slot] = { slot: q.slot, state: 'queued', title: q.title, replacing: false };
  }
  for (const p of state.proposals.pending) {
    if (slots[p.slot]) slots[p.slot] = { slot: p.slot, state: 'proposed', title: p.title, replacing: false };
  }
  for (const s of state.sidechains) {
    if (!slots[s.slot]) continue;
    const replacing = state.proposals.pending.some((p) => p.slot === s.slot);
    slots[s.slot] = { slot: s.slot, state: 'active', title: s.title, replacing };
  }
  return slots;
}

/** How far a proposal is: its acks out of the blocks it needs, in percent (0–100). */
export function proposalProgress(p: PendingProposal, info: DrivechainInfo): number {
  const needed = p.replacement ? info.replacementperiod : info.activationperiod;
  return needed > 0 ? Math.min(100, (p.acks / needed) * 100) : 0;
}

/** The blocks a proposal needs in all: the activation period, or the longer one of a replacement. */
export function proposalNeeds(p: PendingProposal, info: DrivechainInfo): number {
  return p.replacement ? info.replacementperiod : info.activationperiod;
}

/** How far a bundle is: its score out of the minimum score, in percent (0–100). */
export function bundleProgress(b: Bundle, info: DrivechainInfo): number {
  return info.withdrawalminscore > 0 ? Math.max(0, Math.min(100, (b.score / info.withdrawalminscore) * 100)) : 0;
}

/** Whether a bundle can still make it: enough blocks left to reach the minimum score. */
export function bundleCanPass(b: Bundle, info: DrivechainInfo): boolean {
  return b.payable || b.score + b.blocksleft >= info.withdrawalminscore;
}

/** The summary line of the page (SidechainPage::refresh), with the escrow already formatted. */
export function summaryText(info: DrivechainInfo, escrow: string): string {
  return t(
    '%1 of %2 sidechain slots in use, holding %3 in escrow. A proposal needs %4 blocks to activate and is rejected after %5 blocks without an ack. A withdrawal has %6 blocks to reach a score of %7.',
    info.activesidechains,
    info.maxsidechains,
    escrow,
    info.activationperiod,
    info.activationmaxfailures,
    info.withdrawalperiod,
    info.withdrawalminscore,
  );
}

/** What the vote of this node on a bundle means, in words. */
export function voteText(vote: string): string {
  switch (vote) {
    case 'upvote':
      return t('Upvote');
    case 'downvote':
      return t('Downvote');
    case 'abstain':
      return t('Abstain');
    case 'follow':
      return t('Follow');
    default:
      return vote;
  }
}

export const HEX64 = /^[0-9a-fA-F]{64}$/;
export const HEX40 = /^[0-9a-fA-F]{40}$/;

/** The errors of the proposal form, before it goes to createsidechainproposal (empty when none). */
export function proposalErrors(form: { slot: number; title: string; hash1: string; hash2: string }, maxSlots: number): string[] {
  const errors: string[] = [];
  if (!Number.isInteger(form.slot) || form.slot < 0 || form.slot >= maxSlots) errors.push(`The slot is a number from 0 to ${maxSlots - 1}.`);
  if (!form.title.trim()) errors.push('A proposal needs a title.');
  if (form.hash1.trim() && !HEX64.test(form.hash1.trim())) errors.push('The release tarball hash is 64 hexadecimal characters.');
  if (form.hash2.trim() && !HEX40.test(form.hash2.trim())) errors.push('The build commit hash is 40 hexadecimal characters.');
  return errors;
}

/**
 * Blind merged mining in the blocks from..from+count-1 of this chain: the sidechains (slots) each block
 * committed to, by height (the badges of the explorer's ribbon).
 */
export async function bmmCommitments(from: number, count: number): Promise<Record<number, number[]>> {
  const sidechains = await rpc.raw<{ slot: number }[]>('listactivesidechains', []);
  const at: Record<number, number[]> = {};
  for (const sc of sidechains) {
    const events = await rpc.raw<{ height: number; bmm?: string }[]>('getsidechainevents', [sc.slot, from, count]);
    for (const e of events) if (e.bmm) (at[e.height] ??= []).push(sc.slot);
  }
  return at;
}
