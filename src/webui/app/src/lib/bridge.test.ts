import { afterEach, describe, expect, it, vi } from 'vitest';
import { BridgeError, dispatch, ErrorCode, installEventSink, on, request, setTransport, type BridgeRequest } from './bridge';

afterEach(() => setTransport(null));

describe('bridge requests', () => {
  it('sends typed requests and gives back results', async () => {
    const seen: BridgeRequest[] = [];
    setTransport(async (r) => {
      seen.push(r);
      return { ok: r.method };
    });
    await expect(request('rpc', 'getbalances', [], { wallet: 'my wallet' })).resolves.toEqual({ ok: 'getbalances' });
    await request('host', 'prefs.get', {});
    expect(seen[0]).toMatchObject({ kind: 'rpc', method: 'getbalances', params: [], wallet: 'my wallet' });
    expect(seen[1]).toMatchObject({ kind: 'host', method: 'prefs.get' });
    expect(seen[1].wallet).toBeUndefined();
    expect(seen[0].rid).not.toBe(seen[1].rid);
  });

  it('turns errors into BridgeError with their code', async () => {
    setTransport(async () => {
      throw { code: -8, message: 'bad parameter' };
    });
    const error = (await request('rpc', 'x').catch((e) => e)) as BridgeError;
    expect(error).toBeInstanceOf(BridgeError);
    expect(error.code).toBe(-8);
    expect(error.message).toBe('bad parameter');
  });

  it('cancels: rejects at once and tells the host', async () => {
    const sent: BridgeRequest[] = [];
    setTransport((r) => {
      sent.push(r);
      return r.kind === 'cancel' ? Promise.resolve(true) : new Promise(() => undefined);
    });
    const controller = new AbortController();
    const pending = request('rpc', 'waitfornewblock', [], { signal: controller.signal });
    controller.abort();
    const error = (await pending.catch((e) => e)) as BridgeError;
    expect(error.code).toBe(ErrorCode.CANCELLED);
    expect(sent[1]).toEqual({ kind: 'cancel', rid: sent[0].rid });
    // Already aborted: nothing is sent.
    await expect(request('rpc', 'x', [], { signal: controller.signal })).rejects.toMatchObject({ code: ErrorCode.CANCELLED });
    expect(sent).toHaveLength(2);
  });

  it('without a host, says so', async () => {
    await expect(request('rpc', 'x')).rejects.toMatchObject({ code: ErrorCode.NOT_READY });
  });
});

describe('bridge events', () => {
  it('delivers events to their listeners, as data', () => {
    installEventSink();
    const tips: unknown[] = [];
    const off = on('tip', (d) => tips.push(d));
    window.__chainsEvents!(JSON.stringify([{ type: 'tip', data: { height: 5 } }, { type: 'other', data: 1 }]));
    // Text of the chain stays text.
    window.__chainsEvents!(JSON.stringify([{ type: 'tip', data: { label: '<img src=x onerror=alert(1)>' } }]));
    expect(tips).toEqual([{ height: 5 }, { label: '<img src=x onerror=alert(1)>' }]);
    off();
    dispatch([{ type: 'tip', data: 6 }]);
    expect(tips).toHaveLength(2);
  });

  it('ignores a broken batch and a failing listener', () => {
    installEventSink();
    const spy = vi.spyOn(console, 'error').mockImplementation(() => undefined);
    window.__chainsEvents!('not json');
    const seen: unknown[] = [];
    const off1 = on('x', () => {
      throw new Error('broken listener');
    });
    const off2 = on('x', (d) => seen.push(d));
    dispatch([{ type: 'x', data: 1 }]);
    expect(seen).toEqual([1]);
    off1();
    off2();
    spy.mockRestore();
  });
});
