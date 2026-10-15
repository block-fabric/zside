<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  import { CHAIN } from '../chain/config';
  // The console of the node window (RPCConsole): commands with nesting and queries, a history
  // (arrows), completion of command names (Tab), a wallet to run them on, help-console, Ctrl+L.
  import { onMount, tick } from 'svelte';
  import Icon from './Icon.svelte';
  import { css } from '../lib/actions';
  import { commandsFromHelp, complete, completions, SCAM_WARNING, welcome } from '../lib/console';
  import { clearConsole, consoleState, history, replace, say } from '../lib/consolestate.svelte';
  import { inform } from '../lib/dialogs.svelte';
  import { host, rpc } from '../lib/rpc';
  import { app, prefs, savePrefs, wallets } from '../lib/state.svelte';
  import { walletDisplayName } from '../lib/wallet';

  const MIN_FONT = 8;
  const MAX_FONT = 28;

  let input = $state('');
  let field: HTMLInputElement | undefined = $state();
  let output: HTMLDivElement | undefined = $state();
  let suggestions = $state<string[]>([]);
  let pick = $state(0);
  /** The wallet the commands run on: the current one by default; '' for none. */
  let walletChoice = $state<string | null>(null);
  const wallet = $derived(walletChoice ?? wallets.current ?? '');
  const fontSize = $derived(prefs.consoleFont);

  function scrollToEnd() {
    tick().then(() => {
      if (output) output.scrollTop = output.scrollHeight;
    });
  }

  function greet() {
    if (consoleState.welcomed) return;
    consoleState.welcomed = true;
    say('info', welcome(app.info?.name ?? CHAIN.name));
    say('error', SCAM_WARNING);
  }

  onMount(() => {
    greet();
    scrollToEnd();
    field?.focus();
    if (consoleState.commands.length === 0) {
      rpc.call('help', []).then((text) => (consoleState.commands = commandsFromHelp(text))).catch(() => undefined);
    }
  });

  async function submit() {
    const cmd = input.trim();
    if (!cmd) return;
    let filtered = cmd;
    try {
      const check = await host.consoleCheck(cmd);
      if (!check.ok) {
        await inform(t('Error'), check.error ?? t('Invalid command line'), 'error');
        return;
      }
      filtered = check.filtered ?? cmd;
    } catch (e) {
      await inform(t('Error'), e instanceof Error ? e.message : String(e), 'error');
      return;
    }
    // A stop may be asked for while a long command runs.
    if (cmd === 'stop') {
      input = '';
      say('request', filtered);
      host.consoleRun(cmd, null).catch(() => undefined);
      return;
    }
    if (consoleState.executing) return;
    input = '';
    suggestions = [];
    if (app.info?.walletEnabled && consoleState.lastWallet !== wallet) {
      say('request', wallet ? `Executing command using "${walletDisplayName(wallet)}" wallet` : 'Executing command without any wallet');
      consoleState.lastWallet = wallet;
    }
    say('request', filtered);
    const pending = say('reply', 'Executing…');
    history.add(filtered);
    consoleState.executing = true;
    scrollToEnd();
    try {
      const r = await host.consoleRun(cmd, wallet || null);
      replace(pending, r.ok ? 'reply' : 'error', r.ok ? (r.result ?? '') : (r.error ?? ''));
    } catch (e) {
      replace(pending, 'error', e instanceof Error ? e.message : String(e));
    } finally {
      consoleState.executing = false;
      scrollToEnd();
    }
  }

  function updateSuggestions() {
    suggestions = completions(input, consoleState.commands);
    pick = 0;
  }

  function onkeydown(e: KeyboardEvent) {
    if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
      if (suggestions.length) {
        e.preventDefault();
        pick = (pick + (e.key === 'ArrowUp' ? -1 : 1) + suggestions.length) % suggestions.length;
        return;
      }
      e.preventDefault();
      input = history.browse(e.key === 'ArrowUp' ? -1 : 1);
      return;
    }
    if (e.key === 'Tab' && suggestions.length) {
      e.preventDefault();
      input = complete(input, suggestions[pick]);
      suggestions = [];
      return;
    }
    if (e.key === 'Escape' && suggestions.length) {
      e.preventDefault();
      e.stopPropagation();
      suggestions = [];
      return;
    }
    if (e.key === 'Enter') {
      e.preventDefault();
      if (suggestions.length && pick >= 0 && input.trim() && !/\s/.test(input.trim()) && consoleState.commands.includes(input.trim()) === false && suggestions[pick].startsWith(input.trim())) {
        input = complete(input, suggestions[pick]);
        suggestions = [];
        return;
      }
      submit();
      return;
    }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'l') {
      e.preventDefault();
      clear();
    }
  }

  function clear() {
    clearConsole();
    history.reset();
    greet();
    field?.focus();
  }

  function setFont(size: number) {
    savePrefs({ consoleFont: Math.max(MIN_FONT, Math.min(MAX_FONT, size)) });
  }

  function timeText(t: number) {
    const d = new Date(t);
    return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}:${String(d.getSeconds()).padStart(2, '0')}`;
  }
</script>

<section class="console panel">
  <div class="bar">
    {#if app.info?.walletEnabled}
      <label class="row">
        <span class="muted">Wallet:</span>
        <select class="wallet" value={wallet} onchange={(e) => (walletChoice = (e.currentTarget as HTMLSelectElement).value)} aria-label="Wallet the commands run on">
          <option value="">(none)</option>
          {#each wallets.list as w (w.name)}<option value={w.name} translate="no">{walletDisplayName(w.name)}</option>{/each}
        </select>
      </label>
    {/if}
    <span class="grow"></span>
    <button type="button" class="btn ghost small icon" title="Decrease font size" aria-label="Decrease font size" onclick={() => setFont(fontSize - 1)}>A−</button>
    <button type="button" class="btn ghost small icon" title="Increase font size" aria-label="Increase font size" onclick={() => setFont(fontSize + 1)}>A+</button>
    <button type="button" class="btn ghost small" title="Clear console (Ctrl+L)" onclick={clear}><Icon name="trash" size={15} /> Clear</button>
  </div>
  <div class="output mono" bind:this={output} role="log" aria-live="polite" aria-label="Console output" use:css={{ 'font-size': `${fontSize}px` }}>
    {#each consoleState.lines as line (line.id)}
      <div class="line {line.kind}">
        <span class="time" translate="no">{timeText(line.time)}</span>
        <span class="mark" aria-hidden="true">{line.kind === 'request' ? '›' : line.kind === 'error' ? '!' : line.kind === 'reply' ? '‹' : '·'}</span>
        <pre class="text">{line.text}</pre>
      </div>
    {/each}
  </div>
  <div class="prompt">
    {#if suggestions.length}
      <ul class="suggest" role="listbox" aria-label="Commands">
        {#each suggestions as s, i (s)}
          <li role="option" aria-selected={i === pick}>
            <button type="button" tabindex="-1" class:picked={i === pick} onclick={() => { input = complete(input, s); suggestions = []; field?.focus(); }}>{s}</button>
          </li>
        {/each}
      </ul>
    {/if}
    <span class="caret mono" aria-hidden="true">&gt;</span>
    <input
      id="console-input"
      bind:this={field}
      type="text"
      class="mono grow"
      placeholder={consoleState.executing ? 'Executing…' : 'Enter a command, help for the list, Tab to complete'}
      spellcheck="false"
      autocomplete="off"
      aria-label="Console command"
      bind:value={input}
      oninput={updateSuggestions}
      {onkeydown}
    />
  </div>
</section>

<style>
  .console {
    display: flex;
    flex-direction: column;
    height: calc(100vh - 15rem);
    min-height: 22rem;
    overflow: hidden;
  }
  .bar {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    padding: 0.5rem 0.75rem;
    border-bottom: 1px solid var(--line);
  }
  .wallet {
    width: auto;
    min-width: 12rem;
    min-height: 1.9rem;
    padding: 0.2rem 0.5rem;
  }
  .output {
    flex: 1;
    overflow: auto;
    padding: 0.6rem 0.75rem;
    background: var(--bg);
  }
  .line {
    display: grid;
    grid-template-columns: 4.6em 1.2em minmax(0, 1fr);
    gap: 0.3em;
    padding: 0.12em 0;
  }
  .time {
    color: var(--text-faint);
    font-size: 0.85em;
    padding-top: 0.1em;
  }
  .mark {
    color: var(--text-faint);
  }
  .text {
    margin: 0;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    font: inherit;
  }
  .request .text,
  .request .mark {
    color: var(--accent-strong);
    font-weight: 600;
  }
  .error .text,
  .error .mark {
    color: var(--negative);
  }
  .info .text {
    color: var(--text-muted);
  }
  .prompt {
    position: relative;
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0.5rem 0.75rem;
    border-top: 1px solid var(--line);
  }
  .caret {
    color: var(--accent-fg);
    font-weight: 700;
  }
  .suggest {
    position: absolute;
    left: 2.2rem;
    bottom: 100%;
    z-index: 2;
    margin: 0;
    padding: 0.25rem;
    list-style: none;
    background: var(--surface);
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-s);
    box-shadow: var(--shadow);
    max-height: 16rem;
    overflow: auto;
  }
  .suggest button {
    display: block;
    width: 100%;
    padding: 0.2rem 0.6rem;
    border: 0;
    border-radius: 4px;
    background: none;
    color: var(--text);
    font-family: var(--font-mono);
    text-align: left;
    cursor: pointer;
  }
  .suggest button.picked,
  .suggest button:hover {
    background: var(--accent-soft);
  }
</style>
