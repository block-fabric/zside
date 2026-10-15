// The themes of the user (Settings → Advanced theme): the strict reading of theme files (they are
// untrusted), how a theme is drawn (known tokens only, through style.setProperty), the library kept in
// the preferences, and import and export through the file chooser of the desktop.
import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { themeTokens } from '../lib/contrast';
import {
  applyLook,
  baseColors,
  BASE_THEMES,
  checkStoredThemes,
  choiceIsDark,
  contrastWarnings,
  isColor,
  isThemeChoice,
  MAX_CUSTOM_THEMES,
  MAX_THEME_FILE,
  parseColor,
  parseThemeFile,
  THEME_FORMAT,
  themeFileText,
  ThemeFileError,
  TOKENS,
  type CustomTheme,
} from '../lib/customthemes';
import { answer, dialogs } from '../lib/dialogs.svelte';
import { bytesToBase64 } from '../lib/files';
import { nav } from '../lib/nav.svelte';
import { draft } from '../lib/settingsdraft.svelte';
import { app, applyPrefs, prefs } from '../lib/state.svelte';
import { THEMES } from '../lib/themes';
import { copyTheme, deleteTheme, duplicateTheme, exportFile, importFile, importText, look, newTheme, saveTheme } from '../lib/themestore.svelte';

const CSS = readFileSync(join(dirname(fileURLToPath(import.meta.url)), '../styles/themes.css'), 'utf8');

/** A good theme file, as an object to spoil. */
function good(): Record<string, unknown> {
  return {
    format: THEME_FORMAT,
    version: 1,
    name: 'Night owl',
    base: 'dracula',
    colors: { bg: '#101018', surface: '#181822', text: '#f0f0f0', accent: 'rgb(200 120 255)', ambient: 'rgba(200, 120, 255, 0.12)', 'card-top': 'hsl(240 30% 90% / 3%)', link: 'hsla(190, 90%, 70%, 1)', 'accent-soft': '#2a2040ff', 'text-muted': '#bbc' },
  };
}
const file = (o: unknown) => JSON.stringify(o);
const rejects = (o: unknown) => expect(() => parseThemeFile(typeof o === 'string' ? o : file(o)), JSON.stringify(o).slice(0, 120)).toThrow(ThemeFileError);

/** The host: prefs.set kept, files by path, the clipboard. */
function fakeHost(files: Record<string, string> = {}): { calls: BridgeRequest[]; files: Record<string, string>; clipboard: string[]; chosen: (string | null)[] } {
  const calls: BridgeRequest[] = [];
  const clipboard: string[] = [];
  const chosen: (string | null)[] = [];
  setTransport(async (r) => {
    calls.push(r);
    const p = (r.params ?? {}) as Record<string, unknown>;
    switch (r.method) {
      case 'prefs.set':
        return {};
      case 'file.choose':
        return chosen.length ? chosen.shift() : null;
      case 'file.read': {
        const path = p.path as string;
        if (!(path in files)) throw { code: -32600, message: 'Only a file chosen by the user can be read' };
        const bytes = new TextEncoder().encode(files[path]);
        return { path, size: bytes.length, base64: bytesToBase64(bytes) };
      }
      case 'file.write':
        files[p.path as string] = p.text as string;
        return p.path;
      case 'clipboard.write':
        clipboard.push(p.text as string);
        return true;
      case 'clipboard.read':
        return clipboard[clipboard.length - 1] ?? null;
      case 'options.get':
        return { ...OPTIONS };
      case 'tray.state':
        return { available: true, visible: true, windowVisible: true };
      case 'languages':
        return [];
      default:
        throw { code: -32601, message: `No ${r.method}` };
    }
  });
  return { calls, files, clipboard, chosen };
}

const OPTIONS = {
  dbcache: 450, par: 0, prune: false, pruneSize: 2, spendZeroConf: true, signer: '', natpmp: false, listen: true, server: false,
  proxyUse: false, proxyIp: '127.0.0.1', proxyPort: '9050', torUse: false, torIp: '127.0.0.1', torPort: '9050', lang: '', startAtLogin: false,
  ignored: {}, overridden: '', limits: { dbcacheMin: 4, parMin: -8, parMax: 15, pruneMinGB: 1 }, proxyReach: { ipv4: false, ipv6: false, tor: false },
  externalSigner: false, restartRequired: false, pending: 0,
};

