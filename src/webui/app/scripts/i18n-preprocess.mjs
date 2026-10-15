// The words written in the templates of the components, made into calls of t() as the components are
// compiled: translation is opt-in (src/lib/i18n.svelte.ts), and this is what opts the templates in.
//
// - A run of text in an element ("Send", "{n} blocks behind") becomes {__t("%1 blocks behind", n)}: the
//   template is looked up in the catalog, the values are put in as they are.
// - The same for the attributes title, placeholder, aria-label and alt of elements, and for the props of
//   components that hold words (label, title, placeholder, empty, openLabel, text).
// - In {expressions} shown as text or in those attributes, the string literals that are what is shown
//   ({busy ? 'Working…' : 'Send'}, `${n} blocks`) are translated too.
// Not translated: <code>, <pre>, <textarea>, <script>, <style>, anything under translate="no", and every
// other expression (what the code computes: data of the chain, the wallet, the user).
import MagicString from 'magic-string';
import { parse } from 'svelte/compiler';
import { relative, dirname, join } from 'node:path';

const ELEMENT_ATTRIBUTES = new Set(['title', 'placeholder', 'aria-label', 'alt']);
const COMPONENT_PROPS = new Set(['label', 'title', 'placeholder', 'empty', 'openLabel', 'text', 'aria-label']);
const SKIP_ELEMENTS = new Set(['code', 'pre', 'textarea', 'script', 'style']);
const FN = '__t';
const LETTER = /\p{L}/u;
// What Svelte does to the whitespace of text outside <pre>.
const collapse = (s) => s.replace(/[ \t\r\n\f]+/g, ' ');
// A '%' that would read as a placeholder: such text is left alone.
const HOLDER = /%[0-9n]/;

function translateNo(node) {
  return (node.attributes ?? []).some((a) => a.type === 'Attribute' && a.name === 'translate' && Array.isArray(a.value) && a.value.length === 1 && a.value[0].data === 'no');
}

/** The leaves of an expression that are what is shown: string literals, template literals. */
function literalLeaves(expr, out) {
  if (!expr) return out;
  switch (expr.type) {
    case 'Literal':
      if (typeof expr.value === 'string') out.push(expr);
      break;
    case 'TemplateLiteral':
      out.push(expr);
      break;
    case 'ConditionalExpression':
      literalLeaves(expr.consequent, out);
      literalLeaves(expr.alternate, out);
      break;
    case 'LogicalExpression':
      // a ?? 'none', a || 'none': the right side is the app's.
      literalLeaves(expr.right, out);
      break;
    case 'ParenthesizedExpression':
      literalLeaves(expr.expression, out);
      break;
    default:
      break;
  }
  return out;
}

