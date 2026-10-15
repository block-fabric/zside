// What the status bar says about the sync of the node (BitcoinGUI::setNumBlocks).
import { niceTimeOffset } from './format';
import { t, tn } from './i18n.svelte';

/** A tip older than this is "behind" (MAX_BLOCK_TIME_GAP of chains-qt). */
export const MAX_BLOCK_TIME_GAP = 90 * 60;
/** Headers further behind than this many blocks are being synced (HEADER_HEIGHT_DELTA_SYNC). */
export const HEADER_HEIGHT_DELTA_SYNC = 24;

export interface SyncInput {
  tipHeight: number;
  tipTime: number;
  progress: number;
  loadingBlocks: boolean;
  headerHeight: number;
  headerTime: number;
  presyncHeight?: number;
  presyncTime?: number;
  connections: number;
  networkActive: boolean;
  /** Seconds between blocks on this network. */
  spacing: number;
  /** Now, in seconds. */
  now: number;
}

export interface SyncStatus {
  upToDate: boolean;
  /** What the node does: "Synchronizing with network…". Empty when up to date. */
  label: string;
  /** "3 hours behind", when behind (in the language shown, as label and detail). */
  behind: string;
  /** Verification progress, 0 to 1. */
  progress: number;
  /** Longer text for a tooltip. */
  detail: string;
}

function percent(height: number, left: number): string {
  return ((100 * height) / (height + left)).toFixed(1);
}

export function syncStatus(s: SyncInput): SyncStatus {
  const secs = s.now - s.tipTime;
  // In the language shown: every message here is one of the app, with its own placeholders.
  const processed = tn('Processed %n block(s) of transaction history.', s.tipHeight);
  if (secs < MAX_BLOCK_TIME_GAP) {
    return { upToDate: true, label: '', behind: '', progress: 1, detail: `${t('Up to date')}. ${processed}` };
  }
  let label: string;
  if (s.loadingBlocks) {
    // While a reindex reads the block files, the headers run ahead of the blocks (BlockSource::DISK, a
    // header tip): "Indexing"; then the blocks are connected: "Processing".
    label = s.headerHeight > s.tipHeight ? t('Indexing blocks on disk…') : t('Processing blocks on disk…');
  } else if (s.connections > 0 && s.networkActive) {
    label = t('Synchronizing with network…');
    const headersLeft = Math.floor((s.now - s.headerTime) / s.spacing);
    if (s.presyncHeight !== undefined && s.presyncTime !== undefined && s.presyncHeight > s.headerHeight) {
      const left = Math.floor((s.now - s.presyncTime) / s.spacing);
      if (left > HEADER_HEIGHT_DELTA_SYNC) label = t('Pre-syncing Headers (%1%)…', percent(s.presyncHeight, left));
    } else if (headersLeft > HEADER_HEIGHT_DELTA_SYNC) {
      label = t('Syncing Headers (%1%)…', percent(s.headerHeight, headersLeft));
    }
  } else if (!s.networkActive) {
    label = t('Network activity disabled');
  } else {
    label = t('Connecting to peers…');
  }
  const behind = t('%1 behind', niceTimeOffset(secs));
  return {
    upToDate: false,
    label,
    behind,
    progress: Math.max(0, Math.min(1, s.progress)),
    detail: [t('Catching up…'), processed, t('Last received block was generated %1 ago.', niceTimeOffset(secs)), t('Transactions after this will not yet be visible.')].join(' '),
  };
}
