// The fixes of the kit from audit 7, area D (audit/2026-10-16-audit-7-D-appkit.md), on the side of the page: what pays,
// signs or votes goes through the host, which asks on the desktop (#1); payment URIs whose label holds line breaks (#4); the
// arming of the dialog of the host (#7); theme colours the web view would not draw (#9) and colours that hide text (#10);
// the set of characters written out, the same as the host's (#11); the explorer read in parts (#12, node-pages.test.ts); the
// CSV guard (#20a).
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { afterEach, describe, expect, it } from 'vitest';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { legibleColors, parseColor, PAGE_TEXT_PAIRS, contrastOf, baseColors, checkName } from '../lib/customthemes';
import { KEYED_BATCH, walletRpc, withKeys } from '../lib/dialogs.svelte';
import { isHidden, oneLine, revealBidi } from '../lib/format';
import { rpc } from '../lib/rpc';
import { parsePaymentUri, SCHEME } from '../lib/uri';
import { csvCell, csvSafe } from '../lib/wallet';

const GUARD = readFileSync(join(__dirname, '../../../guard.cpp'), 'utf8');

afterEach(() => setTransport(null));

describe('audit 7 #1: what pays, signs or votes goes to the host, which asks on the desktop', () => {
  function transport(seen: BridgeRequest[], answer: (r: BridgeRequest) => unknown = () => null) {
    return async (r: BridgeRequest) => {
      seen.push(r);
      if (r.method === 'rpc.confirmedMethods') return { confirmed: ['setwithdrawalvote', 'createsidechaindeposit', 'signmessage', 'setgenerate'], keyed: [] };
      return answer(r);
    };
  }

  it('rpc.raw sends a confirmed method to the host, any other to the node', async () => {
    const seen: BridgeRequest[] = [];
    setTransport(transport(seen));
    await rpc.raw('setwithdrawalvote', [1, 'upvote', 'ab'.repeat(32)]);
    await rpc.raw('getblockcount', []);
    const calls = seen.filter((r) => r.method !== 'rpc.confirmedMethods');
    expect(calls[0]).toMatchObject({ kind: 'host', method: 'rpc.confirmed', params: { method: 'setwithdrawalvote', params: [1, 'upvote', 'ab'.repeat(32)] } });
    expect('wallet' in (calls[0].params as Record<string, unknown>)).toBe(false);
    expect(calls[1]).toMatchObject({ kind: 'rpc', method: 'getblockcount' });
  });

  it('walletRpc: a spend through the host with its wallet, null when the user said no; never a passphrase from the page', async () => {
    const seen: BridgeRequest[] = [];
    let decline = false;
    setTransport(
      transport(seen, (r) => {
        if (r.method === 'rpc.confirmed' && decline) throw { code: -32800, message: 'Deposit to a sidechain: declined.' };
        return { txid: 'cd'.repeat(32) };
      }),
    );
    await expect(walletRpc('w', 'createsidechaindeposit', [1, 's1_x_y', '1'])).resolves.toEqual({ txid: 'cd'.repeat(32) });
    decline = true;
    await expect(walletRpc('w', 'createsidechaindeposit', [1, 's1_x_y', '1'])).resolves.toBeNull();
    const confirmed = seen.filter((r) => r.method === 'rpc.confirmed');
    expect(confirmed).toHaveLength(2);
    expect(confirmed[0].params).toEqual({ wallet: 'w', method: 'createsidechaindeposit', params: [1, 's1_x_y', '1'] });
    expect(seen.some((r) => JSON.stringify(r.params ?? {}).includes('passphrase'))).toBe(false);
  });

  it('walletRpc: a method that only needs the keys runs as it is, and through the host when the wallet is locked', async () => {
    const seen: BridgeRequest[] = [];
    setTransport(
      transport(seen, (r) => {
        if (r.kind === 'rpc') throw { code: -13, message: 'Please enter the wallet passphrase with walletpassphrase first.' };
        return ['mine'];
      }),
    );
    await expect(walletRpc('w', 'listmyassets', [])).resolves.toEqual(['mine']);
    expect(seen.filter((r) => r.method !== 'rpc.confirmedMethods').map((r) => `${r.kind} ${r.method}`)).toEqual(['rpc listmyassets', 'host rpc.confirmed']);
  });

  it('withKeys: several calls in one question of the host', async () => {
    const seen: BridgeRequest[] = [];
    setTransport(transport(seen, () => [{ result: 'sig1' }, { result: 'sig2' }]));
    const answers = await withKeys<string>('w', [
      { method: 'signmessage', params: ['a', 'm'] },
      { method: 'signmessage', params: ['b', 'm'] },
    ]);
    expect(answers).toEqual([{ result: 'sig1' }, { result: 'sig2' }]);
    expect(seen.at(-1)).toMatchObject({ kind: 'host', method: 'rpc.confirmed', params: { wallet: 'w', calls: [{ method: 'signmessage' }, { method: 'signmessage' }] } });
  });
});

describe('audit 7 review: a question of the host shows every call, never cut', () => {
  it('withKeys asks for so many calls at most in one question', async () => {
    const seen: BridgeRequest[] = [];
    setTransport(async (r) => {
      seen.push(r);
      return (r.params as { calls: unknown[] }).calls.map((_, i) => ({ result: `s${i}` }));
    });
    const calls = Array.from({ length: 2 * KEYED_BATCH + 5 }, (_, i) => ({ method: 'signmessage', params: [`a${i}`, 'm'] }));
    const answers = await withKeys<string>('w', calls);
    expect(answers).toHaveLength(calls.length);
    expect(seen.map((r) => (r.params as { calls: unknown[] }).calls.length)).toEqual([KEYED_BATCH, KEYED_BATCH, 5]);
  });
});

