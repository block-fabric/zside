<script lang="ts">
  // Settings → Advanced theme: the themes of the user. Create one from any theme (built-in or the
  // user's), change its colours with a colour picker or as text, drawn on the whole app while it is
  // edited (and back as it was on Cancel); save, rename, duplicate, delete; import and export theme
  // files through the file chooser of the desktop, or as text. Theme files are untrusted: they are read
  // by lib/customthemes.ts (known colours only, colour values only), and names are shown as text.
  import Icon from './Icon.svelte';
  import SafeText from './SafeText.svelte';
  import { confirm, errorText, prompt } from '../lib/dialogs.svelte';
  import { t } from '../lib/i18n.svelte';
  import { host } from '../lib/rpc';
  import { prefs, toast } from '../lib/state.svelte';
  import { stagePrefs } from '../lib/settingsdraft.svelte';
  import { THEMES } from '../lib/themes';
  import {
    BASE_THEMES,
    baseColors,
    contrastWarnings,
    effectiveColors,
    isColor,
    isCustomId,
    MAX_CUSTOM_THEMES,
    MAX_THEME_NAME,
    parseColor,
    TOKEN_GROUPS,
    toHex,
    checkName,
    type BaseTheme,
    type CustomTheme,
    type CustomThemeId,
    type TokenName,
  } from '../lib/customthemes';
  import { copyTheme, deleteTheme, duplicateTheme, exportFile, importFile, importText, look, newTheme, saveTheme, unreadableNote } from '../lib/themestore.svelte';

  /** The theme being edited (a copy: saved only on Save), and whether it is a new one. */
  let editing = $state<CustomTheme | null>(null);
  let isNew = $state(false);
  /** Colour values typed that are not colours yet, by token (shown, not drawn). */
  let typed = $state<Partial<Record<TokenName, string>>>({});
  let nameError = $state('');
  let startFrom = $state<string>(prefs.theme === 'system' ? 'midnight' : prefs.theme);
  let pasteOpen = $state(false);
  let pasted = $state('');
  let importError = $state('');

  /** The names of the built-in themes, and of the colours (words of the app, shown with t()). */
  const NAMES: Record<string, string> = Object.fromEntries(THEMES.map((x) => [x.id, x.name]));
  const LABELS: Record<string, string> = Object.fromEntries(TOKEN_GROUPS.flatMap((g) => g.tokens.map((x) => [x.name, x.label])));
  const maxThemes = MAX_CUSTOM_THEMES;

  // The theme being edited is drawn on the whole app; nothing of it stays when the editor closes.
  $effect(() => {
    look.preview = editing ? { ...editing, colors: { ...editing.colors } } : null;
  });
  $effect(() => () => {
    look.preview = null;
  });

  const colors = $derived(editing ? effectiveColors(editing) : null);
  const warnings = $derived(colors ? contrastWarnings(colors) : []);

  /** The colours of the swatch of a theme of the user. */
  function swatch(theme: CustomTheme): string[] {
    const c = effectiveColors(theme);
    return [c.bg, c.raised, c.accent];
  }

  function themeOf(id: string): Pick<CustomTheme, 'name' | 'base' | 'colors'> | null {
    const custom = prefs.customThemes.find((x) => x.id === id);
    if (custom) return custom;
    const base = BASE_THEMES.find((x) => x === id);
    return base ? { name: t('My %1', t(NAMES[base])), base, colors: {} } : null;
  }

  function create() {
    const from = themeOf(startFrom);
    if (!from) return;
    if (prefs.customThemes.length >= MAX_CUSTOM_THEMES) {
      toast('error', t('Advanced theme'), t('There are already %1 themes: delete one first.', MAX_CUSTOM_THEMES));
      return;
    }
    const fresh = newTheme(from);
    // A copy of the name of a theme of the user says so.
    if (isCustomId(startFrom)) fresh.name = newTheme({ ...from, name: t('%1 (copy)', from.name) }).name;
    open(fresh, true);
  }

  function open(theme: CustomTheme, fresh = false) {
    editing = { ...theme, colors: { ...theme.colors } };
    isNew = fresh;
    typed = {};
    nameError = '';
  }

  function cancel() {
    editing = null;
    typed = {};
    nameError = '';
  }

  function save() {
    if (!editing) return;
    const name = checkName(editing.name);
    if ('error' in name) {
      nameError = name.error;
      return;
    }
    try {
      const saved = saveTheme({ ...editing, name: name.name });
      // The look stays as it was drawn: the saved theme is chosen (it waits for Apply, as the other looks).
      stagePrefs({ theme: saved.id });
      toast('success', t('Advanced theme'), isNew ? t('The theme is saved, and chosen. Apply to keep it as the look of the app.') : t('The theme is saved.'));
      editing = null;
      typed = {};
    } catch (e) {
      toast('error', t('Advanced theme'), errorText(e));
    }
  }

  function setColor(token: TokenName, value: string) {
    if (!editing) return;
    const v = value.trim();
    if (isColor(v)) {
      editing.colors[token] = v;
      delete typed[token];
    } else {
      typed[token] = value;
    }
  }

  /** The colour picker: the colour, keeping the transparency the value had. */
  function pick(token: TokenName, hex: string) {
    if (!editing || !colors) return;
    const now = parseColor(colors[token]);
    const chosen = parseColor(hex);
    if (!chosen) return;
    setColor(token, toHex({ ...chosen, a: now?.a ?? 1 }));
  }

  function resetColor(token: TokenName) {
    if (!editing) return;
    delete editing.colors[token];
    delete typed[token];
  }

  function changeBase(base: BaseTheme) {
    if (editing) editing.base = base;
  }

  /** The value of the colour picker: #rrggbb, without the transparency. */
  function pickerValue(value: string): string {
    const c = parseColor(value);
    return c ? toHex({ ...c, a: 1 }) : '#000000';
  }

  function use(theme: CustomTheme) {
    stagePrefs({ theme: theme.id });
  }

  async function rename(theme: CustomTheme) {
    const name = await prompt(t('Rename theme'), t('New name of the theme'), theme.name, { okLabel: t('Rename') });
    if (name === null) return;
    const checked = checkName(name);
    if ('error' in checked) {
      toast('error', t('Rename theme'), checked.error);
      return;
    }
    try {
      saveTheme({ ...theme, name: checked.name });
    } catch (e) {
      toast('error', t('Rename theme'), errorText(e));
    }
  }

  function duplicate(theme: CustomTheme) {
    try {
      duplicateTheme(theme.id);
    } catch (e) {
      toast('error', t('Duplicate theme'), errorText(e));
    }
  }

  async function remove(theme: CustomTheme) {
    const ok = await confirm(t('Delete theme'), t('Delete the theme "%1"? This cannot be undone; export it first to keep a copy.', theme.name), { yes: 'Delete', danger: true, level: 'warning' });
    if (!ok) return;
    if (editing?.id === theme.id) cancel();
    deleteTheme(theme.id as CustomThemeId);
    toast('info', t('Advanced theme'), t('The theme is deleted.'));
  }

  async function exportTheme(theme: Pick<CustomTheme, 'name' | 'base' | 'colors'>) {
    try {
      const path = await exportFile(theme);
      if (path) toast('success', t('Export Theme'), t('The theme was saved to %1.', path));
    } catch (e) {
      toast('error', t('Export Theme'), errorText(e));
    }
  }

  async function copy(theme: Pick<CustomTheme, 'name' | 'base' | 'colors'>) {
    try {
      await copyTheme(theme);
      toast('success', t('Advanced theme'), t('The theme is copied to the clipboard, as text.'));
    } catch (e) {
      toast('error', t('Advanced theme'), errorText(e));
    }
  }

  async function importFromFile() {
    importError = '';
    try {
      const theme = await importFile();
      if (theme) {
        toast('success', t('Import Theme'), t('The theme "%1" is imported.', theme.name));
        // What the page draws as the base, and why (audit 7 #10).
        const note = unreadableNote(theme);
        if (note) toast('warning', t('Import Theme'), note);
      }
    } catch (e) {
      importError = errorText(e);
    }
  }

  async function pasteFromClipboard() {
    try {
      pasted = (await host.paste()) ?? '';
    } catch (e) {
      importError = errorText(e);
    }
  }

  function importPasted() {
    importError = '';
    try {
      const theme = importText(pasted);
      toast('success', t('Import Theme'), t('The theme "%1" is imported.', theme.name));
      const note = unreadableNote(theme);
      if (note) toast('warning', t('Import Theme'), note);
      pasted = '';
      pasteOpen = false;
    } catch (e) {
      importError = errorText(e);
    }
  }
