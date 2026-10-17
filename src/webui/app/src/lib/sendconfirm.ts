// The lines of the page's preview of a payment (Send, the quick panel): one per recipient, the address first and whole,
// then the amount, then the label, quoted and on one line (audit 7 #4: a label with line breaks once pushed the real
// address out of sight). The desktop shows the same again, decoded from the transaction, before anything is signed.
import { t } from './i18n.svelte';
import { oneLine } from './format';
import { prefs } from './state.svelte';
import { formatAmount } from './units';
import { walletDisplayName } from './wallet';

export function recipientLine(address: string, amount: bigint, label: string | undefined, fromWallet: string | null = null): string {
  let shown = formatAmount(amount, prefs.unit, { withUnit: true });
  if (fromWallet !== null) shown = t("%1 from wallet '%2'", shown, walletDisplayName(fromWallet));
  const name = oneLine(label ?? '', 100);
  return name ? t('To %1: %2 (label \u201c%3\u201d)', address, shown, name) : t('To %1: %2', address, shown);
}
