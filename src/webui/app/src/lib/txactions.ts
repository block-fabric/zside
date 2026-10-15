// What can be done to a transaction of the wallet once sent: increase its fee, abandon it
// (WalletModel::bumpFee, TransactionView::abandonTx).
import { t } from './i18n.svelte';
import { ask, confirm, errorText, inform, withUnlock, type Choice } from './dialogs.svelte';
import { host } from './rpc';
import { prefs, toast } from './state.svelte';
import { formatAmount } from './units';
import { walletApi } from './wallet';
import { refreshRecords } from './walletdata.svelte';

const SEND_CONFIRM_DELAY = 3;

/** Increase the fee of a transaction; the id of the new one, or null. */
export async function bumpFee(wallet: string, txid: string): Promise<string | null> {
  const api = walletApi(wallet);
  let bump;
  try {
    bump = await api.bumpPrepare(txid);
  } catch (e) {
    await inform(t('Fee bump error'), errorText(e), 'error');
    return null;
  }
  const fmt = (v: bigint) => formatAmount(v, prefs.unit, { withUnit: true });
  const lines = [t('Current fee: %1', fmt(bump.oldFee)), t('Increase: %1', fmt(bump.newFee - bump.oldFee)), t('New fee: %1', fmt(bump.newFee))];
  if (prefs.coinControl) {
    lines.push(
      '',
      t('Warning: This may pay the additional fee by reducing change outputs or adding inputs, when necessary. It may add a new change output if one does not already exist. These changes may potentially leak privacy.'),
    );
  }
  const choices: Choice[] = [{ id: 'cancel', label: 'Cancel' }];
  if (prefs.psbtControls || !bump.canSend) choices.push({ id: 'psbt', label: 'Create Unsigned', delay: SEND_CONFIRM_DELAY });
  if (bump.canSend) choices.push({ id: 'yes', label: 'Yes', kind: 'primary', delay: SEND_CONFIRM_DELAY });
  const answer = await ask({ title: t('Confirm fee bump'), message: t('Do you want to increase the fee?'), informative: lines.join('\n'), level: 'question', choices });
  if (answer.choice === 'psbt') {
    try {
      const { psbt } = await api.bumpPsbt(bump.token);
      await host.copy(psbt);
      await inform(t('PSBT copied'), t('Fee-bump PSBT copied to clipboard'));
    } catch (e) {
      await inform(t('Fee bump error'), errorText(e), 'error');
    }
    return null;
  }
  if (answer.choice !== 'yes') return null;
  try {
    const result = await withUnlock(wallet, (passphrase) => api.bumpCommit(bump.token, passphrase));
    if (!result) return null;
    toast('success', t('Fee increased'), t('New transaction %1…', result.txid.slice(0, 16)));
    refreshRecords(true);
    return result.txid;
  } catch (e) {
    await inform(t('Fee bump error'), errorText(e), 'error');
    return null;
  }
}

/** Abandon a transaction that is not in the mempool and not confirmed; its coins can be spent again. */
export async function abandon(wallet: string, txid: string): Promise<boolean> {
  const yes = await confirm(t('Abandon transaction'), t('Abandon this transaction?'), {
    yes: 'Abandon',
    danger: true,
    informative: t('The wallet forgets it: the coins it spends can be spent again. It can still be confirmed if it was sent to the network before.'),
  });
  if (!yes) return false;
  try {
    await walletApi(wallet).abandon(txid);
    refreshRecords(true);
    return true;
  } catch (e) {
    await inform(t('Abandon transaction'), errorText(e), 'error');
    return false;
  }
}
