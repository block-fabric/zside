// The bridge to the host (chains-app): requests to the node and to the app,
// events from them.
//
// In chains-app the host binds window.__chains (a function that returns a
// promise) and calls window.__chainsEvents with batches of events, as JSON
// text. Elsewhere (vite dev in a browser, the tests) a transport can be set
// by hand, see mock.ts.

export type RequestKind = 'rpc' | 'host' | 'cancel';

export interface BridgeRequest {
  rid?: string;
  kind: RequestKind;
  method?: string;
  params?: unknown[] | Record<string, unknown>;
  wallet?: string;
}

export interface BridgeEvent<T = unknown> {
  type: string;
  data: T;
}

export type Transport = (request: BridgeRequest) => Promise<unknown>;

declare global {
  interface Window {
    __chains?: Transport;
    __chainsEvents?: (batch: string) => void;
  }
}

/** An error the node or the app answered with. */
export class BridgeError extends Error {
  readonly code: number;
  constructor(code: number, message: string) {
    super(message);
    this.name = 'BridgeError';
    this.code = code;
  }

  static from(value: unknown): BridgeError {
    if (value instanceof BridgeError) return value;
    if (value && typeof value === 'object' && 'code' in value && 'message' in value) {
      const v = value as { code: unknown; message: unknown };
      return new BridgeError(Number(v.code), String(v.message));
    }
    if (value instanceof Error) return new BridgeError(-1, value.message);
    return new BridgeError(-1, String(value));
  }
}

/** Codes of the bridge itself (src/webui/bridge.h). */
export const ErrorCode = {
  CANCELLED: -32800,
  INVALID_REQUEST: -32600,
  METHOD_NOT_FOUND: -32601,
  NOT_READY: -28,
  STOPPED: -9,
  HOST: -1,
} as const;

let transport: Transport | null = null;
const listeners = new Map<string, Set<(data: unknown) => void>>();
const anyListeners = new Set<(event: BridgeEvent) => void>();

/** Use a transport of one's own (tests, the browser preview). */
export function setTransport(t: Transport | null): void {
  transport = t;
}

function currentTransport(): Transport {
  if (transport) return transport;
  if (typeof window !== 'undefined' && typeof window.__chains === 'function') return window.__chains;
  throw new BridgeError(ErrorCode.NOT_READY, 'No connection to the node');
}

export function hasHost(): boolean {
  return transport !== null || (typeof window !== 'undefined' && typeof window.__chains === 'function');
}

let nextRid = 1;

export interface CallOptions {
  /** Name of the wallet, for wallet RPC methods. */
  wallet?: string | null;
  /** Cancels the request: it rejects with ErrorCode.CANCELLED at once. */
  signal?: AbortSignal;
}

/** Send a request to the host; resolves with the result, rejects with a BridgeError. */
export async function request<T>(kind: 'rpc' | 'host', method: string, params: unknown[] | Record<string, unknown> = [], options: CallOptions = {}): Promise<T> {
  const send = currentTransport();
  const rid = String(nextRid++);
  const body: BridgeRequest = { rid, kind, method, params };
  if (options.wallet != null) body.wallet = options.wallet;
  const { signal } = options;
  if (signal?.aborted) throw new BridgeError(ErrorCode.CANCELLED, 'Cancelled');
  let onAbort: (() => void) | undefined;
  try {
    const pending = send(body);
    if (!signal) return (await pending) as T;
    const aborted = new Promise<never>((_, reject) => {
      onAbort = () => {
        send({ kind: 'cancel', rid }).catch(() => undefined);
        reject(new BridgeError(ErrorCode.CANCELLED, 'Cancelled'));
      };
      signal.addEventListener('abort', onAbort, { once: true });
    });
    return (await Promise.race([pending, aborted])) as T;
  } catch (e) {
    throw BridgeError.from(e);
  } finally {
    if (onAbort && signal) signal.removeEventListener('abort', onAbort);
  }
}

/** Listen to events of a type; returns the function that stops listening. */
export function on<T = unknown>(type: string, fn: (data: T) => void): () => void {
  let set = listeners.get(type);
  if (!set) {
    set = new Set();
    listeners.set(type, set);
  }
  set.add(fn as (data: unknown) => void);
  return () => set!.delete(fn as (data: unknown) => void);
}

/** Listen to every event. */
export function onAny(fn: (event: BridgeEvent) => void): () => void {
  anyListeners.add(fn);
  return () => anyListeners.delete(fn);
}

/** Hand events to the listeners (what the host calls, through window.__chainsEvents). */
export function dispatch(events: BridgeEvent[]): void {
  for (const event of events) {
    if (!event || typeof event.type !== 'string') continue;
    for (const fn of anyListeners) fn(event);
    const set = listeners.get(event.type);
    if (!set) continue;
    for (const fn of [...set]) {
      try {
        fn(event.data);
      } catch (e) {
        console.error(`event ${event.type}:`, e);
      }
    }
  }
}

/** Take the events of the host. The batch is JSON text: parsed, never run. */
export function installEventSink(): void {
  window.__chainsEvents = (batch: string) => {
    let events: unknown;
    try {
      events = JSON.parse(batch);
    } catch {
      console.error('bad event batch');
      return;
    }
    if (Array.isArray(events)) dispatch(events as BridgeEvent[]);
  };
}
