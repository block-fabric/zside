// The link of a sidechain's node to its mainchain node (getmainchaininfo: state, error_kind, since...),
// watched for the whole window: without it the wallet cannot sync, nor see deposits and withdrawals. A
// link down (or the mainchain node still syncing) for more than a few seconds shows a banner at the top of
// every page (components/MainchainAlert.svelte), the status bar says so, the bell keeps a notice of it
// (one per outage), and a long outage tells the desktop. When it is back: the banner goes, with one toast.
//
// The decisions are a pure function of what the node says and the time (stepLink), for the tests.
import { CHAIN, SIDECHAIN } from '../chain/config';
import { t } from './i18n.svelte';
import { addSystemNotice } from './notices.svelte';
import { host, rpc } from './rpc';
import { toast } from './state.svelte';
import type { LinkErrorKind, MainchainInfo } from './side';

/** A link down this long shows the banner (a restart of the mainchain node, a hiccup, do not). */
export const DOWN_DELAY_MS = 20_000;
/** The mainchain node syncing (or the record behind it) this long shows the softer banner. */
export const SYNC_DELAY_MS = 30_000;
/** An outage this long also tells the desktop (once). */
export const DESKTOP_DELAY_MS = 5 * 60_000;
/** How often the node is asked (a call to the node only: it does not wait for the mainchain node). */
export const LINK_POLL_MS = 4_000;

export type Banner = 'down' | 'syncing';

/** What is remembered between two looks. */
export interface LinkTracker {
  /** The bad state seen, and since when (ms). */
  bad: Banner | null;
  since: number;
  /** The banner shown, if any. */
  shown: Banner | null;
  /** An outage is going on (a banner was shown since the link was last ok): one notice of it. */
  outage: boolean;
  /** The link went down during the outage (not only syncing): the toast when it is back. */
  wasDown: boolean;
  desktopTold: boolean;
}

export type LinkEffect =
  | { type: 'notice'; banner: Banner }
  | { type: 'restored'; wasDown: boolean }
  | { type: 'desktop' };

export function newTracker(): LinkTracker {
  return { bad: null, since: 0, shown: null, outage: false, wasDown: false, desktopTold: false };
}

/** The banner a state of the link calls for, if it lasts. */
export function badOf(info: Pick<MainchainInfo, 'state' | 'connected'> | null): Banner | null {
  if (!info) return null;
  // A node of before the states: only whether it answered.
  const state = info.state ?? (info.connected ? 'ok' : 'down');
  return state === 'down' ? 'down' : state === 'syncing' ? 'syncing' : null;
}

/**
 * One look at the link: what the tracker becomes, and what to do (a notice, a toast, the desktop).
 * `info` null: the node could not be asked (busy, stopping): nothing changes.
 */
export function stepLink(tracker: LinkTracker, info: MainchainInfo | null, now: number): { tracker: LinkTracker; effects: LinkEffect[] } {
  if (info === null) return { tracker, effects: [] };
  const next = { ...tracker };
  const effects: LinkEffect[] = [];
  const bad = badOf(info);
  if (bad === null) {
    // Ok (or not asked yet): the banner goes; the toast and the notice if an outage was shown.
    if (tracker.outage) effects.push({ type: 'restored', wasDown: tracker.wasDown });
    return { tracker: newTracker(), effects };
  }
  if (bad !== tracker.bad) {
    next.bad = bad;
    // Since when, as the node says (the app may have started during the outage), else since now.
    const nodeSince = info.since ? info.since * 1000 : 0;
    next.since = nodeSince > 0 && nodeSince <= now ? nodeSince : now;
    // Back from down to syncing while the down banner shows: the mainchain node answers again.
    if (tracker.shown === 'down' && bad === 'syncing') {
      effects.push({ type: 'restored', wasDown: true });
      next.wasDown = false;
      next.desktopTold = false;
    }
  }
  const delay = bad === 'down' ? DOWN_DELAY_MS : SYNC_DELAY_MS;
  // A banner already shown changes at once (down → syncing, syncing → down after its own delay).
  if (now - next.since >= delay || (tracker.shown !== null && bad === 'syncing')) {
    if (next.shown !== bad) {
      if (!next.outage || bad === 'down') effects.push({ type: 'notice', banner: bad });
      next.shown = bad;
      next.outage = true;
    }
    if (bad === 'down') next.wasDown = true;
  }
  if (bad === 'down' && next.shown === 'down' && !next.desktopTold && now - next.since >= DESKTOP_DELAY_MS) {
    next.desktopTold = true;
    effects.push({ type: 'desktop' });
  }
  return { tracker: next, effects };
}

