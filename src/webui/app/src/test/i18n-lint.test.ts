// The lint of the opt-in translation (security audit 2026-10-14, item 3): every field that holds words
// and is shown as text says whose words they are. The app's own (a tab, a menu item, a title held in
// code) are shown with t() / tn(); text from the chain, peers, the wallet or the user (a label, a
// memo, the title of a sidechain, the version of a peer, a URI) with <SafeText>, which is never
// translated and carries translate="no". A field shown bare is an error here: whoever adds one decides.
// Messages with values are translated where they are made, with their own placeholders: t() is never
// given text put together (a call, a template with values, a sum), and the words of the app put in a
// toast or a dialog go through t() there (the toasts and dialogs show what they get as it is).
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';
import { parse } from 'svelte/compiler';
import { describe, expect, it } from 'vitest';

const SRC = join(__dirname, '..');

/** The names of fields that hold words. */
export const TEXT_FIELDS = new Set(['label', 'title', 'name', 'description', 'message', 'memo', 'comment', 'subver', 'subversion', 'uri', 'text', 'hint', 'tip', 'informative', 'heading', 'caption', 'purpose', 'note', 'summary', 'detail', 'details']);
/** Fields that only ever hold text from outside the app: never in t(). */
export const UNTRUSTED_FIELDS = new Set(['memo', 'comment', 'subver', 'subversion', 'uri']);
const TRANSLATE = new Set(['t', 'tn']);
/** Functions that give words of the app already in the language shown (made with t() / tn()). */
const SELF_TRANSLATED = new Set(['typeText', 'statusText', 'voteText', 'summaryText', 'signersText', 'pingText', 'rangeText', 'rateText', 'noWalletText', 'nodeWords', 'addressTypeText', 'pageTitle']);
/** Where a toast or a dialog of the app takes its words: the arguments (by place) and the fields of an object argument. */
const MESSAGE_CALLS: Record<string, { args: number[]; props: string[] }> = {
  toast: { args: [1, 2], props: [] },
  inform: { args: [0, 1, 3], props: [] },
  confirm: { args: [0, 1], props: ['informative'] },
  prompt: { args: [0, 1], props: ['message'] },
  ask: { args: [], props: ['title', 'message', 'informative'] },
  warn: { args: [0], props: [] },
};
/** The name of the app (from the host), put in sentences of the app: not text from outside. */
const ALLOWED = new Set(["app.info?.name ?? 'Chains'", 'app.info?.name ?? CHAIN.name', 'CHAIN.name', 'CHAIN.mainchain?.name']);

interface Finding {
  file: string;
  line: number;
  code: string;
  why: string;
}

function svelteFiles(dir: string, out: string[] = []): string[] {
  for (const f of readdirSync(dir)) {
    const p = join(dir, f);
    if (statSync(p).isDirectory()) svelteFiles(p, out);
    else if (p.endsWith('.svelte')) out.push(p);
  }
  return out;
}

type Node = { type: string; [key: string]: unknown };

/** The fields of words an expression shows as it is (not through a call). */
function shownFields(expr: Node | undefined, out: string[]): string[] {
  if (!expr) return out;
  switch (expr.type) {
    case 'MemberExpression': {
      const prop = expr.property as Node;
      if (!expr.computed && prop.type === 'Identifier' && TEXT_FIELDS.has(prop.name as string)) out.push(prop.name as string);
      break;
    }
    case 'Identifier':
      if (/^[A-Z][A-Z0-9_]+$/.test(expr.name as string)) out.push(expr.name as string);
      break;
    case 'CallExpression': {
      // What a function of words gives (typeText(row.type), statusLabel(tx)): words too.
      const callee = expr.callee as Node;
      const name = callee.type === 'Identifier' ? (callee.name as string) : callee.type === 'MemberExpression' && (callee.property as Node).type === 'Identifier' ? ((callee.property as Node).name as string) : '';
      if (/(Text|Label|Title|Name|Words|Message)$/.test(name) && !TRANSLATE.has(name) && !SELF_TRANSLATED.has(name)) out.push(`${name}()`);
      break;
    }
    case 'ChainExpression':
    case 'ParenthesizedExpression':
    case 'TSAsExpression':
    case 'TSNonNullExpression':
      shownFields(expr.expression as Node, out);
      break;
    case 'ConditionalExpression':
      shownFields(expr.consequent as Node, out);
      shownFields(expr.alternate as Node, out);
      break;
    case 'LogicalExpression':
      shownFields(expr.left as Node, out);
      shownFields(expr.right as Node, out);
      break;
    case 'TemplateLiteral':
      for (const e of expr.expressions as Node[]) shownFields(e, out);
      break;
    case 'BinaryExpression':
      if (expr.operator === '+') {
        shownFields(expr.left as Node, out);
        shownFields(expr.right as Node, out);
      }
      break;
    default:
      break;
  }
  return out;
}

