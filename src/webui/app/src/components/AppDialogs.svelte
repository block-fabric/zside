<script lang="ts">
  // The dialogs of the app itself: about, command-line options.
  import SafeText from './SafeText.svelte';
  import { CHAIN } from '../chain/config';
  import Dialog from './Dialog.svelte';
  import { host } from '../lib/rpc';
  import { nav } from '../lib/nav.svelte';

  let about: { name: string; version: string; license: string } | null = $state(null);
  let help: { usage: string; options: string } | null = $state(null);

  $effect(() => {
    if (nav.dialog === 'about' && !about) host.about().then((a) => (about = a)).catch(() => undefined);
    if (nav.dialog === 'help' && !help) host.help().then((h) => (help = h)).catch(() => undefined);
  });

  function close() {
    nav.dialog = null;
  }
</script>

{#if nav.dialog === 'about'}
  <Dialog open={true} title="About {about?.name ?? CHAIN.name}" onclose={close}>
    <p class="version"><SafeText text={about?.name} /> version {about?.version}</p>
    <pre class="license">{about?.license}</pre>
    <p class="muted credits">
      The interface runs in a native web view (webview/webview, MIT), and is built with Svelte. Its themes follow the ones of chains-qt;
      the classic dark theme takes its colours from QDarkStyleSheet, and the Catppuccin Mocha and Dracula themes from the palettes of those projects (MIT). Fonts: Inter, Nunito, JetBrains Mono (SIL Open Font License).
    </p>
    {#snippet actions()}<button type="button" class="btn primary" onclick={close}>Close</button>{/snippet}
  </Dialog>
{:else if nav.dialog === 'help'}
  <Dialog open={true} title="Command-line options" wide onclose={close}>
    <p class="mono">{help?.usage}</p>
    <pre class="options">{help?.options}</pre>
    {#snippet actions()}<button type="button" class="btn primary" onclick={close}>Close</button>{/snippet}
  </Dialog>
{/if}

<style>
  .version {
    font-weight: 600;
  }
  pre {
    font-family: var(--font-mono);
    font-size: 0.82rem;
    white-space: pre-wrap;
    background: var(--surface-2);
    padding: 0.8rem;
    border-radius: var(--radius-s);
    max-height: 50vh;
    overflow: auto;
  }
  .credits {
    font-size: 0.88rem;
  }
</style>
