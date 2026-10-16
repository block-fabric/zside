// The three first steps of this chain, shown on Home in Simple mode until dismissed
// (components/FirstRunTips.svelte; lib/firststeps.ts). English here: the card translates them. A chain made
// from the template says its own here: what a beginner does first with it.
import type { FirstStep } from '../lib/firststeps';
import { CHAIN } from './config';

const main = CHAIN.mainchain?.name ?? 'the mainchain';

export const FIRST_STEPS: readonly FirstStep[] = [
  { title: 'Bring coins here', text: `Coins come from ${main}. Drivechains gives a deposit address: paste it in the ${main} app (Sidechains, Move coins). They arrive here a block or two later.`, icon: 'down', action: { label: 'Get a deposit address', page: 'peg', tab: 'deposit' } },
  { title: 'Send and receive', text: 'Pay any address of this chain, or give yours to be paid. Payments here are confirmed with the next block.', icon: 'send', action: { label: 'Receive', page: 'receive', panel: 'receive' } },
  { title: 'Take coins back', text: `When you are done, Drivechains sends coins back to ${main}. That takes longer: the miners of ${main} approve withdrawals together.`, icon: 'up', action: { label: 'Send coins back', page: 'peg', tab: 'withdraw' } },
];
