// What the intro says of the data directory chosen (intro.cpp, freespacechecker.cpp of chains-qt).
import { t } from './i18n.svelte';
import type { Space } from './rpc';

/** FreespaceChecker::check: what the path is, and whether the app can start with it. */
export function pathMessage(space: Space | null): { text: string; error: boolean } | null {
  if (!space) return null;
  if (space.error) return { text: t('Cannot create data directory here.'), error: true };
  if (space.exists && !space.isDirectory) return { text: t('Path already exists, and is not a directory.'), error: true };
  if (space.exists) return { text: t('Directory already exists. Add %1 if you intend to create a new directory here.', '/name'), error: false };
  return { text: t('A new data directory will be created.'), error: false };
}

/**
 * Intro::UpdatePruneLabels: how old a backup the blocks kept can restore, in days (blocks of 2.25 MB
 * with their undo data, one every 10 minutes, as chains-qt counts before the chain is known).
 */
export function backupDays(gb: number): number {
  return Math.floor((gb * 1e9) / ((2_250_000 * 86_400) / 600));
}
