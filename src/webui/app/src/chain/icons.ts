// Icons of this chain's own pages, beside those of the kit (components/Icon.svelte): line icons on a
// 24 grid with a 1.75 stroke, one SVG path each. The pages of zSide name them in chain/local.ts.
export const LOCAL_ICONS = {
  /** Private: a shield with a keyhole. */
  private: 'M12 3l8 3v6c0 4.5-3.4 8-8 9-4.6-1-8-4.5-8-9V6zM12 10.5a1.6 1.6 0 1 0 0 .01M12 12v3.5',
  /** Memos: a sealed envelope. */
  memo: 'M4 6h16v12H4zM4 7l8 6 8-6M9 13.5L4 18M15 13.5l5 4.5',
  /** The shielded history: a clock inside a shield. */
  zhistory: 'M12 3l8 3v6c0 4.5-3.4 8-8 9-4.6-1-8-4.5-8-9V6zM12 8.5V12l2.5 1.5',
  /** Melt & cast: coins going down into a pool and bills coming up out of it. */
  meltcast: 'M7 4v7M4.5 8.5L7 11l2.5-2.5M17 11V4M14.5 6.5L17 4l2.5 2.5M3 15c3 2 6 2 9 0s6-2 9 0M3 19c3 2 6 2 9 0s6-2 9 0',
  /** The shielded pool: a drop over waves. */
  pool: 'M12 3c2.5 3 4 5.2 4 7a4 4 0 0 1-8 0c0-1.8 1.5-4 4-7zM3 17c3 2 6 2 9 0s6-2 9 0M3 21c3 2 6 2 9 0s6-2 9 0',
  /** A viewing key: a key with an eye. */
  viewkey: 'M7.5 14a3.5 3.5 0 1 1 2.8-5.6H20V12h-2v2h-3v-2h-4.7A3.5 3.5 0 0 1 7.5 14zM7.5 10.5h.01',
} as const satisfies Record<string, string>;
