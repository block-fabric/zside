<script lang="ts">
  // The dialogs a page waits for (lib/dialogs.svelte.ts): questions, a line of text, passphrases.
  import SafeText from './SafeText.svelte';
  import { t } from '../lib/i18n.svelte';
  import { onDestroy } from 'svelte';
  import Dialog from './Dialog.svelte';
  import Icon from './Icon.svelte';
  import CopyButton from './CopyButton.svelte';
  import { answer, dialogs, type PassphraseAsk, type Pending } from '../lib/dialogs.svelte';
  import { walletDisplayName } from '../lib/wallet';
  import { revealBidi } from '../lib/format';

  const current: Pending | null = $derived(dialogs.queue[0] ?? null);

  // --- Questions: buttons that wait (SendConfirmationDialog) ---
  let elapsed = $state(0);
  let timer: ReturnType<typeof setInterval> | undefined;
  let checked = $state(false);
  let showDetails = $state(false);
  let shownId = -1;

  $effect(() => {
    const d = current;
    if (!d || d.id === shownId) return;
    shownId = d.id;
    elapsed = 0;
    showDetails = false;
    clearInterval(timer);
    if (d.kind === 'question') {
      checked = d.checkbox?.checked ?? false;
      if (d.choices.some((c) => c.delay)) timer = setInterval(() => elapsed++, 1000);
    } else if (d.kind === 'prompt') {
      text = d.value;
    } else {
      pass1 = '';
      pass2 = '';
      pass3 = '';
      reveal = false;
      caps = false;
      mismatch = false;
    }
  });
  onDestroy(() => clearInterval(timer));

  function left(delay = 0): number {
    return Math.max(0, delay - elapsed);
  }

  // --- A line of text ---
  let text = $state('');

  // --- Passphrases (AskPassphraseDialog) ---
  let pass1 = $state('');
  let pass2 = $state('');
  let pass3 = $state('');
  let reveal = $state(false);
  let caps = $state(false);
  let mismatch = $state(false);
  const MAX_PASSPHRASE = 1024;

  function acceptable(d: PassphraseAsk & { id: number }): boolean {
    if (d.mode === 'encrypt') return pass2 !== '' && pass3 !== '';
    if (d.mode === 'change') return pass1 !== '' && pass2 !== '' && pass3 !== '';
    return pass1 !== '';
  }

  function onKey(e: KeyboardEvent) {
    if (typeof e.getModifierState === 'function') caps = e.getModifierState('CapsLock');
  }

  function submitPassphrase(d: PassphraseAsk & { id: number }) {
    if (!acceptable(d)) return;
    if ((d.mode === 'encrypt' || d.mode === 'change') && pass2 !== pass3) {
      mismatch = true;
      return;
    }
    const value = d.mode === 'encrypt' ? { passphrase: pass2, next: pass2 } : d.mode === 'change' ? { passphrase: pass1, next: pass2 } : { passphrase: pass1 };
    // Overwrite the fields at once (secureClearPassFields).
    pass1 = pass2 = pass3 = '';
    answer(d.id, value);
  }

  function close() {
    if (!current) return;
    if (current.kind === 'question') answer(current.id, { choice: null, checked });
    else answer(current.id, null);
  }
</script>

