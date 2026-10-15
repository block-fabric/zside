// The interface in the languages of chains-qt (audit M6): the catalogs, the messages with numbers and
// plural forms in them, and the page translated where it shows them, but never the text of the user.
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { afterEach, describe, expect, it } from 'vitest';
import { render } from '@testing-library/svelte';
import { flushSync } from 'svelte';
import I18nHarness from './I18nHarness.svelte';
import { i18n, pluralIndex, refreshPage, setCatalog, t, tn } from '../lib/i18n.svelte';

const APP = join(__dirname, '../..');
const DE = { code: 'de', name: 'Deutsch', rtl: false, messages: { Send: 'Überweisen', 'Mask values': 'Werte ausblenden', '%1 behind': '%1 im Rückstand', 'Enter address or label to search': 'Zu suchende Adresse oder Bezeichnung eingeben', '%n GB of space available': ['%n GB freier Speicherplatz verfügbar', '%n GB freier Speicherplatz verfügbar'] } };
const RU = { code: 'ru', name: 'русский', rtl: false, messages: { 'Processed %n block(s) of transaction history.': ['Обработан %n блок', 'Обработано %n блока', 'Обработано %n блоков', 'Обработано %n блоков'] } };

afterEach(() => {
  setCatalog(null);
  document.body.innerHTML = '';
});

describe('the catalogs', () => {
  it('are made from the catalogs of src/webui/locale, and are up to date', () => {
    const index = JSON.parse(readFileSync(join(APP, 'public/locales/index.json'), 'utf8')) as { code: string; name: string; messages: number }[];
    expect(index.length).toBeGreaterThan(90);
    const de = JSON.parse(readFileSync(join(APP, 'public/locales/de.json'), 'utf8'));
    expect(de.name).toBe('Deutsch');
    expect(Object.keys(de.gui).length).toBeGreaterThan(100);
    // Normalized as the interface writes its messages: no mnemonics, an ellipsis, Chains.
    expect(Object.keys(de.gui).some((k) => /&(?=\S)|\.\.\./.test(k))).toBe(false);
    expect(Object.keys(de.gui).some((k) => /Bitcoin/.test(k))).toBe(false);
    expect(Object.keys(de.core).length).toBeGreaterThan(50);
    // Made again from the catalogs (src/webui/locale) and the sources, nothing changes.
    expect(existsSync(join(APP, '../locale/bitcoin_de.ts'))).toBe(true);
    execFileSync(process.execPath, [join(APP, 'scripts/i18n.mjs'), '--check'], { stdio: 'pipe' });
  });
});

describe('a language', () => {
  it('translates messages, with what is in them', () => {
    setCatalog(DE);
    expect(t('Send')).toBe('Überweisen');
    expect(tn('%n GB of space available', 12)).toBe('12 GB freier Speicherplatz verfügbar');
    expect(t('Nothing of the catalog')).toBe('Nothing of the catalog');
    expect(t('Mask values')).toBe('Werte ausblenden');
    expect(t('%1 behind', '2 days')).toBe('2 days im Rückstand');
    // Only the message of the app is looked up, as a whole: text put together is never matched, so a
    // part of it is never translated alone.
    expect(t('3 hours behind')).toBe('3 hours behind');
    expect(t('%1 hours behind', 3)).toBe('3 hours behind');
    expect(i18n.code).toBe('de');
  });

  it('chooses plural forms by the rules of the language', () => {
    setCatalog(RU);
    expect(pluralIndex('ru', 1, 4)).toBe(0);
    expect(pluralIndex('ru', 3, 4)).toBe(1);
    expect(pluralIndex('ru', 7, 4)).toBe(2);
    expect(tn('Processed %n block(s) of transaction history.', 21)).toBe('Обработан 21 блок');
    expect(tn('Processed %n block(s) of transaction history.', 5)).toBe('Обработано 5 блоков');
    // In English, the "(s)" of a message is the English plural.
    setCatalog(null);
    expect(tn('Processed %n block(s) of transaction history.', 1)).toBe('Processed 1 block of transaction history.');
    expect(tn('Processed %n block(s) of transaction history.', 5)).toBe('Processed 5 blocks of transaction history.');
    expect(pluralIndex('de', 1, 2)).toBe(0);
    expect(pluralIndex('de', 2, 2)).toBe(1);
  });

  it('translates the words of the templates, and only them (opt-in)', async () => {
    setCatalog(DE);
    const { container } = render(I18nHarness, { props: { hours: 5, label: 'Send' } });
    const $ = (s: string) => container.querySelector(s) as HTMLElement;
    expect($('button').textContent).toBe('Überweisen');
    expect($('button').title).toBe('Werte ausblenden');
    expect(($('input') as HTMLInputElement).placeholder).toBe('Zu suchende Adresse oder Bezeichnung eingeben');
    // A template is the message of the app: "%1 hours behind" is not "%1 behind" with "5 hours" in it.
    expect($('#behind').textContent).toBe('5 hours behind');
    expect($('#behind-own').textContent).toBe('5 im Rückstand');
    expect($('#cond').textContent).toBe('Überweisen');
    expect($('#tab-a').textContent).toBe('Werte ausblenden');
    // Text from outside: as it is, with or without SafeText (SafeText also says translate="no").
    expect($('#label').textContent).toBe('Send');
    expect($('#label [translate="no"]')).toBeTruthy();
    expect($('#bare').textContent).toBe('Send');
    expect($('#no').textContent).toBe('Send');
    expect($('code').textContent).toBe('Send');
    // Another language, and back to English: the page follows.
    setCatalog(null);
    flushSync();
    expect($('button').textContent).toBe('Send');
    expect($('#behind').textContent).toBe('5 hours behind');
    refreshPage();
    expect(document.documentElement.lang).toBe('en');
    setCatalog({ code: 'ar', name: 'العربية', rtl: true, messages: {} });
    refreshPage();
    expect(document.documentElement.dir).toBe('rtl');
  });
});