</script>

<div class="editor-page">
  <section class="panel">
    <div class="panel-body stack">
      <p class="intro">Make your own look: start from any theme, change its colours, and share it as a theme file. A theme changes colours only; the fonts, the corners and the shadows come from the theme it starts from.</p>

      <div class="toolbar">
        <div class="field start">
          <label for="theme-start">Start from</label>
          <div class="row">
            <select id="theme-start" bind:value={startFrom}>
              <optgroup label="Built-in themes">
                {#each BASE_THEMES as id (id)}<option value={id}>{t(NAMES[id])}</option>{/each}
              </optgroup>
              {#if prefs.customThemes.length > 0}
                <optgroup label="Your themes">
                  {#each prefs.customThemes as theme (theme.id)}<option value={theme.id} translate="no">{theme.name}</option>{/each}
                </optgroup>
              {/if}
            </select>
            <button type="button" class="btn primary" disabled={!!editing} onclick={create}><Icon name="plus" size={15} /> New theme</button>
          </div>
        </div>
        <span class="grow"></span>
        <button type="button" class="btn" onclick={importFromFile}><Icon name="upload" size={15} /> Import file…</button>
        <button type="button" class="btn" aria-expanded={pasteOpen} onclick={() => (pasteOpen = !pasteOpen)}><Icon name="paste" size={15} /> Paste theme…</button>
      </div>

      {#if pasteOpen}
        <div class="field paste">
          <label for="theme-paste">Theme as text (JSON)</label>
          <textarea id="theme-paste" class="mono" rows="6" spellcheck="false" placeholder="Paste the text of a theme file here" bind:value={pasted}></textarea>
          <div class="row">
            <button type="button" class="btn small" onclick={pasteFromClipboard}><Icon name="paste" size={14} /> From the clipboard</button>
            <span class="grow"></span>
            <button type="button" class="btn small" onclick={() => { pasteOpen = false; pasted = ''; importError = ''; }}>Cancel</button>
            <button type="button" class="btn small primary" disabled={!pasted.trim()} onclick={importPasted}>Import</button>
          </div>
        </div>
      {/if}
      {#if importError}<p class="error" role="alert">{importError}</p>{/if}

      <div class="field">
        <span class="label">Your themes ({prefs.customThemes.length} of {maxThemes})</span>
        {#if prefs.customThemes.length === 0}
          <p class="muted empty">No theme of your own yet. Create one, or import a theme file.</p>
        {:else}
          <ul class="mine" aria-label="Your themes">
            {#each prefs.customThemes as theme (theme.id)}
              <li class:chosen={prefs.theme === theme.id} class:open={editing?.id === theme.id}>
                <span class="swatch" aria-hidden="true">{#each swatch(theme) as c, i (i)}<span style:background={c}></span>{/each}</span>
                <span class="who">
                  <SafeText class="name" text={theme.name} />
                  <span class="faint small">{t('From %1', t(NAMES[theme.base]))}{prefs.theme === theme.id ? ` · ${t('in use')}` : ''}</span>
                </span>
                <span class="actions">
                  <button type="button" class="btn small" disabled={prefs.theme === theme.id} onclick={() => use(theme)}>Use</button>
                  <button type="button" class="btn small" disabled={!!editing} onclick={() => open(theme)}><Icon name="edit" size={14} /> Edit</button>
                  <button type="button" class="btn small ghost" title="Rename" aria-label="Rename" onclick={() => rename(theme)}>Rename…</button>
                  <button type="button" class="btn small ghost icon" title="Duplicate" aria-label="Duplicate" onclick={() => duplicate(theme)}><Icon name="copy" size={14} /></button>
                  <button type="button" class="btn small ghost icon" title="Export to a file" aria-label="Export to a file" onclick={() => exportTheme({ ...theme, colors: effectiveColors(theme) })}><Icon name="download" size={14} /></button>
                  <button type="button" class="btn small ghost icon" title="Copy as text" aria-label="Copy as text" onclick={() => copy({ ...theme, colors: effectiveColors(theme) })}><Icon name="text" size={14} /></button>
                  <button type="button" class="btn small ghost icon danger" title="Delete" aria-label="Delete" onclick={() => remove(theme)}><Icon name="trash" size={14} /></button>
                </span>
              </li>
            {/each}
          </ul>
        {/if}
      </div>
    </div>
  </section>

  {#if editing && colors}
    <section class="panel edit" aria-label="Theme editor">
      <div class="panel-head"><h2>{isNew ? t('New theme') : t('Edit theme')}</h2><span class="faint small">Drawn on the whole app while you edit it.</span></div>
      <div class="panel-body stack">
        <div class="cols">
          <div class="field">
            <label for="theme-name">Name</label>
            <input id="theme-name" type="text" maxlength={MAX_THEME_NAME} bind:value={editing.name} oninput={() => (nameError = '')} />
            {#if nameError}<span class="error" role="alert">{nameError}</span>{/if}
          </div>
          <div class="field">
            <label for="theme-base">Fonts, corners and shadows of</label>
            <select id="theme-base" value={editing.base} onchange={(e) => changeBase((e.currentTarget as HTMLSelectElement).value as BaseTheme)}>
              {#each BASE_THEMES as id (id)}<option value={id}>{t(NAMES[id])}</option>{/each}
            </select>
          </div>
        </div>

        {#if warnings.length > 0}
          <div class="warnings" role="status">
            <Icon name="alert" size={15} />
            <div>
              <strong>Some text may be hard to read.</strong>
              <ul>
                {#each warnings as w (w.fg + w.bg)}
                  <li>{t('%1 on %2: %3:1 (text needs 4.5:1)', t(LABELS[w.fg]), t(LABELS[w.bg]), w.ratio.toFixed(2))}</li>
                {/each}
              </ul>
            </div>
          </div>
        {/if}

        {#each TOKEN_GROUPS as group (group.label)}
          <fieldset class="group">
            <legend>{t(group.label)}</legend>
            <div class="tokens">
              {#each group.tokens as token (token.name)}
                {@const changed = editing.colors[token.name] !== undefined}
                {@const bad = typed[token.name] !== undefined}
                <div class="token" class:changed>
                  <input type="color" aria-label={t(token.label)} value={pickerValue(colors[token.name])} oninput={(e) => pick(token.name, (e.currentTarget as HTMLInputElement).value)} />
                  <label class="tlabel" for="tok-{token.name}">{t(token.label)}</label>
                  <input
                    id="tok-{token.name}"
                    type="text"
                    class="mono value"
                    class:invalid={bad}
                    aria-invalid={bad}
                    spellcheck="false"
                    maxlength="64"
                    value={typed[token.name] ?? colors[token.name]}
                    oninput={(e) => setColor(token.name, (e.currentTarget as HTMLInputElement).value)}
                  />
                  <button type="button" class="btn small ghost icon" title="Back to the colour of the base theme" aria-label="Back to the colour of the base theme" disabled={!changed && !bad} onclick={() => resetColor(token.name)}><Icon name="refresh" size={13} /></button>
                </div>
              {/each}
            </div>
          </fieldset>
        {/each}
        <p class="hint">Colours are #rgb, #rrggbb or #rrggbbaa, or rgb(), rgba(), hsl() and hsla() of numbers. A value that is not a colour is not used.</p>

        <div class="row end">
          <button type="button" class="btn ghost" onclick={() => editing && exportTheme({ ...editing, colors })}><Icon name="download" size={15} /> Export…</button>
          <button type="button" class="btn ghost" onclick={() => editing && copy({ ...editing, colors })}><Icon name="text" size={15} /> Copy as text</button>
          <span class="grow"></span>
          <button type="button" class="btn" onclick={cancel}>Cancel</button>
          <button type="button" class="btn primary" onclick={save}>Save theme</button>
        </div>
      </div>
    </section>
  {/if}
</div>

<style>
  .editor-page {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    padding-top: 1.1rem;
  }
  .intro {
    margin: 0;
    color: var(--text-muted);
    max-width: 52rem;
  }
  .toolbar {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-end;
    gap: 0.6rem;
  }
  .start {
    flex: 1 1 26rem;
    max-width: 38rem;
  }
  .start .row {
    flex-wrap: nowrap;
  }
  .start select {
    flex: 1;
    min-width: 0;
  }
  .row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
  }
  .row.end {
    border-top: 1px solid var(--line);
    padding-top: 0.9rem;
  }
  .paste textarea {
    width: 100%;
    resize: vertical;
  }
  .empty {
    margin: 0;
  }
  .mine {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .mine li {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.75rem;
    padding: 0.55rem 0.7rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
  }
  .mine li.chosen {
    border-color: var(--accent);
    box-shadow: inset 0 0 0 1px var(--accent);
  }
  .mine li.open {
    border-style: dashed;
  }
  .swatch {
    display: flex;
    width: 4.5rem;
    height: 2rem;
    flex: none;
    border-radius: var(--radius-s);
    overflow: hidden;
    border: 1px solid var(--line-strong);
  }
  .swatch span {
    flex: 1;
  }
  .who {
    display: flex;
    flex-direction: column;
    min-width: 8rem;
    flex: 1;
  }
  .who :global(.name) {
    font-weight: 600;
    overflow-wrap: anywhere;
  }
  .small {
    font-size: 0.82rem;
  }
  .actions {
    display: flex;
    flex-wrap: wrap;
    gap: 0.3rem;
  }
  .btn.danger {
    color: var(--negative);
  }
  .panel-head h2 {
    margin: 0;
    font-size: 1.05rem;
  }
  .panel-head {
    gap: 0.8rem;
    flex-wrap: wrap;
  }
  .cols {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(18rem, 100%), 1fr));
    gap: 1rem 1.5rem;
  }
  .warnings {
    display: flex;
    gap: 0.6rem;
    align-items: flex-start;
    padding: 0.6rem 0.9rem;
    border-radius: var(--radius-s);
    background: var(--warning-soft);
    color: var(--warning);
  }
  .warnings ul {
    margin: 0.3rem 0 0;
    padding-left: 1.1rem;
  }
  .group {
    margin: 0;
    padding: 0;
    border: 0;
  }
  .group legend {
    padding: 0;
    margin-bottom: 0.5rem;
    font-weight: 600;
    color: var(--text-muted);
    font-size: 0.85rem;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  .tokens {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(25rem, 100%), 1fr));
    gap: 0.45rem 1.2rem;
  }
  .token {
    display: grid;
    grid-template-columns: 2.2rem minmax(6rem, 1fr) minmax(8rem, 13.5rem) auto;
    align-items: center;
    gap: 0.5rem;
  }
  .token.changed .tlabel {
    font-weight: 600;
  }
  .token.changed .tlabel::after {
    content: ' •';
    color: var(--accent-fg);
  }
  .tlabel {
    font-size: 0.9rem;
    min-width: 0;
  }
  input[type='color'] {
    width: 2.2rem;
    height: 2rem;
    padding: 0.1rem;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-s);
    background-color: var(--surface-2);
    cursor: pointer;
  }
  .value {
    width: 100%;
    min-width: 0;
    font-size: 0.8rem;
  }
  .value.invalid {
    border-color: var(--negative);
    box-shadow: 0 0 0 1px var(--negative);
  }
  .hint {
    margin: 0;
    color: var(--text-muted);
    font-size: 0.86rem;
  }
  .error {
    margin: 0;
    color: var(--negative);
  }
</style>