{#if current?.kind === 'question'}
  {@const q = current}
  <Dialog open={true} title={q.title} onclose={close}>
    <div class="question {q.level ?? 'info'}">
      <span class="mark" aria-hidden="true"><Icon name={q.level === 'error' || q.level === 'warning' ? 'alert' : q.level === 'question' ? 'info' : 'info'} size={26} /></span>
      <div class="grow">
        <p class="message break"><SafeText text={q.message} /></p>
        {#if q.informative}<p class="informative break"><SafeText text={q.informative} /></p>{/if}
        {#if q.code}
          <div class="code">
            <pre class="mono break" dir="auto" translate="no">{revealBidi(q.code)}</pre>
            <CopyButton text={q.code} label="Copy" iconOnly={false} />
          </div>
        {/if}
        {#if q.checkbox}
          <label class="check" title={q.checkbox.tip}>
            <input type="checkbox" bind:checked />
            <span>{t(q.checkbox.label)}</span>
          </label>
        {/if}
        {#if q.details?.length}
          <button type="button" class="btn ghost small" aria-expanded={showDetails} onclick={() => (showDetails = !showDetails)}>
            {showDetails ? 'Hide details' : 'Show details…'}
          </button>
          {#if showDetails}
            <ul class="details">
              {#each q.details as line, i (i)}<li class="break">{line}</li>{/each}
            </ul>
          {/if}
        {/if}
      </div>
    </div>
    {#snippet actions()}
      {#each q.choices as c (c.id)}
        <button
          type="button"
          class="btn"
          class:primary={c.kind === 'primary'}
          class:danger={c.kind === 'danger'}
          disabled={left(c.delay) > 0}
          data-choice={c.id}
          onclick={() => answer(q.id, { choice: c.id, checked })}
        >
          {t(c.label)}{left(c.delay) > 0 ? ` (${left(c.delay)})` : ''}
        </button>
      {/each}
    {/snippet}
  </Dialog>
{:else if current?.kind === 'prompt'}
  {@const p = current}
  <Dialog open={true} title={p.title} onclose={close}>
    <form
      class="stack"
      id="prompt-form"
      onsubmit={(e) => {
        e.preventDefault();
        answer(p.id, text);
      }}
    >
      {#if p.message}<p class="break"><SafeText text={p.message} /></p>{/if}
      <div class="field">
        <label for="prompt-input"><SafeText text={p.label} /></label>
        <!-- svelte-ignore a11y_autofocus -->
        <input id="prompt-input" type="text" bind:value={text} autocomplete="off" spellcheck="false" autofocus />
      </div>
    </form>
    {#snippet actions()}
      <button type="button" class="btn" onclick={close}>Cancel</button>
      <button type="submit" form="prompt-form" class="btn primary">{p.okLabel ?? 'OK'}</button>
    {/snippet}
  </Dialog>
{:else if current?.kind === 'passphrase'}
  {@const d = current}
  <Dialog open={true} title={d.title} onclose={close}>
    <form
      class="stack"
      id="passphrase-form"
      onsubmit={(e) => {
        e.preventDefault();
        submitPassphrase(d);
      }}
    >
      {#if d.error}<p class="error-box break" role="alert">{d.error}</p>{/if}
      {#if d.wallet !== undefined}<p class="muted">Wallet: <strong><SafeText text={walletDisplayName(d.wallet)} /></strong></p>{/if}
      {#if d.mode === 'encrypt'}
        <p>Enter the new passphrase for the wallet.<br />Please use a passphrase of <strong>ten or more random characters</strong>, or <strong>eight or more words</strong>.</p>
      {:else if d.mode === 'change'}
        <p>Enter the old passphrase and new passphrase for the wallet.</p>
      {:else}
        <p>This operation needs your wallet passphrase to unlock the wallet.</p>
      {/if}
      {#if d.mode === 'unlock' || d.mode === 'migrate' || d.mode === 'change'}
        <div class="field">
          <label for="pass1">{d.mode === 'change' ? 'Old passphrase' : 'Passphrase'}</label>
          <!-- svelte-ignore a11y_autofocus -->
          <input id="pass1" type={reveal ? 'text' : 'password'} maxlength={MAX_PASSPHRASE} bind:value={pass1} onkeydown={onKey} onkeyup={onKey} autocomplete="off" autofocus />
        </div>
      {/if}
      {#if d.mode === 'encrypt' || d.mode === 'change'}
        <div class="field">
          <label for="pass2">New passphrase</label>
          <!-- svelte-ignore a11y_autofocus -->
          <input id="pass2" type={reveal ? 'text' : 'password'} maxlength={MAX_PASSPHRASE} bind:value={pass2} onkeydown={onKey} onkeyup={onKey} autocomplete="new-password" autofocus={d.mode === 'encrypt'} />
        </div>
        <div class="field">
          <label for="pass3">Repeat new passphrase</label>
          <input id="pass3" type={reveal ? 'text' : 'password'} maxlength={MAX_PASSPHRASE} bind:value={pass3} onkeydown={onKey} onkeyup={onKey} autocomplete="new-password" aria-invalid={mismatch ? 'true' : undefined} />
          {#if mismatch}<span class="error">The supplied passphrases do not match.</span>{/if}
        </div>
      {/if}
      <div class="row between">
        <label class="check"><input type="checkbox" bind:checked={reveal} /> <span>Show passphrase</span></label>
        {#if caps}<span class="caps" role="status"><Icon name="alert" size={15} /> Warning: The Caps Lock key is on!</span>{/if}
      </div>
    </form>
    {#snippet actions()}
      <button type="button" class="btn" onclick={close}>Cancel</button>
      <button type="submit" form="passphrase-form" class="btn primary" disabled={!acceptable(d)}>OK</button>
    {/snippet}
  </Dialog>
{/if}

<style>
  .question {
    display: flex;
    gap: 0.9rem;
    align-items: flex-start;
  }
  .mark {
    color: var(--accent-fg);
    padding-top: 0.1rem;
  }
  .warning .mark {
    color: var(--warning);
  }
  .error .mark {
    color: var(--negative);
  }
  .message {
    font-weight: 550;
  }
  .informative {
    color: var(--text-muted);
    font-size: 0.93rem;
    white-space: pre-line;
  }
  .details {
    margin: 0.5rem 0 0;
    padding-left: 1.1rem;
    color: var(--text-muted);
    font-size: 0.9rem;
  }
  .code {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 0.3rem;
    margin: 0.4rem 0 0.6rem;
  }
  .code pre {
    margin: 0;
    width: 100%;
    max-height: 9rem;
    overflow: auto;
    padding: 0.55rem 0.7rem;
    border-radius: var(--radius-s);
    background: var(--surface-2);
    font-size: 0.8rem;
    white-space: pre-wrap;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.85rem;
  }
  .between {
    justify-content: space-between;
    flex-wrap: wrap;
  }
  .caps {
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
    color: var(--warning);
    font-size: 0.88rem;
    font-weight: 600;
  }
  .error-box {
    margin: 0;
    padding: 0.55rem 0.75rem;
    border-radius: var(--radius-s);
    background: var(--negative-soft, var(--warning-soft));
    color: var(--negative);
    font-weight: 550;
  }
</style>