beforeAll(() => {
  const proto = HTMLDialogElement.prototype as unknown as Record<string, unknown>;
  proto.showModal ??= function (this: HTMLDialogElement) {
    this.setAttribute('open', '');
  };
  proto.close ??= function (this: HTMLDialogElement) {
    this.removeAttribute('open');
  };
  window.matchMedia ??= ((query: string) => ({ matches: false, media: query, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia;
});

beforeEach(() => {
  prefs.customThemes = [];
  prefs.theme = 'midnight';
  look.preview = null;
  for (const k of Object.keys(draft.prefs)) delete (draft.prefs as Record<string, unknown>)[k];
  for (const k of Object.keys(draft.original)) delete (draft.original as Record<string, unknown>)[k];
});

afterEach(() => {
  setTransport(null);
  dialogs.queue.splice(0);
});

describe('colours', () => {
  it('reads the colours of the grammar, and nothing else', () => {
    for (const ok of ['#abc', '#AABBCC', '#aabbcc80', 'rgb(1, 2, 3)', 'rgba(1,2,3,0.5)', 'rgb(1 2 3)', 'rgb(1 2 3 / 50%)', 'rgb(10% 20% 30%)', 'hsl(120, 50%, 50%)', 'hsla(120 50% 50% / 0.2)', 'rgb(255 255 255 / 2.5%)']) {
      expect(isColor(ok), ok).toBe(true);
    }
    for (const bad of ['', 'red', 'transparent', '#ab', '#abcd', '#abcdeg', 'url(x.png)', 'url("data:image/svg+xml,<svg/>")', 'var(--accent)', 'calc(1px + 2px)', 'rgb(1,2,3);', '#fff; background: url(x)', '#fff}', '{#fff', 'rgb(256, 0, 0)', 'rgb(1, 2)', 'rgb(1, 2, 3, 4, 5)', 'rgb(1 2 3 / 2)', 'rgb(-1 2 3)', 'rgb(1e2 2 3)', 'hsl(120deg 50% 50%)', 'hsl(120 50 50)', 'rgb(1, 2 3)', 'rgb(1 2 3 / 0.5 / 1)', 'expression(alert(1))', 'rgb(1 2 3)) url(x', '#'.padEnd(80, 'a'), null, 12, {}]) {
      expect(isColor(bad), String(bad)).toBe(false);
    }
    expect(parseColor('hsl(0 100% 50%)')).toEqual({ r: 255, g: 0, b: 0, a: 1 });
    expect(parseColor('#11223380')).toEqual({ r: 0x11, g: 0x22, b: 0x33, a: 0.502 });
  });

  it('every colour of every built-in theme is in the grammar, and every token is set by every theme', () => {
    const all = themeTokens(CSS);
    for (const base of BASE_THEMES) {
      for (const token of TOKENS) {
        expect(all[base][token], `${base} --${token}`).toBeTruthy();
        expect(isColor(all[base][token]), `${base} --${token}: ${all[base][token]}`).toBe(true);
      }
      expect(baseColors(base).bg).toBe(all[base].bg);
    }
  });

  it('warns, without refusing, when text is hard to read', () => {
    expect(contrastWarnings(baseColors('mocha'))).toEqual([]);
    expect(contrastWarnings(baseColors('dracula'))).toEqual([]);
    const low = contrastWarnings({ ...baseColors('midnight'), text: '#333333' });
    expect(low.map((w) => `${w.fg}/${w.bg}`)).toEqual(['text/bg', 'text/surface']);
    expect(low[0].ratio).toBeLessThan(4.5);
  });
});

describe('theme files (untrusted)', () => {
  it('reads a good file: its name, its base, its colours', () => {
    const t = parseThemeFile(file(good()));
    expect(t.name).toBe('Night owl');
    expect(t.base).toBe('dracula');
    expect(t.colors.bg).toBe('#101018');
    expect(t.colors.accent).toBe('rgb(200 120 255)');
    // No base: Midnight; no colours: the base as it is.
    expect(parseThemeFile(file({ format: THEME_FORMAT, version: 1, name: 'Plain' }))).toEqual({ name: 'Plain', base: 'midnight', colors: {} });
    // A byte-order mark in front is fine.
    expect(parseThemeFile(`﻿${file(good())}`).name).toBe('Night owl');
  });

  it('refuses each thing that is not a theme', () => {
    rejects('');
    rejects('not json');
    rejects('[1, 2]');
    rejects('"a string"');
    rejects('null');
    rejects({ ...good(), format: 'something-else' });
    rejects({ ...good(), format: undefined });
    rejects({ ...good(), version: 2 });
    rejects({ ...good(), version: '1' });
    rejects({ ...good(), extra: 1 });
    rejects({ ...good(), css: 'body { display: none }' });
    rejects({ ...good(), name: undefined });
    rejects({ ...good(), name: '' });
    rejects({ ...good(), name: '   ' });
    rejects({ ...good(), name: 42 });
    rejects({ ...good(), name: 'x'.repeat(49) });
    rejects({ ...good(), name: 'bad\u0007bell' });
    rejects({ ...good(), name: 'line\nbreak' });
    rejects({ ...good(), name: 'evil‮gnp.exe' });
    rejects({ ...good(), base: 'system' });
    rejects({ ...good(), base: 'nope' });
    rejects({ ...good(), colors: [] });
    rejects({ ...good(), colors: 'bg: red' });
    rejects({ ...good(), colors: { bg: '#000', unknown: '#fff' } });
    rejects({ ...good(), colors: { chevron: '#fff' } });
    rejects({ ...good(), colors: { 'font-ui': '#fff' } });
    rejects({ ...good(), colors: { shadow: '#fff' } });
    rejects({ ...good(), colors: { bg: 'url(https://example.com/x.png)' } });
    rejects({ ...good(), colors: { bg: 'var(--accent)' } });
    rejects({ ...good(), colors: { bg: 'calc(1 + 1)' } });
    rejects({ ...good(), colors: { bg: 'expression(alert(1))' } });
    rejects({ ...good(), colors: { bg: '#000; color: red' } });
    rejects({ ...good(), colors: { bg: '#000 } body { display: none' } });
    rejects({ ...good(), colors: { bg: 'red' } });
    rejects({ ...good(), colors: { bg: 0 } });
    rejects({ ...good(), colors: { bg: ['#000'] } });
    rejects('{"format":"chains-app-theme","version":1,"name":"p","colors":{"__proto__":"#000"}}');
    rejects('{"format":"chains-app-theme","version":1,"name":"p","__proto__":{"x":1}}');
    // Larger than 64 KB, even when it would read.
    rejects(file({ ...good(), name: 'Big' }) + ' '.repeat(MAX_THEME_FILE));
  });

  it('writes files it reads back the same (export, import)', () => {
    const t = parseThemeFile(file(good()));
    const text = themeFileText(t);
    expect(JSON.parse(text).format).toBe(THEME_FORMAT);
    expect(parseThemeFile(text)).toEqual(t);
  });
});

describe('the themes of the user in the preferences', () => {
  it('keeps only stored themes that still read, at most 50, and a choice only of a theme that exists', () => {
    const one = { id: 'custom-a1', name: 'One', base: 'mocha', colors: { bg: '#000000' } };
    const stored = [one, { ...one }, { id: 'custom-b2', name: 'Bad', base: 'mocha', colors: { bg: 'url(x)' } }, { id: 'nope', name: 'Bad id', base: 'mocha', colors: {} }, { id: 'custom-c3', name: '<img src=x onerror=alert(1)>', base: 'light', colors: {} }];
    expect(checkStoredThemes(stored).map((x) => x.id)).toEqual(['custom-a1', 'custom-c3']);
    const many = Array.from({ length: 70 }, (_, i) => ({ id: `custom-n${i}`, name: `T${i}`, base: 'dark', colors: {} }));
    expect(checkStoredThemes(many)).toHaveLength(MAX_CUSTOM_THEMES);
    applyPrefs({ customThemes: stored, theme: 'custom-c3' });
    expect(prefs.customThemes.map((x) => x.id)).toEqual(['custom-a1', 'custom-c3']);
    expect(prefs.theme).toBe('custom-c3');
    applyPrefs({ theme: 'custom-zz' });
    expect(prefs.theme).toBe('custom-c3');
    expect(isThemeChoice('mocha', [])).toBe(true);
    expect(isThemeChoice('dracula', [])).toBe(true);
    expect(isThemeChoice('custom-a1', [])).toBe(false);
  });

  it('creates, saves, renames, duplicates, chooses and deletes themes', () => {
    const host = fakeHost();
    const fresh = newTheme({ name: 'Mine', base: 'mocha', colors: { accent: '#ff8800' } });
    const saved = saveTheme(fresh);
    expect(prefs.customThemes).toEqual([saved]);
    expect(host.calls.filter((c) => c.method === 'prefs.set').at(-1)?.params).toEqual({ customThemes: [saved] });
    // A second one of the same name gets a free name.
    const other = saveTheme(newTheme({ name: 'Mine', base: 'light', colors: {} }));
    expect(other.name).toBe('Mine (2)');
    // Rename.
    saveTheme({ ...saved, name: 'Renamed' });
    expect(prefs.customThemes.map((x) => x.name)).toEqual(['Renamed', 'Mine (2)']);
    // A bad colour is never saved.
    expect(() => saveTheme({ ...saved, colors: { bg: 'url(x)' } as CustomTheme['colors'] })).toThrow(ThemeFileError);
    const copy = duplicateTheme(saved.id)!;
    expect(copy.id).not.toBe(saved.id);
    expect(copy.colors).toEqual({ accent: '#ff8800' });
    expect(prefs.customThemes).toHaveLength(3);
    // Chosen, then deleted: the look goes back to its base.
    prefs.theme = saved.id;
    expect(choiceIsDark(saved.id, false, prefs.customThemes)).toBe(true);
    deleteTheme(saved.id);
    expect(prefs.customThemes.map((x) => x.id)).toEqual([other.id, copy.id]);
    expect(prefs.theme).toBe('mocha');
    expect(host.calls.filter((c) => c.method === 'prefs.set').at(-1)?.params).toMatchObject({ theme: 'mocha' });
    // No more than 50.
    while (prefs.customThemes.length < MAX_CUSTOM_THEMES) saveTheme(newTheme({ name: 'Many', base: 'dark', colors: {} }));
    expect(() => saveTheme(newTheme({ name: 'One more', base: 'dark', colors: {} }))).toThrow(ThemeFileError);
    expect(() => importText(themeFileText({ name: 'One more', base: 'dark', colors: {} }))).toThrow(ThemeFileError);
  });

  it('draws a theme of the user as its base with its colours set one by one, and takes them off again', () => {
    const doc = document;
    const theme: CustomTheme = { id: 'custom-x1', name: 'X', base: 'dracula', colors: { bg: '#f4f4f4', 'text-muted': 'rgb(80 80 80)', accent: '#123456' } };
    applyLook(doc, theme.id, [theme], null, 0, false);
    const root = doc.documentElement;
    expect(root.dataset.theme).toBe('dracula');
    expect(root.dataset.themeChoice).toBe('custom');
    expect(root.style.getPropertyValue('--bg')).toBe('#f4f4f4');
    expect(root.style.getPropertyValue('--accent')).toBe('#123456');
    expect(root.style.getPropertyValue('--chevron')).toContain("stroke='%23505050'");
    // A light page: the form controls are drawn light.
    expect(root.style.colorScheme).toBe('light');
    // Only the known tokens are ever set, whatever the object holds.
    const sneaky = { ...theme, colors: { ...theme.colors, 'font-ui': 'Comic Sans', bg: 'url(x)' } } as unknown as CustomTheme;
    applyLook(doc, theme.id, [sneaky], null, 0, false);
    expect(root.style.getPropertyValue('--font-ui')).toBe('');
    expect(root.style.getPropertyValue('--bg')).toBe('');
    // The preview wins over the choice; a built-in look takes every colour of the theme off.
    applyLook(doc, 'midnight', [], theme, 0, false);
    expect(root.style.getPropertyValue('--accent')).toBe('#123456');
    applyLook(doc, 'mocha', [theme], null, 0, false);
    expect(root.dataset.theme).toBe('mocha');
    expect(root.style.getPropertyValue('--accent')).toBe('');
    expect(root.style.getPropertyValue('--chevron')).toBe('');
    // A theme that no longer exists: the default look.
    applyLook(doc, 'custom-gone', [], null, 0, false);
    expect(root.dataset.theme).toBe('midnight');
  });
});

describe('import and export through the file chooser of the desktop', () => {
  it('round-trips a theme through a file, and the clipboard', async () => {
    const host = fakeHost();
    const theme = saveTheme(newTheme({ name: 'Round trip', base: 'mocha', colors: { accent: '#fab387', bg: '#101010' } }));
    host.chosen.push('/home/u/round.json');
    expect(await exportFile(theme)).toBe('/home/u/round.json');
    const choose = host.calls.find((c) => c.method === 'file.choose')!;
    expect(choose.params).toMatchObject({ save: true, name: 'Round trip.json' });
    const written = host.files['/home/u/round.json'];
    expect(JSON.parse(written)).toEqual({ format: THEME_FORMAT, version: 1, name: 'Round trip', base: 'mocha', colors: { bg: '#101010', accent: '#fab387' } });
    // Read back: a new theme, under a free name, with the same colours.
    host.chosen.push('/home/u/round.json');
    const imported = (await importFile())!;
    expect(imported.id).not.toBe(theme.id);
    expect(imported.name).toBe('Round trip (2)');
    expect(imported.base).toBe('mocha');
    expect(imported.colors).toEqual(theme.colors);
    // The page reads only the path the chooser gave.
    expect(host.calls.filter((c) => c.method === 'file.read').map((c) => c.params)).toEqual([{ path: '/home/u/round.json' }]);
    // Cancelled: nothing.
    expect(await importFile()).toBeNull();
    // As text on the clipboard.
    await copyTheme(theme);
    expect(parseThemeFile(host.clipboard[0]).colors).toEqual(theme.colors);
  });

  it('refuses a file too large, not UTF-8, or not a theme, and keeps nothing of it', async () => {
    const host = fakeHost({ '/big.json': file(good()) + ' '.repeat(MAX_THEME_FILE), '/evil.json': file({ ...good(), colors: { bg: 'url(javascript:alert(1))' } }) });
    host.chosen.push('/big.json', '/evil.json');
    await expect(importFile()).rejects.toThrow(ThemeFileError);
    await expect(importFile()).rejects.toThrow(ThemeFileError);
    // Bytes that are not UTF-8.
    setTransport(async (r) => (r.method === 'file.choose' ? '/bin.json' : { path: '/bin.json', size: 3, base64: bytesToBase64(new Uint8Array([0xff, 0xfe, 0xfd])) }));
    await expect(importFile()).rejects.toThrow(ThemeFileError);
    expect(prefs.customThemes).toEqual([]);
  });
});

describe('the Advanced theme tab of Settings', () => {
  it('creates a theme drawn live, cancels it back, saves one, and deletes it after the confirmation', async () => {
    fakeHost();
    app.info = { name: 'Chains', version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp/x', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 600 } as typeof app.info;
    const { default: Settings } = await import('../pages/Settings.svelte');
    nav.page = 'settings';
    nav.tab = 'theme';
    const view = render(Settings);
    expect(await screen.findByRole('tab', { name: 'Advanced theme', selected: true })).toBeTruthy();
    // Every built-in look can be started from, the new ones too.
    const start = screen.getByLabelText('Start from') as HTMLSelectElement;
    const options = [...start.querySelectorAll('option')].map((o) => o.textContent);
    expect(options).toEqual(THEMES.filter((x) => x.id !== 'system').map((x) => x.name));
    expect(options).toContain('Catppuccin Mocha');
    expect(options).toContain('Dracula');
    await fireEvent.change(start, { target: { value: 'dracula' } });
    await fireEvent.click(screen.getByRole('button', { name: /New theme/ }));
    const editor = await screen.findByRole('region', { name: 'Theme editor' });
    // Drawn on the page while it is edited.
    await waitFor(() => expect(look.preview?.base).toBe('dracula'));
    const accent = within(editor).getByLabelText('Accent (buttons, the current page)', { selector: 'input[type="text"]' }) as HTMLInputElement;
    expect(accent.value).toBe('#d7263d');
    await fireEvent.input(accent, { target: { value: '#ff5500' } });
    await waitFor(() => expect(look.preview?.colors.accent).toBe('#ff5500'));
    // Not a colour: shown as typed, not drawn.
    await fireEvent.input(accent, { target: { value: 'url(x)' } });
    expect(accent.getAttribute('aria-invalid')).toBe('true');
    expect(look.preview?.colors.accent).toBe('#ff5500');
    // Low contrast: a warning, Save still possible.
    const text = within(editor).getByLabelText('Text', { selector: 'input[type="text"]' }) as HTMLInputElement;
    await fireEvent.input(text, { target: { value: '#2a2a2a' } });
    expect(await within(editor).findByText('Some text may be hard to read.')).toBeTruthy();
    // Cancel: nothing kept, the look as it was.
    await fireEvent.click(within(editor).getByRole('button', { name: 'Cancel' }));
    await waitFor(() => expect(look.preview).toBeNull());
    expect(prefs.customThemes).toEqual([]);
    // Again, named, saved: in the list (its name as text, never translated), chosen.
    await fireEvent.click(screen.getByRole('button', { name: /New theme/ }));
    const editor2 = await screen.findByRole('region', { name: 'Theme editor' });
    const name = within(editor2).getByLabelText('Name') as HTMLInputElement;
    await fireEvent.input(name, { target: { value: '<b>Bold</b> theme' } });
    await fireEvent.click(within(editor2).getByRole('button', { name: 'Save theme' }));
    await waitFor(() => expect(prefs.customThemes).toHaveLength(1));
    expect(prefs.customThemes[0]).toMatchObject({ name: '<b>Bold</b> theme', base: 'dracula' });
    expect(prefs.theme).toBe(prefs.customThemes[0].id);
    expect(draft.prefs.theme).toBe(prefs.customThemes[0].id);
    const list = screen.getByRole('list', { name: 'Your themes' });
    const shown = within(list).getByText('<b>Bold</b> theme');
    expect(shown.getAttribute('translate')).toBe('no');
    expect(list.querySelector('b')).toBeNull();
    // Delete: asked first (a dangerous button); No keeps it.
    await fireEvent.click(within(list).getByRole('button', { name: 'Delete' }));
    await waitFor(() => expect(dialogs.queue).toHaveLength(1));
    const q = dialogs.queue[0] as { message: string; choices: { id: string; kind?: string }[] };
    expect(q.message).toContain('<b>Bold</b> theme');
    expect(q.choices.find((c) => c.id === 'yes')?.kind).toBe('danger');
    answer(dialogs.queue[0].id, { choice: null, checked: false });
    await waitFor(() => expect(dialogs.queue).toHaveLength(0));
    expect(prefs.customThemes).toHaveLength(1);
    await fireEvent.click(within(list).getByRole('button', { name: 'Delete' }));
    await waitFor(() => expect(dialogs.queue).toHaveLength(1));
    answer(dialogs.queue[0].id, { choice: 'yes', checked: false });
    await waitFor(() => expect(prefs.customThemes).toHaveLength(0));
    expect(prefs.theme).toBe('dracula');
    view.unmount();
    expect(look.preview).toBeNull();
  });
});
