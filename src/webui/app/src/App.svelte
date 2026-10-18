<script lang="ts">
  // The page of chains-app: what it shows follows the phase of the app.
  import { onMount } from 'svelte';
  import { CHAIN } from './chain/config';
  import AppDialogs from './components/AppDialogs.svelte';
  import DialogHost from './components/DialogHost.svelte';
  import NodeDialogs from './components/NodeDialogs.svelte';
  import ResizeHandles from './components/ResizeHandles.svelte';
  import TitleStrip from './components/TitleStrip.svelte';
  import Toasts from './components/Toasts.svelte';
  import { hasHost, installEventSink, on } from './lib/bridge';
  import { t, followLocale, loadLocale } from './lib/i18n.svelte';
  import { handleShortcut } from './lib/keyboard';
  import { go, nav, PAGES, type PageId } from './lib/nav.svelte';
  import { host } from './lib/rpc';
  import { app, applySnapshot, loadPrefs, phaseEvents, prefs, subscribe, toast } from './lib/state.svelte';
  import { applyLook } from './lib/customthemes';
  import { look } from './lib/themestore.svelte';
  import { parsePaymentUri } from './lib/uri';
  import { watchWindow } from './lib/windowframe.svelte';
  import Fatal from './views/Fatal.svelte';
  import Intro from './views/Intro.svelte';
  import Shell from './views/Shell.svelte';
  import Splash from './views/Splash.svelte';

  let prefersDark = $state(false);
  let connectError = $state('');

  $effect(() => {
    applyLook(document, prefs.theme, prefs.customThemes, look.preview, prefs.fontAdjust, prefersDark, prefs.fontFamily, prefs.moneyFont);
  });

  async function refreshNode() {
    try {
      applySnapshot(await host.nodeState());
    } catch {
      // Not running yet; the events will tell.
    }
  }

  onMount(() => {
    const media = window.matchMedia('(prefers-color-scheme: dark)');
    prefersDark = media.matches;
    const onScheme = (e: MediaQueryListEvent) => (prefersDark = e.matches);
    media.addEventListener('change', onScheme);

    installEventSink();
    let offWindow: (() => void) | undefined;
    subscribe();
    const offPhase = on<{ phase: string }>('phase', ({ phase }) => {
      if (phase === 'ready') refreshNode();
      if (phase === 'init' || phase === 'ready') loadPrefs();
    });
    const offUri = on<{ uri: string }>('uri', ({ uri }) => handleUri(uri));
    // The tray menu, or another part of the app, asks for a page.
    const offNavigate = on<{ page: PageId; tab: string }>('navigate', ({ page, tab }) => {
      if (page in PAGES) go(page, tab);
    });

    (async () => {
      if (import.meta.env.DEV && !hasHost()) {
        const mock = await import('./lib/mock');
        mock.installMock();
      }
      try {
        // The phase the host says now; a phase event that comes while the page gets ready (the
        // language is loaded, below) is newer, and is not overwritten by this answer: a node that
        // starts fast (no wallet to load) is ready before the language is, and the page stayed on
        // the splash for ever.
        const phasesBefore = phaseEvents.count;
        const hello = await host.hello();
        // The language first: the page shows in it from its first frame on.
        followLocale();
        await loadLocale();
        app.info = hello.info;
        app.dialogs = hello.dialogs;
        if (phaseEvents.count === phasesBefore) {
          app.phaseData = hello.phaseData;
          app.phase = hello.phase;
        }
        if (hello.info.test) {
          // The tests of the app drive the page through this (only with -webuitestscript).
          const state = await import('./lib/state.svelte');
          const navigation = await import('./lib/nav.svelte');
          const dialogs = await import('./lib/dialogs.svelte');
          const walletdata = await import('./lib/walletdata.svelte');
          const i18n = await import('./lib/i18n.svelte');
          const commands = await import('./lib/commands.svelte');
          const quit = await import('./lib/quitguard');
          (window as unknown as Record<string, unknown>).__chainsTest = { ...state, ...navigation, dialogs: dialogs.dialogs, answerDialog: dialogs.answer, walletData: walletdata.current, i18n: i18n.i18n, t: i18n.t, buildCommands: commands.buildCommands, quitGuard: quit.quitGuard };
        }
        offWindow = watchWindow();
        await loadPrefs();
        if (app.phase === 'ready') await refreshNode();
      } catch (e) {
        connectError = e instanceof Error ? e.message : String(e);
      }
    })();

    return () => {
      media.removeEventListener('change', onScheme);
      offPhase();
      offUri();
      offWindow?.();
      offNavigate();
    };
  });

  /** A payment URI: from another start of the app, or dropped on the window (BitcoinGUI::dropEvent). */
  function handleUri(uri: string) {
    const parsed = parsePaymentUri(uri);
    if (!parsed.ok) {
      toast('error', t('Payment request error'), parsed.error);
      return;
    }
    nav.pendingUri = uri;
    nav.uriSeq++;
    go('send');
    toast('info', t('Payment request'), t('Pay to %1', parsed.request.address));
  }

  function droppedUris(e: DragEvent): string[] {
    const data = e.dataTransfer;
    if (!data) return [];
    const text = data.getData('text/uri-list') || data.getData('text/plain') || '';
    return text
      .split(/\r?\n/)
      .map((l) => l.trim())
      .filter((l) => l && !l.startsWith('#') && l.toLowerCase().startsWith(`${CHAIN.uriScheme}:`));
  }

  function ondragover(e: DragEvent) {
    if (app.phase !== 'ready' || !e.dataTransfer) return;
    // A link dropped on the window would load in it: the app takes payment links, and nothing else.
    e.preventDefault();
    e.dataTransfer.dropEffect = [...e.dataTransfer.types].some((t) => t === 'text/uri-list' || t === 'text/plain') ? 'copy' : 'none';
  }

  function ondrop(e: DragEvent) {
    if (app.phase !== 'ready') return;
    const target = e.target as HTMLElement | null;
    // Text dropped into a field stays the business of the field.
    if (target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA')) return;
    e.preventDefault();
    for (const uri of droppedUris(e)) handleUri(uri);
  }

  function onkeydown(e: KeyboardEvent) {
    if (app.phase !== 'ready' || e.defaultPrevented) return;
    if (handleShortcut(e)) e.preventDefault();
  }
</script>

<svelte:window {onkeydown} {ondragover} {ondrop} />

{#if connectError || app.phase !== 'ready'}
  <!-- Before the node runs: the integrated title bar alone (the layouts have theirs). -->
  <div class="frame">
    <TitleStrip />
    <div class="phase">
      {#if connectError}
        <main class="lost">
          <h1>No connection to the node</h1>
          <p class="muted">{connectError}</p>
        </main>
      {:else if app.phase === 'intro'}
        <Intro />
      {:else if app.phase === 'fatal'}
        <Fatal />
      {:else if app.phase === 'shutdown'}
        <Splash stopping />
      {:else}
        <Splash />
      {/if}
    </div>
  </div>
{:else}
  <Shell />
{/if}
<ResizeHandles />

<NodeDialogs />
<AppDialogs />
<DialogHost />
<Toasts />

<style>
  .frame {
    display: flex;
    flex-direction: column;
    height: 100%;
  }
  .phase {
    flex: 1 1 0;
    min-height: 0;
  }
  .lost {
    display: grid;
    place-content: center;
    gap: 0.5rem;
    height: 100%;
    text-align: center;
  }
</style>
