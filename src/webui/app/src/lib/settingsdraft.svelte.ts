// The changes made on the Settings page wait for Apply, or go on Cancel (the OK and Cancel of the
// options dialog of chains-qt): one stray click never takes effect.
//
// The options of the node are kept by the host: each change is written at once and what it replaced is
// kept (options.apply forgets that, options.cancel puts it back, and so does the next start when they were
// never applied). The preferences of the app (the look, the window, the wallet pages) are kept here: shown
// at once while they wait, and back as they were on Cancel.
import { t } from './i18n.svelte';
import { confirm, errorText, inform } from './dialogs.svelte';
import { host, type NodeOptions } from './rpc';
import { prefs, savePrefs, toast } from './state.svelte';
import { frame, setTitleBar } from './windowframe.svelte';

export type Prefs = Omit<typeof prefs, 'loaded'>;

export const draft = $state({
  /** Preferences of the app waiting to be applied (already shown). */
  prefs: {} as Partial<Prefs>,
  /** What those preferences were before, to come back to on Cancel. */
  original: {} as Partial<Prefs>,
  /** The options of the node as they were when the changes began (what the risky ones are measured against). */
  baseline: null as NodeOptions | null,
  /** Changes of the options of the node the host keeps for Apply, as last known (the sidebar shows them). */
  hostPending: 0,
  /**
   * The title bar chosen, waiting for Apply (null: no change). It changes the frame of the window, so it is
   * not shown before it is applied: only its box is ticked.
   */
  titleBar: null as 'integrated' | 'system' | null,
});

/** The title bar the box of the settings shows: the one waiting for Apply, or the one of the window. */
export function shownTitleBar(): 'integrated' | 'system' {
  return draft.titleBar ?? frame.titleBar;
}

/** The title bar chosen on the page: it waits for Apply, as the other settings. */
export function stageTitleBar(mode: 'integrated' | 'system'): void {
  draft.titleBar = mode === frame.titleBar ? null : mode;
}

/** The number of changes waiting: the options the host keeps, and the preferences kept here. */
export function pendingCount(options: Pick<NodeOptions, 'pending'> | null): number {
  return (options?.pending ?? 0) + Object.keys(draft.prefs).length + (draft.titleBar ? 1 : 0);
}

/** What waits for Apply, as last known, wherever the user is. */
export function pendingNow(): number {
  return draft.hostPending + Object.keys(draft.prefs).length + (draft.titleBar ? 1 : 0);
}

/** The options of the node before the first change: kept once, until applied or cancelled. */
export function noteBaseline(options: NodeOptions | null): void {
  if (options && (!draft.baseline || options.pending === 0)) draft.baseline = options;
  if (options) draft.hostPending = options.pending ?? 0;
}

/** A preference of the app changed on the page: shown at once, saved when applied. */
export function stagePrefs(changes: Partial<Prefs>): void {
  for (const [key, value] of Object.entries(changes) as [keyof Prefs, unknown][]) {
    if (!(key in draft.original)) (draft.original as Record<string, unknown>)[key] = prefs[key];
    if (sameValue(draft.original[key], value)) {
      delete draft.prefs[key];
      delete draft.original[key];
    } else {
      (draft.prefs as Record<string, unknown>)[key] = value;
    }
  }
  Object.assign(prefs, changes);
}

function sameValue(a: unknown, b: unknown): boolean {
  return a === b || JSON.stringify(a) === JSON.stringify(b);
}

function clearPrefs(): void {
  for (const k of Object.keys(draft.prefs)) delete draft.prefs[k as keyof Prefs];
  for (const k of Object.keys(draft.original)) delete draft.original[k as keyof Prefs];
}

