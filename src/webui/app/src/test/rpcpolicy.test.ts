// The RPC methods the pages call are the ones the host lets them call (src/webui/rpcpolicy.cpp, audit M3):
// a method missing there would fail in the app only.
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';

const SRC = join(__dirname, '..');
const CHAIN_JSON = JSON.parse(readFileSync(join(__dirname, '../chain/chain.json'), 'utf8')) as { role: string; drop?: string[] };
const SIDECHAIN = CHAIN_JSON.role === 'sidechain';

/**
 * rpcpolicy.cpp as this chain builds it: the branches of `#if WEBUI_SIDECHAIN` it does not take are left out
 * (audit port-1 #5: the test read both, so a sidechain's page could call a method of the mainchain's).
 */
export function built(text: string, sidechain: boolean): string {
  const out: string[] = [];
  const stack: { on: boolean; taken: boolean }[] = [];
  const active = () => stack.every((f) => f.on);
  for (const line of text.split('\n')) {
    const directive = line.trim();
    const m = /^#if\s+(!?)WEBUI_SIDECHAIN\b/.exec(directive);
    if (m) {
      const on = m[1] ? !sidechain : sidechain;
      stack.push({ on, taken: on });
    } else if (/^#if/.test(directive)) {
      stack.push({ on: true, taken: true });
    } else if (/^#else/.test(directive) && stack.length) {
      const top = stack[stack.length - 1];
      top.on = !top.taken;
      top.taken = true;
    } else if (/^#endif/.test(directive) && stack.length) {
      stack.pop();
    } else if (active()) {
      out.push(line);
    }
  }
  return out.join('\n');
}
const POLICY = built(readFileSync(join(__dirname, '../../../rpcpolicy.cpp'), 'utf8'), SIDECHAIN).replace(/\/\/.*$/gm, '');
/** The methods of this chain's own pages (local.cpp), added to the policy. */
const LOCAL = built(readFileSync(join(__dirname, '../../../local.cpp'), 'utf8'), SIDECHAIN).replace(/\/\/.*$/gm, '');

/** The sources that never run in this chain: the pages chain.json drops, and the home of the other role. */
function runsHere(where: string): boolean {
  const dropped: Record<string, string> = { sidechains: 'pages/Sidechains.svelte', 'sidechain-nodes': 'pages/SidechainNodes.svelte', mine: 'pages/Mine.svelte' };
  for (const id of CHAIN_JSON.drop ?? []) if (dropped[id] === where) return false;
  // The Sidechains page's helpers (lib/chain.ts) and its Simple form (Move coins) come with it.
  if ((where === 'lib/chain.ts' || where === 'pages/MoveCoins.svelte') && (CHAIN_JSON.drop ?? []).includes('sidechains')) return false;
  if (where === (SIDECHAIN ? 'views/home/Home.svelte' : 'views/home/SideHome.svelte')) return false;
  return true;
}

function sources(dir: string): string[] {
  return readdirSync(dir).flatMap((name) => {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) return name === 'test' ? [] : sources(path);
    if (!/\.(ts|svelte)$/.test(name) || /\.test\.ts$/.test(name) || name === 'mock.ts') return [];
    return [path];
  });
}

function allowed(): Set<string> {
  const body = POLICY.slice(POLICY.indexOf('methods{'), POLICY.indexOf('return methods;'));
  const local = LOCAL.slice(LOCAL.indexOf('RpcMethods()'), LOCAL.indexOf('KeyedRpcMethods()') > 0 ? LOCAL.indexOf('KeyedRpcMethods()') : undefined);
  return new Set([...body.matchAll(/\{"([a-z0-9_]+)", (?:WORKER|LONG)\}/g), ...local.matchAll(/\{"([a-z0-9_]+)", (?:Bridge::Thread::)?(?:WORKER|LONG)\}/g)].map((m) => m[1]));
}

function keyed(): Set<string> {
  const fn = POLICY.slice(POLICY.indexOf('bool KeyedWalletRpc'));
  const list = fn.slice(fn.indexOf('methods{'), fn.indexOf('};'));
  // And those of this chain's own pages (local.cpp).
  const local = LOCAL.slice(LOCAL.indexOf('KeyedRpcMethods()'), LOCAL.indexOf('void RegisterHost'));
  const own = local.slice(local.indexOf('return'));
  return new Set([...list.matchAll(/"([a-z0-9_]+)"/g), ...own.matchAll(/"([a-z0-9_]+)"/g)].map((m) => m[1]));
}

