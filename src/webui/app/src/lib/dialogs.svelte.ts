// Dialogs that a page waits for: a question with buttons (QMessageBox), a line of text
// (QInputDialog), a passphrase (AskPassphraseDialog). One shows at a time; DialogHost.svelte draws it.
import { BridgeError, ErrorCode, request } from './bridge';
import { t } from './i18n.svelte';
import { confirmedMethods, rpc } from './rpc';
import { WalletError } from './wallet';

export interface Choice {
  id: string;
  label: string;
  kind?: 'primary' | 'danger' | 'plain';
  /** Seconds before the button can be pressed (SendConfirmationDialog), counted down on it. */
  delay?: number;
}

export interface Question {
  kind: 'question';
  title: string;
  message: string;
  /** A second paragraph, smaller (QMessageBox::setInformativeText). */
  informative?: string;
  /** Lines shown under "Show details" (QMessageBox::setDetailedText). */
  details?: string[];
  level?: 'info' | 'warning' | 'error' | 'question';
  choices: Choice[];
  /** A checkbox with its label, and its answer (the "load the wallet after migration" box). */
  checkbox?: { label: string; checked: boolean; tip?: string };
  /** Monospaced text in a box, to read or copy (a PSBT, a signature). */
  code?: string;
}

export interface Prompt {
  kind: 'prompt';
  title: string;
  label: string;
  value: string;
  message?: string;
  okLabel?: string;
}

export type PassphraseMode = 'unlock' | 'encrypt' | 'change' | 'migrate';

export interface PassphraseAsk {
  kind: 'passphrase';
  mode: PassphraseMode;
  title: string;
  /** Which wallet, shown in the dialog. */
  wallet?: string;
  /** An error of the last try (a wrong passphrase), shown at the top. */
  error?: string;
}

export type Pending = (Question | Prompt | PassphraseAsk) & { id: number; resolve: (answer: unknown) => void };

export const dialogs = $state({ queue: [] as Pending[] });
let nextId = 1;

function push<T>(d: Question | Prompt | PassphraseAsk): Promise<T> {
  return new Promise<T>((resolve) => {
    dialogs.queue.push({ ...d, id: nextId++, resolve: resolve as (answer: unknown) => void } as Pending);
  });
}

/** Answer the dialog shown (DialogHost). */
export function answer(id: number, value: unknown): void {
  const i = dialogs.queue.findIndex((d) => d.id === id);
  if (i < 0) return;
  const [d] = dialogs.queue.splice(i, 1);
  d.resolve(value);
}

/** Ask; resolves with the id of the button pressed, or null when the dialog was closed. */
export function ask(q: Omit<Question, 'kind'>): Promise<{ choice: string | null; checked: boolean }> {
  return push({ ...q, kind: 'question' });
}

/** A message with an OK button. */
export async function inform(title: string, message: string, level: Question['level'] = 'info', informative?: string): Promise<void> {
  await ask({ title, message, level, informative, choices: [{ id: 'ok', label: 'OK', kind: 'primary' }] });
}

/** Yes or no; true for yes. */
export async function confirm(title: string, message: string, options: { yes?: string; no?: string; informative?: string; danger?: boolean; level?: Question['level'] } = {}): Promise<boolean> {
  const r = await ask({
    title,
    message,
    informative: options.informative,
    level: options.level ?? 'question',
    choices: [
      { id: 'no', label: options.no ?? 'Cancel' },
      { id: 'yes', label: options.yes ?? 'Yes', kind: options.danger ? 'danger' : 'primary' },
    ],
  });
  return r.choice === 'yes';
}

/** A line of text; null when cancelled. */
export function prompt(title: string, label: string, value = '', options: { message?: string; okLabel?: string } = {}): Promise<string | null> {
  return push({ kind: 'prompt', title, label, value, ...options });
}

export interface PassphraseAnswer {
  /** The passphrase (unlock, migrate), or the old one (change). */
  passphrase: string;
  /** The new passphrase (encrypt, change). */
  next?: string;
}

/** Ask for a passphrase; null when cancelled. */
export function askPassphrase(mode: PassphraseMode, options: { wallet?: string; error?: string } = {}): Promise<PassphraseAnswer | null> {
  const titles: Record<PassphraseMode, string> = {
    unlock: t('Unlock wallet'),
    migrate: t('Unlock wallet'),
    encrypt: t('Encrypt wallet'),
    change: t('Change passphrase'),
  };
  return push({ kind: 'passphrase', mode, title: titles[mode], ...options });
}

/** Whether the user said no in a dialog of the desktop (the host's own, App::AskHost): nothing to say then. */
export function declined(e: unknown): boolean {
  return BridgeError.from(e).code === ErrorCode.CANCELLED;
}