/** The changes that are hard to take back, with what they do (asked before they are applied). */
export function riskyChanges(before: NodeOptions | null, now: NodeOptions | null): string[] {
  if (!before || !now) return [];
  const out: string[] = [];
  if (now.prune && !before.prune) {
    out.push(`Prune block storage to ${now.pruneSize} GB: at the next start the node deletes the oldest blocks. Reverting this setting requires re-downloading the entire blockchain.`);
  } else if (now.prune && before.prune && now.pruneSize < before.pruneSize) {
    out.push(`Prune block storage to ${now.pruneSize} GB instead of ${before.pruneSize} GB: more of the oldest blocks are deleted at the next start, and only downloading them again brings them back.`);
  }
  if (now.dbcache > Math.max(4096, before.dbcache)) out.push(`A database cache of ${now.dbcache} MiB: the node may use that much more memory.`);
  const proxy = (o: NodeOptions) => `${o.proxyUse}:${o.proxyIp}:${o.proxyPort}:${o.torUse}:${o.torIp}:${o.torPort}`;
  if (proxy(now) !== proxy(before)) out.push('The proxy settings: every connection of the node to its peers goes out another way. A wrong proxy leaves the node without peers.');
  if (!now.server && before.server) out.push('The RPC server off: command-line tools, and the sidechain nodes run from here, can no longer reach this node.');
  if (!now.listen && before.listen) out.push('Incoming connections off: other nodes can no longer connect to this one.');
  return out;
}

/**
 * Apply the changes, once the risky ones are confirmed. Resolves with the options as the host has them
 * after, or null when the user did not confirm (the changes still wait then).
 */
export async function applyChanges(now: NodeOptions | null): Promise<NodeOptions | null> {
  const risky = riskyChanges(draft.baseline, now);
  if (risky.length > 0) {
    const ok = await confirm(t('Confirm options'), t('Some of these changes are hard to take back. Apply them?'), {
      yes: 'Apply',
      level: 'warning',
      informative: risky.map((r) => `• ${r}`).join('\n'),
    });
    if (!ok) return null;
  }
  let options = now;
  try {
    if ((now?.pending ?? 0) > 0) options = await host.applyOptions();
  } catch (e) {
    await inform(t('Settings'), errorText(e), 'error');
    return null;
  }
  if (Object.keys(draft.prefs).length > 0) savePrefs({ ...draft.prefs });
  clearPrefs();
  if (draft.titleBar) {
    const mode = draft.titleBar;
    try {
      await setTitleBar(mode);
      draft.titleBar = null;
    } catch (e) {
      await inform(t('Title bar'), errorText(e), 'error');
    }
  }
  draft.baseline = options;
  draft.hostPending = options?.pending ?? 0;
  return options;
}

/** Throw the changes away: the options of the node and the look come back as they were. */
export async function cancelChanges(now: NodeOptions | null): Promise<NodeOptions | null> {
  Object.assign(prefs, draft.original);
  clearPrefs();
  draft.titleBar = null;
  let options = now;
  try {
    if ((now?.pending ?? 0) > 0) options = await host.cancelOptions();
  } catch (e) {
    await inform(t('Settings'), errorText(e), 'error');
  }
  draft.baseline = options;
  draft.hostPending = options?.pending ?? 0;
  return options;
}

/**
 * The user leaves the Settings page with changes waiting: a notice offers to apply or discard them (a
 * notice, not a dialog: the page the user goes to stays usable). They wait meanwhile, and what is never
 * applied is taken back when the app stops.
 */
export function askOnLeave(now: NodeOptions | null): void {
  const count = pendingCount(now);
  if (count === 0) return;
  toast('warning', t('Settings not applied'), t('%1 for Apply. Changes not applied never take effect: they are taken back at the next start.', count === 1 ? t('1 change waits') : t('%1 changes wait', count)), 0, [
    {
      label: 'Apply',
      run: async () => {
        const options = await host.options().catch(() => now);
        await applyChanges(options);
      },
    },
    {
      label: 'Discard',
      run: async () => {
        const options = await host.options().catch(() => now);
        await cancelChanges(options);
      },
    },
  ]);
}
