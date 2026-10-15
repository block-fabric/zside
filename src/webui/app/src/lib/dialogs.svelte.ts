// Dialogs that a page waits for: a question with buttons (QMessageBox), a line of text
// (QInputDialog), a passphrase (AskPassphraseDialog). One shows at a time; DialogHost.svelte draws it.
import { BridgeError, request } from './bridge';
import { t } from './i18n.svelte';
import { rpc } from './rpc';
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

/**
 * Run a wallet function that may need the keys (WalletModel::requestUnlock): first without a
 * passphrase; if the wallet is locked, ask for it and run again with it, as often as it is wrong.
 * Resolves with null when the user cancels.
 */
export async function withUnlock<T>(wallet: string, fn: (passphrase?: string) => Promise<T>): Promise<T | null> {
  try {
    return await fn();
  } catch (e) {
    if (BridgeError.from(e).code !== WalletError.UNLOCK_NEEDED) throw e;
  }
  let error: string | undefined;
  for (;;) {
    const answered = await askPassphrase('unlock', { wallet, error });
    if (!answered) return null;
    try {
      return await fn(answered.passphrase);
    } catch (e) {
      const err = BridgeError.from(e);
      if (err.code === WalletError.PASSPHRASE_INCORRECT) {
        error = err.message;
        continue;
      }
      throw e;
    }
  }
}

/** The message of an error, for a dialog. */
export function errorText(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

/** The answer of one call of wallet.withKeys: its result, or its error. */
export interface KeyedAnswer<T = unknown> {
  result?: T;
  error?: { code: number; message: string };
}

/**
 * Wallet RPC methods that need the keys (a deposit, a stamp, signing a PSBT, proofs of funds): the host
 * runs them with the keys of an encrypted wallet unlocked for these calls alone, and locks it again right
 * after (UnlockContext of chains-qt). If the wallet is locked, the passphrase is asked for first.
 * Resolves with the answer of each call, or null when the user cancels.
 */
export function withKeys<T = unknown>(wallet: string, calls: { method: string; params: unknown[] | Record<string, unknown> }[]): Promise<KeyedAnswer<T>[] | null> {
  return withUnlock(wallet, (passphrase) =>
    request<KeyedAnswer<T>[]>('host', 'wallet.withKeys', passphrase ? { wallet, calls, passphrase } : { wallet, calls }),
  );
}

/**
 * One wallet RPC method that may need the keys; its error is thrown. A wallet that needs no unlocking
 * runs it as it is; a locked one answers -13, and then the passphrase is asked for and the host runs it
 * with the keys unlocked for this call alone (withKeys). Resolves with null when the user cancels.
 */
export function walletRpc<T>(wallet: string, method: string, params: unknown[] | Record<string, unknown>): Promise<T | null> {
  return withUnlock(wallet, async (passphrase) => {
    if (!passphrase) return rpc.raw<T>(method, params, { wallet });
    const [answer] = await request<KeyedAnswer<T>[]>('host', 'wallet.withKeys', { wallet, calls: [{ method, params }], passphrase });
    if (answer.error) throw new BridgeError(answer.error.code, answer.error.message);
    return answer.result as T;
  });
}
