// The side panel of the dashboard: Send and Receive at the right of the home, without leaving it.
// One recipient and the fee of the settings; the Send page stays for several recipients, coin
// control and unsigned transactions (PSBT). Sending asks the same confirmation as the Send page.
import { t } from './i18n.svelte';
import { closeDetail } from './detail.svelte';
import { ask, errorText, inform, withUnlock, type Choice } from './dialogs.svelte';
import { nav } from './nav.svelte';
import { host } from './rpc';
import { prefs, toast, wallets } from './state.svelte';
import { formatAmount, UNITS } from './units';
import { nearestConfTarget, walletApi, walletDisplayName, type Prepared } from './wallet';
import { current, refreshInfo } from './walletdata.svelte';

export type QuickMode = 'send' | 'receive';

export const quick = $state({ mode: null as QuickMode | null });

export function openQuick(mode: QuickMode): void {
  closeDetail();
  quick.mode = mode;
}

export function closeQuick(): void {
  quick.mode = null;
}

/** Seconds before the send button of the confirmation can be pressed (as the Send page). */
const SEND_CONFIRM_DELAY = 3;

/** The fee as the Send page has it: the smart fee of the target, or the custom rate when chosen. */
export function quickFeeChoice(): { confTarget: number; feeRate?: number } {
  const confTarget = nearestConfTarget(prefs.confTarget || current.info?.confirmTarget || 6);
  return prefs.feeMode === 'custom' && prefs.customFee > 0 ? { confTarget, feeRate: prefs.customFee } : { confTarget };
}

/**
 * Send to one recipient: prepare, confirm (with the delay of the Send page), send. Wallets that
 * cannot sign here (watch-only, external signers) are sent to the Send page instead. The txid, or null.
 */
export async function quickSend(recipient: { address: string; label: string; amount: bigint; subtractFee: boolean }): Promise<string | null> {
  const wallet = wallets.current;
  if (wallet === null) return null;
  const api = walletApi(wallet);
  let prepared: Prepared | null;
  try {
    prepared = await withUnlock(wallet, (passphrase) => api.prepare([{ ...recipient, message: '' }], {}, quickFeeChoice(), passphrase));
  } catch (e) {
    await inform(t('Send Coins'), errorText(e), 'warning');
    return null;
  }
  if (!prepared) return null;
  const info = current.info ?? (await refreshInfo());
  const r = prepared.recipients[0];
  let amount = formatAmount(r.amount, prefs.unit, { withUnit: true });
  if (wallets.list.length > 1) amount = t("%1 from wallet '%2'", amount, walletDisplayName(wallet));
  const line = r.label ? `${t("%1 to '%2'", amount, r.label)} (${r.address})` : t('%1 to %2', amount, r.address);
  const alternatives = UNITS.filter((u) => u.id !== prefs.unit)
    .map((u) => formatAmount(prepared.total, u.id, { withUnit: true }))
    .join(' or ');
  const kvb = (prepared.vsize / 1000).toPrecision(3);
  const informative = [
    t('Please, review your transaction.'),
    prepared.fee > 0n ? t('Transaction fee (%1 kvB): %2', kvb, formatAmount(prepared.fee, prefs.unit, { withUnit: true })) : '',
    t('You can increase the fee later.'),
    t('Total Amount: %1', formatAmount(prepared.total, prefs.unit, { withUnit: true })),
    `(= ${alternatives})`,
    line,
  ]
    .filter(Boolean)
    .join('\n');
  const choices: Choice[] = [
    { id: 'cancel', label: 'Cancel' },
    { id: 'send', label: info?.externalSigner ? 'Sign on device' : 'Send', kind: 'primary', delay: SEND_CONFIRM_DELAY },
  ];
  const answer = await ask({ title: t('Confirm send coins'), message: t('Do you want to create this transaction?'), informative, level: 'question', choices });
  if (answer.choice !== 'send') {
    api.discard(prepared.token).catch(() => undefined);
    return null;
  }
  try {
    const result = await api.commit(prepared.token);
    if (!result.complete && result.psbt) {
      await host.copy(result.psbt).catch(() => undefined);
      await inform(t('Unsigned Transaction'), t('The PSBT has been copied to the clipboard. Open the Send page to save it.'), 'info');
      return null;
    }
    toast('success', t('Sent'), t('Transaction %1…', result.txid?.slice(0, 16)));
    nav.highlightTx = result.txid ?? null;
    return result.txid ?? null;
  } catch (e) {
    await inform(t('Send Coins'), errorText(e), 'error');
    return null;
  }
}

/** Whether the wallet shown can send from the panel (it signs here). */
export function quickCanSend(): boolean {
  const info = current.info;
  return !!info && !info.privateKeysDisabled && !info.externalSigner;
}
