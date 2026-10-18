// Exact numbers from the node (audit 7 E-BA6): JSON.parse reads every number as a double, so an amount past 2^53
// units (an asset with 8 decimals and a supply of billions) comes back rounded. A request asked {exact: true}
// (bridge.ts) gets its result as text from the host ({"$rawjson": text}, src/webui/bridge.cpp); read here, a number
// a double holds exactly as the node wrote it stays a number, and only one it cannot hold becomes the text of its
// digits ("1152921504606846977"), for code that reads amounts as text or bigint (unitsOf of BitAssets).

/** The key of a result the host gave as text (EXACT_KEY of bridge.h). */
export const EXACT_KEY = '$rawjson';

/** The significant digits of a decimal number and where its point is: value = 0.<digits> × 10^exp. */
function normalize(text: string): { digits: string; exp: number } | null {
  const m = /^-?(\d*)(?:\.(\d*))?(?:[eE]([+-]?\d+))?$/.exec(text);
  if (!m) return null;
  const whole = m[1] ?? '';
  const all = whole + (m[2] ?? '');
  const lead = all.length - all.replace(/^0+/, '').length;
  const digits = all.slice(lead).replace(/0+$/, '');
  return { digits, exp: whole.length + Number(m[3] ?? 0) - lead };
}

/** Whether a number of JSON, read as a double, keeps every digit it was written with. */
export function holdsExactly(literal: string): boolean {
  const value = Number(literal);
  if (!Number.isFinite(value)) return false;
  const want = normalize(literal);
  if (!want) return false;
  // Zero, and up to 15 significant digits: always held.
  if (want.digits.length <= 15) return true;
  if (want.digits.length > 100) return false;
  const got = normalize(Math.abs(value).toPrecision(want.digits.length));
  return !!got && got.digits === want.digits && got.exp === want.exp;
}

const TOKENS = /"(?:[^"\\]|\\.)*"|-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?/g;

/** JSON read with its numbers exact: one a double cannot hold is given as the text of its digits. */
export function parseExact<T = unknown>(text: string): T {
  const kept = text.replace(TOKENS, (token) => (token.charCodeAt(0) === 34 || holdsExactly(token) ? token : `"${token}"`));
  return JSON.parse(kept) as T;
}

/** A result of the host: read exactly when the host gave it as text, else as it is (a transport of the tests). */
export function exactResult<T>(result: unknown): T {
  if (result && typeof result === 'object' && !Array.isArray(result)) {
    const raw = (result as Record<string, unknown>)[EXACT_KEY];
    if (typeof raw === 'string' && Object.keys(result).length === 1) return parseExact<T>(raw);
  }
  return result as T;
}
