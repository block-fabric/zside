// The first-run tips of Home in Simple mode (components/FirstRunTips.svelte): three steps of this chain,
// each with the page it opens. The steps are the chain's own (chain/firststeps.ts), so that a merge of the
// template never conflicts with them.
import type { IconName } from '../components/Icon.svelte';
import type { PageId } from './nav.svelte';

export interface FirstStep {
  title: string;
  text: string;
  icon: IconName;
  /** The button of the step: the page (or Home's side panel) it opens. */
  action?: { label: string; page: PageId; tab?: string; panel?: 'send' | 'receive' };
}

export { FIRST_STEPS } from '../chain/firststeps';
