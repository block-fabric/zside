// The three first steps of zSide, shown on Home in Simple mode until dismissed (components/FirstRunTips.svelte;
// lib/firststeps.ts). English here: the card translates them.
import type { FirstStep } from '../lib/firststeps';
import { CHAIN } from './config';

const main = CHAIN.mainchain?.name ?? 'the mainchain';

export const FIRST_STEPS: readonly FirstStep[] = [
  { title: 'Bring coins here', text: `Coins come from ${main}. Drivechains gives a deposit address: paste it in the ${main} app (Sidechains, Move coins). They arrive here, public, a block or two later.`, icon: 'down', action: { label: 'Get a deposit address', page: 'peg', tab: 'deposit' } },
  { title: 'Make them private', text: 'Move coins into your private balance: after that, nobody can see how much you hold or whom you pay.', icon: 'shield', action: { label: 'Make private', page: 'private', tab: 'move' } },
  { title: 'Send privately', text: 'On the Send page, turn on “Send privately”: only the recipient sees the amount and your message.', icon: 'send', action: { label: 'Send', page: 'send' } },
];