describe('audit 7 #4: a payment URI', () => {
  it('has a label and a message of one line, what is not shown as itself written out, cut', () => {
    const attack = `${SCHEME}:bcrt1qattacker?amount=1&label=Exchange%27%20(GOODADDR)%0A%0A%0A%0A%0Ax&message=a%0Db%E2%80%AEc`;
    const parsed = parsePaymentUri(attack);
    expect(parsed.ok).toBe(true);
    if (!parsed.ok) return;
    expect(parsed.request.label).toBe("Exchange' (GOODADDR) x");
    expect(parsed.request.label).not.toMatch(/[\n\r]/);
    expect(parsed.request.message).toBe('a b[U+202E]c');
    const long = parsePaymentUri(`${SCHEME}:bcrt1qx?label=${'y'.repeat(300)}`);
    expect(long.ok && [...long.request.label!].length).toBe(100);
  });

  it('oneLine folds controls and separators, keeps other text', () => {
    expect(oneLine('a b c\u0085d\t e')).toBe('a b c d e');
    expect(oneLine('  مرحبا  hello ')).toBe('مرحبا hello');
  });
});

describe('audit 7 #9, #10: theme colours', () => {
  it('refuses a colour that mixes numbers and percentages (the web view would not draw it)', () => {
    expect(parseColor('rgb(0, 100%, 0)')).toBeNull();
    expect(parseColor('rgb(0 100% 0)')).toBeNull();
    expect(parseColor('rgb(0%, 100%, 0%)')).toEqual({ r: 0, g: 255, b: 0, a: 1 });
    expect(parseColor('rgb(0, 255, 0)')).toEqual({ r: 0, g: 255, b: 0, a: 1 });
  });

  it('draws as the base what hides text, warnings or errors; the rest as hex', () => {
    const hidden = legibleColors({ base: 'midnight', colors: { negative: '#ff000000', warning: '#ffaa0000', 'text-muted': 'rgba(200, 200, 200, 0.1)', line: 'rgb(18, 52, 86)' } });
    expect(hidden.replaced).toEqual(expect.arrayContaining(['negative', 'warning', 'text-muted']));
    expect(hidden.colors.negative).toBeUndefined();
    expect(hidden.colors.line).toBe('#123456');
    // A text colour as dark as its background: given way to the base's.
    const dim = legibleColors({ base: 'midnight', colors: { text: '#101010' } });
    expect(dim.replaced).toContain('text');
    // Whatever kept is legible where the base is.
    const base = baseColors('midnight');
    const kept = legibleColors({ base: 'midnight', colors: { warning: '#ffd166', negative: '#ff6b6b' } });
    for (const [fg, bg] of PAGE_TEXT_PAIRS) {
      const own = (t: string) => (kept.colors as Record<string, string>)[t] ?? (base as Record<string, string>)[t];
      const before = contrastOf(base[fg], base[bg]) ?? 0;
      if (before >= 4.5) expect(contrastOf(own(fg), own(bg)) ?? 0, `${fg} on ${bg}`).toBeGreaterThanOrEqual(4.5);
    }
  });
});

describe('audit 7 #11: what is written out, the same set as the host', () => {
  it('writes out invisible and format characters, the Hangul fillers, the joiner, tags, and stray variation selectors', () => {
    for (const cp of [0x200b, 0x2060, 0xe0041, 0x180e, 0x034f, 0x3164, 0x115f, 0x1160, 0xffa0, 0x00ad, 0xfeff, 0x2028]) {
      expect(revealBidi(`a${String.fromCodePoint(cp)}b`), cp.toString(16)).toBe(`a[U+${cp.toString(16).toUpperCase().padStart(4, '0')}]b`);
    }
    // A variation selector after a letter is hidden; after an emoji it is the emoji's look.
    expect(revealBidi('a️')).toBe('a[U+FE0F]');
    expect(revealBidi('❤️')).toBe('❤️');
    // Line breaks and tabs stay in a block of code.
    expect(revealBidi('a\nb\tc')).toBe('a\nb\tc');
    // Theme names with any of them are refused (they could look like another's).
    expect('error' in checkName('Rose​')).toBe(true);
    expect('error' in checkName('Roseㅤ')).toBe(true);
  });

  it('is the set of the host (guard.cpp IsControlOrFormat): every range there is hidden here', () => {
    const body = GUARD.slice(GUARD.indexOf('bool IsControlOrFormat(uint32_t cp)'), GUARD.indexOf('bool IsVariationSelector'));
    const points = [...body.matchAll(/0x([0-9A-Fa-f]+)/g)].map((m) => parseInt(m[1], 16));
    expect(points.length).toBeGreaterThan(30);
    for (const cp of points) {
      // Each bound of a range, and each code point named alone (0x20 and 0x7F are the ends of the C0 and C1 tests).
      if (cp === 0x20) continue;
      expect(isHidden(0x61, cp), cp.toString(16)).toBe(true);
    }
  });
});

describe('audit 7 #20a: the CSV guard', () => {
  it('looks past spaces, controls, invisible marks and quotes, and knows the full-width signs', () => {
    for (const cell of [' =1+1', ' =1+1', '＝cmd', '"\n=1', '\t=1', "'=1", '​@SUM(A1)', '＋1']) {
      expect(csvSafe(cell).startsWith("'"), JSON.stringify(cell)).toBe(true);
      expect(csvCell(csvSafe(cell))).toBe(cell);
    }
    for (const cell of ['-0.5', '+12', 'plain', 'a=b', '']) expect(csvSafe(cell)).toBe(cell);
    expect(csvCell("'plain")).toBe("'plain");
  });
});
