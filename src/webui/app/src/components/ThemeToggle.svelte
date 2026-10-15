<script lang="ts" module>
  import type { ThemeId } from '../lib/themes';
  /** The dark look last chosen, to come back to it from the light one. */
  let lastDark: ThemeId | null = null;
</script>

<script lang="ts">
  // The quick switch between the light look and the dark one, in the top bars (sun and moon). The
  // other looks are in Settings → Appearance, the sidebars and the palette.
  import Icon from './Icon.svelte';
  import { prefs, savePrefs } from '../lib/state.svelte';
  import { isDark, toggledTheme } from '../lib/themes';

  let prefersDark = $state(false);
  $effect(() => {
    if (typeof window.matchMedia !== 'function') return;
    const media = window.matchMedia('(prefers-color-scheme: dark)');
    prefersDark = media.matches;
    const onChange = (e: MediaQueryListEvent) => (prefersDark = e.matches);
    media.addEventListener('change', onChange);
    return () => media.removeEventListener('change', onChange);
  });

  const dark = $derived(isDark(prefs.theme, prefersDark));
  const label = $derived(dark ? 'Switch to the light look' : 'Switch to the dark look');

  function toggle() {
    if (dark && prefs.theme !== 'system') lastDark = prefs.theme;
    savePrefs({ theme: toggledTheme(prefs.theme, prefersDark, lastDark) });
  }
</script>

<button type="button" class="btn round theme-toggle" aria-label={label} title={label} onclick={toggle}>
  <Icon name={dark ? 'sun' : 'moon'} size={17} />
</button>