/** Untrusted fields put in a call of t(): their text would be looked up in the catalog. */
function untrustedInT(expr: Node | undefined, out: string[], inT = false): string[] {
  if (!expr || typeof expr !== 'object') return out;
  if (expr.type === 'CallExpression') {
    const callee = expr.callee as Node;
    const isT = callee.type === 'Identifier' && TRANSLATE.has(callee.name as string);
    const args = expr.arguments as Node[];
    // t(source, ...values): only the source is looked up.
    if (isT && args[0]) untrustedInT(args[0], out, true);
    for (const a of isT ? args.slice(1) : args) untrustedInT(a, out, inT);
    untrustedInT(callee, out, inT);
    return out;
  }
  if (inT && expr.type === 'MemberExpression') {
    const prop = expr.property as Node;
    if (!expr.computed && prop.type === 'Identifier' && UNTRUSTED_FIELDS.has(prop.name as string)) out.push(prop.name as string);
  }
  for (const [k, v] of Object.entries(expr)) {
    if (k === 'start' || k === 'end' || k === 'loc') continue;
    if (Array.isArray(v)) v.forEach((c) => untrustedInT(c as Node, out, inT));
    else if (v && typeof v === 'object' && 'type' in (v as object)) untrustedInT(v as Node, out, inT);
  }
  return out;
}

/** Whether an expression is text put together: a call, a template with values, a sum (or one of them in a branch). */
function composed(expr: Node | undefined): boolean {
  if (!expr) return false;
  switch (expr.type) {
    case 'CallExpression':
    case 'BinaryExpression':
      return true;
    case 'TemplateLiteral':
      return (expr.expressions as Node[]).length > 0;
    case 'ConditionalExpression':
      return composed(expr.consequent as Node) || composed(expr.alternate as Node);
    case 'LogicalExpression':
      return composed(expr.left as Node) || composed(expr.right as Node);
    case 'ChainExpression':
    case 'ParenthesizedExpression':
    case 'TSAsExpression':
    case 'TSNonNullExpression':
      return composed(expr.expression as Node);
    default:
      return false;
  }
}

/** Literal words of an expression (a string, a template, in a branch): what must go through t(). */
function literalWords(expr: Node | undefined): boolean {
  if (!expr) return false;
  switch (expr.type) {
    case 'Literal':
      return typeof expr.value === 'string' && /\p{L}/u.test(expr.value);
    case 'TemplateLiteral':
      return (expr.quasis as Node[]).some((q) => /\p{L}/u.test(((q.value as { cooked?: string }).cooked ?? '') as string));
    case 'ConditionalExpression':
      return literalWords(expr.consequent as Node) || literalWords(expr.alternate as Node);
    case 'LogicalExpression':
      return literalWords(expr.right as Node);
    case 'ParenthesizedExpression':
    case 'TSAsExpression':
      return literalWords(expr.expression as Node);
    default:
      return false;
  }
}