export function transform(content, filename = 'Component.svelte') {
  let ast;
  try {
    ast = parse(content, { modern: true });
  } catch {
    return null;
  }
  const s = new MagicString(content);
  let changed = false;

  /** Replace a string literal or template literal of an expression with a call of t(). */
  function wrapLeaf(leaf) {
    const call = leafCall(leaf);
    if (!call) return;
    s.overwrite(leaf.start, leaf.end, call);
    changed = true;
  }

  /** The call of t() that a literal leaf becomes, as text (null: left as it is). */
  function leafCall(leaf) {
    if (leaf.type === 'Literal') {
      if (!LETTER.test(leaf.value) || HOLDER.test(leaf.value)) return null;
      return `${FN}(${JSON.stringify(leaf.value)})`;
    }
    const quasis = leaf.quasis.map((q) => q.value.cooked ?? '');
    if (!quasis.some((q) => LETTER.test(q)) || quasis.some((q) => HOLDER.test(q))) return null;
    let source = '';
    const args = [];
    leaf.quasis.forEach((q, i) => {
      source += quasis[i];
      if (i < leaf.expressions.length) {
        args.push(content.slice(leaf.expressions[i].start, leaf.expressions[i].end));
        source += `%${args.length}`;
      }
    });
    return `${FN}(${[JSON.stringify(source), ...args.map((a) => `(${a})`)].join(', ')})`;
  }

  /** The source of an expression put in a template, with its literal leaves translated. */
  function argSource(expr) {
    const leaves = literalLeaves(expr, []).map((leaf) => ({ leaf, call: leafCall(leaf) })).filter((x) => x.call);
    let out = content.slice(expr.start, expr.end);
    for (const { leaf, call } of leaves.sort((a, b) => b.leaf.start - a.leaf.start)) {
      out = out.slice(0, leaf.start - expr.start) + call + out.slice(leaf.end - expr.start);
    }
    return out;
  }

  function wrapExpression(tag) {
    for (const leaf of literalLeaves(tag.expression, [])) wrapLeaf(leaf);
  }

  /** A run of Text and ExpressionTag nodes (one text node of the page). */
  function wrapRun(run) {
    const hasWords = run.some((n) => n.type === 'Text' && LETTER.test(n.data));
    if (!hasWords) {
      for (const n of run) if (n.type === 'ExpressionTag') wrapExpression(n);
      return;
    }
    if (run.some((n) => n.type === 'Text' && HOLDER.test(n.data))) return;
    // The whitespace at both ends stays where it is, outside the call (Svelte collapses it as before).
    const first = run[0];
    const last = run[run.length - 1];
    let start = first.start;
    let end = last.end;
    let leading = '';
    let trailing = '';
    if (first.type === 'Text') {
      leading = first.raw.match(/^[ \t\r\n\f]*/)[0];
      start += leading.length;
    }
    if (last.type === 'Text') {
      trailing = last.raw.match(/[ \t\r\n\f]*$/)[0];
      end -= trailing.length;
      if (end < start) return;
    }
    let source = '';
    const args = [];
    run.forEach((n, i) => {
      if (n.type === 'Text') {
        let data = n.data;
        if (i === 0) data = data.replace(/^[ \t\r\n\f]+/, '');
        if (i === run.length - 1) data = data.replace(/[ \t\r\n\f]+$/, '');
        source += collapse(data);
      } else {
        args.push(argSource(n.expression));
        source += `%${args.length}`;
      }
    });
    if (!source.trim()) return;
    s.overwrite(start, end, `{${FN}(${[JSON.stringify(source), ...args.map((a) => `(${a}) ?? ''`)].join(', ')})}`);
    changed = true;
  }

  /** An attribute or a prop that holds words: "text" or "text {x}" becomes {__t(...)}; {cond ? 'a' : 'b'} gets its literals translated. */
  function wrapAttribute(attr) {
    const value = attr.value;
    if (value === true) return;
    if (!Array.isArray(value)) {
      if (value?.type === 'ExpressionTag') wrapExpression(value);
      return;
    }
    if (!value.some((n) => n.type === 'Text' && LETTER.test(n.data))) {
      for (const n of value) if (n.type === 'ExpressionTag') wrapExpression(n);
      return;
    }
    if (value.some((n) => n.type === 'Text' && HOLDER.test(n.data))) return;
    let source = '';
    const args = [];
    for (const n of value) {
      if (n.type === 'Text') source += n.data;
      else {
        args.push(argSource(n.expression));
        source += `%${args.length}`;
      }
    }
    // name="..." -> name={...}: from the start of the value (with its quote) to the end of the attribute.
    const valueStart = value[0].start;
    const valueEnd = value[value.length - 1].end;
    const quoted = content[valueStart - 1] === '"' || content[valueStart - 1] === "'";
    const from = quoted ? valueStart - 1 : valueStart;
    const to = quoted ? valueEnd + 1 : valueEnd;
    s.overwrite(from, to, `{${FN}(${[JSON.stringify(source.trim()), ...args.map((a) => `(${a}) ?? ''`)].join(', ')})}`);
    changed = true;
  }

  function visitFragment(fragment, skip) {
    if (!fragment) return;
    let run = [];
    const flush = () => {
      if (run.length && !skip) wrapRun(run);
      run = [];
    };
    for (const node of fragment.nodes) {
      if (node.type === 'Text' || node.type === 'ExpressionTag') {
        run.push(node);
        continue;
      }
      flush();
      visit(node, skip);
    }
    flush();
  }

  function visit(node, skip) {
    if (!node || typeof node !== 'object') return;
    switch (node.type) {
      case 'RegularElement':
      case 'SvelteElement': {
        const off = skip || translateNo(node) || SKIP_ELEMENTS.has(node.name);
        if (!skip && !translateNo(node)) {
          for (const a of node.attributes) if (a.type === 'Attribute' && ELEMENT_ATTRIBUTES.has(a.name)) wrapAttribute(a);
        }
        visitFragment(node.fragment, off);
        return;
      }
      case 'Component':
      case 'SvelteComponent':
      case 'SvelteSelf': {
        const off = skip || node.name === 'SafeText';
        if (!off) {
          for (const a of node.attributes) if (a.type === 'Attribute' && COMPONENT_PROPS.has(a.name)) wrapAttribute(a);
        }
        visitFragment(node.fragment, off);
        return;
      }
      default:
        break;
    }
    // Blocks ({#if}, {#each}, {#snippet}, <svelte:...>): their fragments.
    for (const key of ['fragment', 'consequent', 'alternate', 'body', 'fallback', 'pending', 'then', 'catch']) {
      const child = node[key];
      if (child && child.type === 'Fragment') visitFragment(child, skip);
    }
  }

  visitFragment(ast.fragment, false);
  if (!changed) return null;

  // The function, imported in the script of the component (one is made if there is none).
  const lib = join(dirname(new URL(import.meta.url).pathname), '../src/lib/i18n.svelte');
  let path = relative(dirname(filename), lib).replace(/\\/g, '/');
  if (!path.startsWith('.')) path = './' + path;
  const line = `import { t as ${FN} } from ${JSON.stringify(path)};`;
  if (ast.instance) {
    s.appendLeft(ast.instance.content.start, `\n${line}\n`);
  } else {
    const lang = ast.module && /lang=["']ts["']/.test(content.slice(ast.module.start, ast.module.content.start)) ? ' lang="ts"' : '';
    s.prepend(`<script${lang}>${line}</script>\n`);
  }
  return { code: s.toString(), map: s.generateMap({ hires: true, source: filename }) };
}

/** The preprocessor, for the Svelte plugin of Vite. */
export default function i18nPreprocess() {
  return {
    name: 'chains-i18n',
    markup({ content, filename }) {
      if (!filename || !filename.endsWith('.svelte')) return undefined;
      if (filename.includes('node_modules')) return undefined;
      const out = transform(content, filename);
      return out ?? undefined;
    },
  };
}
