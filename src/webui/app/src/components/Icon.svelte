<script lang="ts" module>
  import { LOCAL_ICONS } from '../chain/icons';
  // Line icons drawn for chains-app, on a 24 grid with a 1.75 stroke.
  export const ICONS = {
    overview: 'M4 13h6V4H4zM14 20h6v-9h-6zM4 20h6v-3H4zM14 7h6V4h-6z',
    send: 'M4 12h13M12 5l7 7-7 7',
    receive: 'M20 12H7M12 19l-7-7 7-7',
    history: 'M4 12a8 8 0 1 0 2.3-5.6M4 4v4h4M12 8v4l3 2',
    sidechains: 'M7 7a3 3 0 1 0 0 .01M17 17a3 3 0 1 0 0 .01M17 7a3 3 0 1 0 0 .01M9.6 8.5l4.8 7M10 7h4',
    nodes: 'M4 6h16v4H4zM4 14h16v4H4zM7 8h.01M7 16h.01',
    // A sidechain: the peg (coins down to it, up back to the mainchain), merged mining (its blocks
    // committed to in the blocks of the mainchain).
    peg: 'M8 4v14M4.5 14.5L8 18l3.5-3.5M16 20V6M12.5 9.5L16 6l3.5 3.5',
    merged: 'M3 6h7v5H3zM14 13h7v5h-7zM10 8.5h2.5a1.5 1.5 0 0 1 1.5 1.5v5.5',
    mine: 'M14.5 4.5l5 5M4 20l9-9M12 6l6 6-2 2-6-6z',
    explorer: 'M10.5 17a6.5 6.5 0 1 0 0-13 6.5 6.5 0 0 0 0 13zM15.5 15.5L20 20',
    terminal: 'M4 5h16v14H4zM7 9l3 3-3 3M12 15h5',
    settings: 'M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM19 12l2-1-1-3-2 .3-1.5-1.5L17 5l-3-1-1 2h-2l-1-2-3 1 .5 2L6 8.3 4 8l-1 3 2 1v0l-2 1 1 3 2-.3L7.5 17 7 19l3 1 1-2h2l1 2 3-1-.5-2 1.5-1.5 2 .3 1-3z',
    copy: 'M9 9h11v11H9zM5 15V4h11',
    check: 'M5 12.5l4.5 4.5L19 7',
    x: 'M6 6l12 12M18 6L6 18',
    chevron: 'M6 9l6 6 6-6',
    'chevron-right': 'M9 6l6 6-6 6',
    lock: 'M6 11h12v9H6zM8.5 11V8a3.5 3.5 0 0 1 7 0v3',
    unlock: 'M6 11h12v9H6zM8.5 11V8a3.5 3.5 0 0 1 6.8-1.2',
    peers: 'M8 11a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM3 19c.5-3 2.5-5 5-5s4.5 2 5 5M16 11a3 3 0 1 0 0-6M17 14c2.2.4 3.6 2.3 4 5',
    offline: 'M3 3l18 18M8.5 16.5a5 5 0 0 1 7 0M5 13a10 10 0 0 1 4-2.5M19 13a10 10 0 0 0-3-2M2 9a15 15 0 0 1 4.5-3M22 9a15 15 0 0 0-11-4',
    paste: 'M9 4h6v3H9zM7 5H5v15h14V5h-2',
    qr: 'M4 4h6v6H4zM14 4h6v6h-6zM4 14h6v6H4zM14 14h2v2h-2zM18 18h2v2h-2zM14 18h2M18 14h2',
    search: 'M10.5 17a6.5 6.5 0 1 0 0-13 6.5 6.5 0 0 0 0 13zM15.5 15.5L20 20',
    up: 'M12 19V5M6 11l6-6 6 6',
    down: 'M12 5v14M6 13l6 6 6-6',
    alert: 'M12 4l9 16H3zM12 10v4M12 17h.01',
    info: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM12 11v6M12 8h.01',
    eye: 'M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12zM12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6z',
    eyeoff: 'M3 3l18 18M10.6 5.1A10 10 0 0 1 12 5c6.5 0 10 7 10 7a17 17 0 0 1-3.2 4.2M6.6 6.6A17 17 0 0 0 2 12s3.5 7 10 7a9.6 9.6 0 0 0 5.4-1.6M9.9 9.9a3 3 0 0 0 4.2 4.2',
    menu: 'M4 7h16M4 12h16M4 17h16',
    plus: 'M12 5v14M5 12h14',
    wallet: 'M4 7h15v12H4zM4 7l11-3v3M15 13h.01',
    shield: 'M12 3l8 3v6c0 4.5-3.4 8-8 9-4.6-1-8-4.5-8-9V6z',
    block: 'M12 3l8 4.5v9L12 21l-8-4.5v-9zM4 7.5l8 4.5 8-4.5M12 12v9',
    power: 'M12 3v9M6.3 6.3a8 8 0 1 0 11.4 0',
    folder: 'M3 6h6l2 2h10v11H3z',
    text: 'M5 6h14M5 12h14M5 18h9',
    book: 'M5 4h11a3 3 0 0 1 3 3v13H8a3 3 0 0 1-3-3zM5 17a3 3 0 0 1 3-3h11M9 8h6',
    trash: 'M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3',
    download: 'M12 4v11M7 10l5 5 5-5M5 20h14',
    upload: 'M12 20V9M7 14l5-5 5 5M5 4h14',
    edit: 'M4 20h4L19 9l-4-4L4 16zM13.5 6.5l4 4',
    more: 'M5 12h.01M12 12h.01M19 12h.01',
    coins: 'M8 9a5 2.5 0 1 0 0-.01M3 9v4c0 1.4 2.2 2.5 5 2.5s5-1.1 5-2.5V9M11 13.5c.9.6 2.3 1 4 1 2.8 0 5-1.1 5-2.5v-4M16 7.6c2.3.2 4 1.2 4 2.4',
    key: 'M8 15a4 4 0 1 1 3-6.6L20 8.4V12h-2v2h-3l-1.3-1.3A4 4 0 0 1 8 15z',
    refresh: 'M20 11a8 8 0 0 0-14.3-4.3L4 9M4 4v5h5M4 13a8 8 0 0 0 14.3 4.3L20 15M20 20v-5h-5',
    home: 'M4 11l8-7 8 7M6 9.5V20h12V9.5M10 20v-5h4v5',
    tabs: 'M4 5h16v14H4zM4 9h16M9 5v4',
    dashboard: 'M4 4h7v9H4zM13 4h7v5h-7zM13 11h7v9h-7zM4 15h7v5H4z',
    sidebar: 'M4 5h16v14H4zM9 5v14',
    panel: 'M4 5h16v14H4zM15 5v14',
    clock: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM12 7v5l3 2',
    hash: 'M5 9h14M5 15h14M10 4L8 20M16 4l-2 16',
    tool: 'M14.7 6.3a4 4 0 0 0-5.4 5.4L4 17l3 3 5.3-5.3a4 4 0 0 0 5.4-5.4l-2.5 2.5-2.4-.6-.6-2.4z',
    arrow: 'M5 12h14M13 6l6 6-6 6',
    bell: 'M6 16V11a6 6 0 0 1 12 0v5l1.5 2h-15zM10 20.5a2 2 0 0 0 4 0',
    moon: 'M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5z',
    logout: 'M14 5V4H5v16h9v-1M10 12h11M17 8l4 4-4 4',
    sun: 'M12 16a4 4 0 1 0 0-8 4 4 0 0 0 0 8zM12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4',
  ...LOCAL_ICONS,
  } as const;

  export type IconName = keyof typeof ICONS;
</script>

<script lang="ts">
  let { name, size = 18, label }: { name: IconName; size?: number; label?: string } = $props();
</script>

<svg
  class="icon"
  width={size}
  height={size}
  viewBox="0 0 24 24"
  fill="none"
  stroke="currentColor"
  stroke-width="1.75"
  stroke-linecap="round"
  stroke-linejoin="round"
  role={label ? 'img' : undefined}
  aria-label={label}
  aria-hidden={label ? undefined : 'true'}
>
  <path d={ICONS[name]} />
</svg>

<style>
  .icon {
    flex: none;
    display: block;
  }
</style>