/**
 * Run a wallet function of the host that may pay, sign or need the keys. The host asks the user itself, in its own dialog
 * (outside the page), what it does and, for a locked wallet, the passphrase (audit 7 #1): the page never asks for it, and
 * never sees it. Resolves with null when the user said no there. (`fn` gets no passphrase: kept for the callers of old.)
 */
export async function withUnlock<T>(wallet: string, fn: (passphrase?: string) => Promise<T>): Promise<T | null> {
  void wallet;
  try {
    return await fn();
  } catch (e) {
    if (declined(e)) return null;
    throw e;
  }
}

/** The message of an error, for a dialog. */
export function errorText(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

/** The answer of one call of several (withKeys): its result, or its error. */
export interface KeyedAnswer<T = unknown> {
  result?: T;
  error?: { code: number; message: string };
}

/**
 * Wallet RPC methods that pay, sign or need the keys, several at once (proofs of funds sign one message with each address):
 * the host shows what they do in its own dialog, once, with the passphrase field of a locked wallet, and runs them with the
 * keys unlocked for these calls alone (rpc.confirmed). Resolves with the answer of each call, or null when the user said no.
 */
export async function withKeys<T = unknown>(wallet: string, calls: { method: string; params: unknown[] | Record<string, unknown> }[]): Promise<KeyedAnswer<T>[] | null> {
  // The host shows every call of a question, never cut: so many at most in one (confirm.cpp refuses more).
  const answers: KeyedAnswer<T>[] = [];
  try {
    for (let i = 0; i < calls.length; i += KEYED_BATCH) answers.push(...(await request<KeyedAnswer<T>[]>('host', 'rpc.confirmed', { wallet, calls: calls.slice(i, i + KEYED_BATCH) })));
    return answers;
  } catch (e) {
    if (declined(e)) return null;
    throw e;
  }
}

/** The most calls in one question of the host (withKeys): every row of each fits in its dialog. */
export const KEYED_BATCH = 20;

/**
 * One wallet RPC method; its error is thrown. One that pays or signs goes through the host, which asks the user on the
 * desktop first (rpc.confirmed, rpc.ts confirmedMethods); one that only needs the keys runs as it is, and when the wallet is
 * locked (-13) through the host, which asks for the passphrase in its own dialog. Resolves with null when the user said no.
 */
export async function walletRpc<T>(wallet: string, method: string, params: unknown[] | Record<string, unknown>): Promise<T | null> {
  try {
    if ((await confirmedMethods()).has(method)) return await request<T>('host', 'rpc.confirmed', { wallet, method, params });
    try {
      return await rpc.raw<T>(method, params, { wallet });
    } catch (e) {
      if (BridgeError.from(e).code !== WalletError.UNLOCK_NEEDED) throw e;
      return await request<T>('host', 'rpc.confirmed', { wallet, method, params });
    }
  } catch (e) {
    if (declined(e)) return null;
    throw e;
  }
}

/**
 * A first step and its second in one question of the host (rpc.confirmed {then}, src/webui/confirm.h): a reservation
 * and its registration, say. Accepted, the first runs and the host keeps a one-time authorization of exactly that second
 * step (walletRpcIfAuthorized runs it without a question). The first's result, with `authorized` (the seconds the second
 * may come in) when the host keeps one; null when declined.
 */
export async function walletRpcTwoSteps<T extends object>(
  wallet: string,
  method: string,
  params: unknown[],
  then: { method: string; params: unknown[] },
): Promise<(T & { authorized?: number }) | null> {
  try {
    return await request<T & { authorized?: number }>('host', 'rpc.confirmed', { wallet, method, params, then });
  } catch (e) {
    if (declined(e)) return null;
    throw e;
  }
}

/**
 * A call that runs only if it needs no question (a second step the user accepted with its first, the wallet not
 * locked): its result, or `needed` (why the user's word is needed) when the host would have to ask. Never a dialog.
 */
export async function walletRpcIfAuthorized<T>(wallet: string, method: string, params: unknown[]): Promise<{ result: T } | { needed: string }> {
  try {
    return { result: await request<T>('host', 'rpc.confirmed', { wallet, method, params, authorizedOnly: true }) };
  } catch (e) {
    const err = BridgeError.from(e);
    if (err.code === ErrorCode.AUTH_NEEDED) return { needed: err.message };
    throw e;
  }
}

let prepared = 0;
/**
 * A confirmation of the host is likely soon (the page shows the review of what it would ask): the host makes its dialog
 * window ahead, so that it shows at once even on a busy computer (dialog.prepare). At most once in 30 seconds.
 */
export function prepareConfirm(): void {
  const now = Date.now();
  if (now - prepared < 30_000) return;
  prepared = now;
  request('host', 'dialog.prepare', {}).catch(() => undefined);
}