/** Calls of t() given text put together, and words of the app put in a toast or a dialog without t(). */
function messageFindings(root: Node, file: string, source: string, lineOf: (pos: number) => number): Finding[] {
  const out: Finding[] = [];
  const walk = (node: unknown) => {
    if (!node || typeof node !== 'object') return;
    if (Array.isArray(node)) {
      node.forEach(walk);
      return;
    }
    const n = node as Node;
    if (n.type === 'CallExpression' && (n.callee as Node).type === 'Identifier') {
      const name = (n.callee as Node).name as string;
      const args = n.arguments as Node[];
      const code = source.slice(n.start as number, Math.min(n.end as number, (n.start as number) + 80));
      if (TRANSLATE.has(name) && composed(args[0])) out.push({ file, line: lineOf(n.start as number), code, why: 'text put together given to t(): write the message with its own placeholders, t(\'… %1\', value)' });
      const spec = MESSAGE_CALLS[name];
      if (spec) {
        const parts = spec.args.map((i) => args[i]);
        for (const a of args) {
          if (a?.type === 'ObjectExpression') {
            for (const p of a.properties as Node[]) {
              if (p.type === 'Property' && (p.key as Node).type === 'Identifier' && spec.props.includes((p.key as Node).name as string)) parts.push(p.value as Node);
            }
          }
        }
        for (const p of parts) {
          if (literalWords(p)) out.push({ file, line: lineOf(p.start as number), code: source.slice(p.start as number, p.end as number).slice(0, 80), why: `words of the app in ${name}() without t(): toasts and dialogs show what they get as it is` });
        }
      }
    }
    for (const [k, v] of Object.entries(n)) {
      if (k === 'start' || k === 'end' || k === 'loc') continue;
      if (v && typeof v === 'object') walk(v);
    }
  };
  walk(root);
  return out;
}

/** The same for a TypeScript file of the interface. */
export function lintScript(file: string, source: string): Finding[] {
  const wrapped = `<script lang="ts">${source}</script>`;
  const ast = parse(wrapped, { modern: true }) as unknown as { instance: Node };
  const offset = '<script lang="ts">'.length;
  const lineOf = (pos: number) => source.slice(0, Math.max(0, pos - offset)).split('\n').length;
  return messageFindings(ast.instance.content as Node, file, wrapped, lineOf);
}

export function lint(file: string, source: string): Finding[] {
  const findings: Finding[] = [];
  const ast = parse(source, { modern: true }) as unknown as { fragment: Node; instance?: Node };
  const lineOf = (pos: number) => source.slice(0, pos).split('\n').length;
  const visit = (node: Node | undefined, exempt: boolean) => {
    if (!node || typeof node !== 'object') return;
    if (Array.isArray(node)) {
      for (const n of node) visit(n as Node, exempt);
      return;
    }
    let off = exempt;
    if (node.type === 'RegularElement' || node.type === 'SvelteElement') {
      const attrs = (node.attributes as Node[]) ?? [];
      if (attrs.some((a) => a.type === 'Attribute' && a.name === 'translate' && Array.isArray(a.value) && (a.value as Node[])[0]?.data === 'no')) off = true;
      if (['code', 'pre', 'textarea'].includes(node.name as string)) off = true;
    }
    if (node.type === 'ExpressionTag' && !exempt && !ALLOWED.has(source.slice((node.expression as Node).start as number, (node.expression as Node).end as number))) {
      const expr = node.expression as Node;
      for (const field of shownFields(expr, [])) {
        findings.push({ file, line: lineOf(node.start as number), code: source.slice(node.start as number, node.end as number), why: `${field} shown bare: t() for words of the app, <SafeText> for words from outside` });
      }
    }
    if (node.type === 'ExpressionTag' || node.type === 'Attribute') {
      const expr = node.type === 'ExpressionTag' ? (node.expression as Node) : undefined;
      const values = node.type === 'Attribute' && Array.isArray(node.value) ? (node.value as Node[]) : node.type === 'Attribute' && node.value && typeof node.value === 'object' ? [node.value as Node] : [];
      for (const e of expr ? [expr] : values.filter((v) => v.type === 'ExpressionTag').map((v) => v.expression as Node)) {
        for (const field of untrustedInT(e, [])) {
          findings.push({ file, line: lineOf((e.start as number) ?? 0), code: source.slice(e.start as number, e.end as number), why: `${field} is text from outside the app: never looked up in the catalog` });
        }
      }
    }
    for (const [k, v] of Object.entries(node)) {
      if (k === 'attributes' || k === 'expression' || k === 'start' || k === 'end') continue;
      if (v && typeof v === 'object') visit(v as Node, off);
    }
    // Attributes: only the untrusted-in-t() rule (their words are translated by the build).
    for (const a of (node.attributes as Node[]) ?? []) visit({ ...a, value: a.value } as Node, true);
  };
  visit(ast.fragment, false);
  findings.push(...messageFindings(ast.fragment, file, source, lineOf));
  if (ast.instance) findings.push(...messageFindings(ast.instance, file, source, lineOf));
  // In the code of the component, too: t(x.memo)...
  if (ast.instance) {
    for (const field of untrustedInT(ast.instance.content as Node, [])) {
      findings.push({ file, line: 0, code: field, why: `${field} is text from outside the app: never looked up in the catalog` });
    }
  }
  return findings;
}

