#!/usr/bin/env node
// The translations of chains-app, made from the catalogs in src/webui/locale (bitcoin_*.ts, the work of the
// translators of Bitcoin Core on Transifex, as chains-qt shipped them; `tx pull` updates them, see
// doc/translation_process.md).
//
//   node scripts/i18n.mjs            write public/locales/<language>.json and public/locales/index.json
//   node scripts/i18n.mjs --check    exit 1 if they are not up to date, or the catalogs are missing (the tests)
//   node scripts/i18n.mjs --if-present   as the first, but only if the catalogs are there (npm run build, also
//                                        from a copy of src/webui/app alone)
//
// A catalog keeps the messages of the interface (the "gui" part) that the interface of chains-app shows
// (the text of its sources, and of the host in src/webui), and the messages of the node ("core", the
// context bitcoin-core) that chains-app shows through G_TRANSLATION_FUN. Messages are normalized as
// chains-app writes them: no mnemonic "&", "…" for "...", "Chains" for "Bitcoin". A message with plural
// forms (%n) keeps them all, in the order of the CLDR categories of the language.
import { existsSync, mkdirSync, readFileSync, readdirSync, unlinkSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const APP = join(dirname(fileURLToPath(import.meta.url)), '..');
// The chain of the app (src/chain/chain.json): its name takes the place of "Bitcoin", its scheme that of "bitcoin:".
const CHAIN = JSON.parse(readFileSync(join(APP, 'src/chain/chain.json'), 'utf8'));
const CATALOGS = join(APP, '../locale');
const OUT = join(APP, 'public/locales');
const RTL = new Set(['ar', 'fa', 'he', 'ur', 'ps', 'ug', 'sd', 'ku_IQ', 'yi']);

function unescapeXml(s) {
  return s
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&apos;/g, "'")
    .replace(/&#x([0-9a-f]+);/gi, (_, h) => String.fromCodePoint(parseInt(h, 16)))
    .replace(/&#(\d+);/g, (_, d) => String.fromCodePoint(Number(d)))
    .replace(/&amp;/g, '&');
}

/** As the app writes a message: no mnemonic, an ellipsis, the name of the chain. */
export function normalize(s) {
  return s
    .replace(/&&/g, '\u0000')
    .replace(/&(?=\S)/g, '')
    .replace(/\u0000/g, '&')
    .replace(/\.\.\./g, '…')
    .replace(/\bBitcoin Core\b/g, CHAIN.name)
    .replace(/\bBitcoin\b/g, CHAIN.name)
    .replace(/\bbitcoin:/g, `${CHAIN.uriScheme}:`)
    .trim();
}

/** The messages of a catalog: [{context, source, translation | forms}], the finished ones only. */
export function parseCatalog(xml) {
  const out = [];
  for (const ctx of xml.matchAll(/<context>\s*<name>([^<]*)<\/name>([\s\S]*?)<\/context>/g)) {
    const context = ctx[1];
    for (const m of ctx[2].matchAll(/<message( numerus="yes")?>([\s\S]*?)<\/message>/g)) {
      const body = m[2];
      const source = body.match(/<source>([\s\S]*?)<\/source>/);
      const tr = body.match(/<translation( type="[^"]*")?>([\s\S]*?)<\/translation>|<translation( type="[^"]*")?\s*\/>/);
      if (!source || !tr || (tr[1] && /unfinished|obsolete|vanished/.test(tr[1])) || tr[3]) continue;
      if (m[1]) {
        const forms = [...tr[2].matchAll(/<numerusform>([\s\S]*?)<\/numerusform>/g)].map((f) => normalize(unescapeXml(f[1])));
        if (forms.length === 0 || forms.some((f) => !f)) continue;
        out.push({ context, source: normalize(unescapeXml(source[1])), forms });
      } else {
        const translation = normalize(unescapeXml(tr[2]));
        if (!translation) continue;
        out.push({ context, source: normalize(unescapeXml(source[1])), translation });
      }
    }
  }
  return out;
}

/** All the text of the interface and of the host: what decides which messages a catalog keeps. */
function appText() {
  const parts = [];
  const walk = (dir) => {
    for (const name of readdirSync(dir, { withFileTypes: true })) {
      const path = join(dir, name.name);
      if (name.isDirectory()) {
        if (!['node_modules', 'dist', 'test', 'locale', 'locales', 'webview'].includes(name.name)) walk(path);
      } else if (/\.(svelte|ts|cpp|h)$/.test(name.name) && !/\.test\.ts$/.test(name.name)) {
        parts.push(readFileSync(path, 'utf8'));
      }
    }
  };
  walk(join(APP, 'src'));
  walk(join(APP, '..'));
  // Decoded as the strings would show (escaped quotes and apostrophes).
  return parts.join('\n').replace(/\\'/g, "'").replace(/\\"/g, '"');
}

/** Whether the interface can show a message: verbatim, or its fixed parts around the %1, %n. */
export function used(source, text) {
  if (source.length < 2) return false;
  if (!/%[0-9n]/.test(source)) return text.includes(source);
  const parts = source
    .split(/%[0-9n]/)
    .map((p) => p.trim())
    .filter((p) => p.length >= 3);
  return parts.length > 0 && parts.every((p) => text.includes(p));
}

/** A translation without its colon at the end (also the full-width one), as the label it goes with. */
function stripColon(value) {
  const strip = (t) => t.replace(/\s*[:：]\s*$/, '');
  return Array.isArray(value) ? value.map(strip) : strip(value);
}

function languageName(code) {
  const tag = code.replace('_', '-').replace(/@latin$/, '-Latn').replace(/@.*$/, '');
  try {
    const name = new Intl.DisplayNames([tag], { type: 'language' }).of(tag);
    if (name && name !== tag) return name.charAt(0).toLocaleUpperCase(tag) + name.slice(1);
  } catch {
    // An unknown tag: its code.
  }
  return code;
}

export function build() {
  const text = appText();
  const memo = new Map();
  const isUsed = (source) => {
    let u = memo.get(source);
    if (u === undefined) memo.set(source, (u = used(source, text)));
    return u;
  };
  const files = readdirSync(CATALOGS).filter((f) => /^bitcoin_.+\.ts$/.test(f)).sort();
  const index = [];
  const outputs = {};
  for (const file of files) {
    const code = file.slice('bitcoin_'.length, -'.ts'.length);
    const messages = parseCatalog(readFileSync(join(CATALOGS, file), 'utf8'));
    const gui = {};
    const core = {};
    for (const m of messages) {
      const value = m.forms ?? m.translation;
      if (m.context === 'bitcoin-core') {
        core[m.source] = value;
        continue;
      }
      // The labels of the forms of chains-qt end with a colon ("Pay To:"); the interface writes them without.
      const bare = m.source.replace(/\s*:$/, '');
      const variants = bare !== m.source && bare.length > 1 ? [[m.source, value], [bare, stripColon(value)]] : [[m.source, value]];
      for (const [source, v] of variants) {
        if (isUsed(source) && !(source in gui)) gui[source] = v;
      }
    }
    if (Object.keys(gui).length === 0 && Object.keys(core).length === 0) continue;
    const entry = { code, name: languageName(code), rtl: RTL.has(code) || RTL.has(code.split('_')[0]) };
    index.push({ ...entry, messages: Object.keys(gui).length });
    outputs[`${code}.json`] = JSON.stringify({ ...entry, gui, core }) + '\n';
  }
  outputs['index.json'] = JSON.stringify(index, null, 1) + '\n';
  return outputs;
}

function main() {
  if (!existsSync(CATALOGS)) {
    console.error(`No catalogs in ${CATALOGS}: the translations stay as they are.`);
    process.exit(process.argv.includes('--if-present') ? 0 : 1);
  }
  const outputs = build();
  if (process.argv.includes('--check')) {
    const stale = Object.entries(outputs).filter(([name, data]) => !existsSync(join(OUT, name)) || readFileSync(join(OUT, name), 'utf8') !== data);
    const extra = existsSync(OUT) ? readdirSync(OUT).filter((f) => !(f in outputs)) : [];
    if (stale.length || extra.length) {
      console.error(`The translations are not up to date (run npm run i18n): ${[...stale.map(([n]) => n), ...extra].join(', ')}`);
      process.exit(1);
    }
    return;
  }
  mkdirSync(OUT, { recursive: true });
  for (const f of readdirSync(OUT)) if (!(f in outputs)) unlinkSync(join(OUT, f));
  for (const [name, data] of Object.entries(outputs)) writeFileSync(join(OUT, name), data);
  const index = JSON.parse(outputs['index.json']);
  console.log(`${index.length} languages, ${index.reduce((a, l) => a + l.messages, 0)} messages of the interface in all`);
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) main();