// --- The state of the window ---

export const mainLink = $state({
  info: null as MainchainInfo | null,
  /** The banner shown (null: none). */
  banner: null as Banner | null,
  /** Since when the link is bad (ms), for "for 3 minutes". */
  since: 0,
  /** The "How to fix" of the banner is open (the status bar opens it). */
  expanded: false,
});

let tracker = newTracker();

const mainName = () => CHAIN.mainchain?.name ?? 'the mainchain';

/**
 * What the indicators that would else say "Up to date" (the Network card of Home, the status bar) say of the
 * link: down at once (the node cannot follow the mainchain, so it cannot know it is up to date), syncing only
 * once its banner shows (the record a block behind for a moment, after each block of the mainchain, is not).
 */
export function linkHold(): Banner | null {
  if (mainLink.banner) return mainLink.banner;
  return badOf(mainLink.info) === 'down' ? 'down' : null;
}

/** Why not "Up to date" (linkHold; the pages say "Waiting for Chains" or "Chains is syncing"), for a tooltip. */
export function holdDetail(hold: Banner): string {
  return hold === 'down' ? t('This wallet cannot sync, nor confirm deposits and withdrawals, until %1 is back.', mainName()) : t('New blocks, deposits and withdrawals wait until it catches up.');
}

/** Why the link is down, in plain words (the kind the node gives). */
export function reasonText(kind: LinkErrorKind | undefined): string {
  switch (kind) {
    case 'refused':
      return t('connection refused');
    case 'unreachable':
      return t('the address cannot be reached');
    case 'auth':
      return t('it refused the user name and password');
    case 'timeout':
      return t('it does not answer in time');
    case 'wrong-network':
      return t('it is on another network');
    case 'not-synced':
      return t('it is still syncing');
    default:
      return t('it answered with an error');
  }
}

/** Apply what a look decided (the window's notices). */
export function applyEffects(effects: LinkEffect[], info: MainchainInfo | null): void {
  for (const e of effects) {
    if (e.type === 'notice') {
      addSystemNotice(
        e.banner === 'down' ? 'mainchain-down' : 'mainchain-syncing',
        e.banner === 'down' ? t('Cannot reach your %1 node', mainName()) : t('%1 is still syncing', mainName()),
        e.banner === 'down' ? t('%1 (%2): %3', mainName(), info?.node ?? '', reasonText(info?.error_kind)) : t('New blocks, deposits and withdrawals wait until it catches up.'),
      );
    } else if (e.type === 'restored') {
      if (!e.wasDown) continue;
      toast('success', t('Connected to %1 again', mainName()), t('This wallet syncs again.'), 8000);
      addSystemNotice('mainchain-restored', t('Connected to %1 again', mainName()), t('This wallet syncs again.'));
    } else if (e.type === 'desktop') {
      host.notify(t('Cannot reach your %1 node', mainName()), t('This wallet cannot sync, nor confirm deposits and withdrawals, until %1 is back.', mainName()), 'warning').catch(() => undefined);
    }
  }
}

/** One look now. */
export async function checkLink(now = Date.now()): Promise<void> {
  let info: MainchainInfo | null = null;
  try {
    const raw = await rpc.raw<MainchainInfo>('getmainchaininfo');
    info = raw && typeof raw === 'object' && typeof raw.connected === 'boolean' ? raw : null;
  } catch {
    info = null;
  }
  if (info) mainLink.info = info;
  const step = stepLink(tracker, info, now);
  tracker = step.tracker;
  mainLink.banner = tracker.shown;
  mainLink.since = tracker.since;
  if (!tracker.shown) mainLink.expanded = false;
  applyEffects(step.effects, info);
}

let timer: ReturnType<typeof setInterval> | null = null;

/** Watch the link while the window runs (a sidechain only). Not resting while hidden: the desktop is told of a long outage. */
export function startMainLink(): () => void {
  if (!SIDECHAIN || timer !== null) return () => undefined;
  void checkLink();
  timer = setInterval(() => void checkLink(), LINK_POLL_MS);
  return stopMainLink;
}

export function stopMainLink(): void {
  if (timer !== null) clearInterval(timer);
  timer = null;
  tracker = newTracker();
  mainLink.info = null;
  mainLink.banner = null;
  mainLink.since = 0;
  mainLink.expanded = false;
}