function scriptFiles(dir: string, out: string[] = []): string[] {
  for (const f of readdirSync(dir)) {
    const p = join(dir, f);
    if (statSync(p).isDirectory()) {
      if (f !== 'test') scriptFiles(p, out);
    } else if (p.endsWith('.ts') && !p.endsWith('.test.ts') && !p.endsWith('.d.ts')) out.push(p);
  }
  return out;
}

describe('the opt-in translation (i18n lint)', () => {
  it('finds fields of words shown bare, and text from outside put in t()', () => {
    const bad = lint('x.svelte', `<script>let tx = { label: 'a' }; const TITLE = 'T';</script><p>{tx.label}</p><h1>{TITLE}</h1><p>{t(peer.subver)}</p>`);
    expect(bad.map((f) => f.code)).toEqual(['{tx.label}', '{TITLE}', 't(peer.subver)']);
    const good = lint('y.svelte', `<p>{t(tab.label)}</p><SafeText text={tx.label} /><span translate="no">{tx.label}</span><p>{t('Sent to %1', tx.label)}</p><code>{x.name}</code>`);
    expect(good).toEqual([]);
  });

  it('finds text put together given to t(), and words put in toasts and dialogs without t()', () => {
    const bad = lint('z.svelte', `<script>toast('error', 'Saved', err); inform(t('Ok'), \`Sent \${n}\`); ask({ title: 'Hi', message: t('m') }); const a = t(statusText(r)); const b = t(\`\${n} hours behind\`); const c = t('x ' + y);</script><p>{t(String(v))}</p>`);
    expect(bad.map((f) => f.why.split(':')[0])).toEqual([
      'text put together given to t()',
      "words of the app in toast() without t()",
      "words of the app in inform() without t()",
      "words of the app in ask() without t()",
      'text put together given to t()',
      'text put together given to t()',
      'text put together given to t()',
    ]);
    const good = lintScript('z.ts', `toast('error', t('Saved'), errorText(e)); inform(t('Ok'), t('Sent %1', n)); confirm(TITLE, t('Sure?'), { informative: lines.join('\\n') }); t(tab.label); t(x ? 'a' : 'b'); tn('%n day(s)', n);`);
    expect(good).toEqual([]);
    expect(lintScript('y.ts', `toast('info', 'Copied', \`\${a} b\`)`).length).toBe(2);
  });

  it('passes on every script of the interface', () => {
    const findings: Finding[] = [];
    for (const file of scriptFiles(SRC)) findings.push(...lintScript(relative(SRC, file), readFileSync(file, 'utf8')));
    expect(findings.map((f) => `${f.file}:${f.line} ${f.code} (${f.why})`)).toEqual([]);
  });

  it('passes on every component of the interface', () => {
    const findings: Finding[] = [];
    for (const file of svelteFiles(SRC)) findings.push(...lint(relative(SRC, file), readFileSync(file, 'utf8')));
    expect(findings.map((f) => `${f.file}:${f.line} ${f.code} (${f.why})`)).toEqual([]);
  });
});