/** The methods named in a call of the RPC client, of walletRpc, or of withKeys. */
function used(): { rpc: Map<string, string>; keyed: Map<string, string>; dynamic: string[] } {
  const rpc = new Map<string, string>();
  const keyedCalls = new Map<string, string>();
  const dynamic: string[] = [];
  for (const file of sources(SRC)) {
    const text = readFileSync(file, 'utf8');
    const where = file.slice(SRC.length + 1);
    if (!runsHere(where)) continue;
    for (const m of text.matchAll(/rpc\.(?:raw|call)\b[\s\S]{0,200}?\(\s*(['"`]?)([A-Za-z0-9_.]+)/g)) {
      if (m[1]) rpc.set(m[2], where);
      else dynamic.push(`${where}: ${m[2]}`);
    }
    for (const m of text.matchAll(/walletRpc\b[^(]*\([^,]+,\s*'([a-z0-9_]+)'/g)) {
      rpc.set(m[1], where);
      keyedCalls.set(m[1], where);
    }
    if (/withKeys</.test(text) || /withKeys\(/.test(text)) {
      for (const m of text.matchAll(/method:\s*'([a-z0-9_]+)'/g)) keyedCalls.set(m[1], where);
    }
    // The Sidechains page runs its RPC methods through a helper: call('method', [...]).
    if (where.endsWith('Sidechains.svelte')) for (const m of text.matchAll(/\bcall\(\s*'([a-z0-9_]+)'/g)) rpc.set(m[1], where);
  }
  return { rpc, keyed: keyedCalls, dynamic };
}

describe('the RPC methods of the pages', () => {
  it('are all allowed by the host', () => {
    const ok = allowed();
    expect(ok.size).toBeGreaterThan(30);
    const { rpc } = used();
    expect(rpc.size).toBeGreaterThan(30);
    const missing = [...rpc].filter(([m]) => !ok.has(m)).map(([m, where]) => `${m} (${where})`);
    expect(missing).toEqual([]);
  });

  it('that need keys are the ones the host runs with keys', () => {
    const ok = keyed();
    const { keyed: calls } = used();
    expect(calls.size).toBeGreaterThan(0);
    const missing = [...calls].filter(([m]) => !ok.has(m)).map(([m, where]) => `${m} (${where})`);
    expect(missing).toEqual([]);
  });

  it('are named in the code, but for the two helpers that pass one on', () => {
    // A method chosen at run time could be one the host refuses: only these helpers pass one on.
    expect(used().dynamic.sort()).toEqual(['lib/dialogs.svelte.ts: method', ...(runsHere('pages/Sidechains.svelte') ? ['pages/Sidechains.svelte: method'] : [])]);
  });

  it('are read as this chain builds the policy, and not as the other role does', () => {
    const text = '{"a", WORKER},\n#if WEBUI_SIDECHAIN\n{"side", WORKER},\n#else\n{"main", WORKER},\n#endif\n#if !WEBUI_SIDECHAIN\n{"main2", WORKER},\n#endif';
    expect(built(text, true)).toBe('{"a", WORKER},\n{"side", WORKER},');
    expect(built(text, false)).toBe('{"a", WORKER},\n{"main", WORKER},\n{"main2", WORKER},');
    const ok = allowed();
    if (SIDECHAIN) {
      // The drivechain methods of the mainchain are not this node's: the mainchain is reached with peg.call.
      for (const m of ['getdrivechaininfo', 'listactivesidechains', 'listwithdrawalbundles', 'createsidechaindeposit', 'setgenerate']) expect(ok.has(m)).toBe(false);
      // What spends the mainchain wallet goes through the host, confirmed on the desktop (audit port-1 #4).
      for (const m of ['setbmm', 'requestbmmblock']) expect(ok.has(m)).toBe(false);
    } else {
      for (const m of ['getmainchaininfo', 'createwithdrawal']) expect(ok.has(m)).toBe(false);
    }
  });

  it('do not include what writes files or stops the node', () => {
    const ok = allowed();
    for (const m of ['backupwallet', 'dumptxoutset', 'dumpwallet', 'stop', 'importdescriptors', 'invalidateblock', 'setban', 'addnode']) expect(ok.has(m)).toBe(false);
  });
});
