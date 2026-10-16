<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  // Notices of new transactions of the wallets (BitcoinGUI::incomingTransaction): date, amount,
  // type, label or address. Many at once (a rescan) make one notice, as in chains-qt.
  import { host } from '../lib/rpc';
  import { node, prefs, toast, wallets } from '../lib/state.svelte';
  import { formatTime } from '../lib/format';
  import { formatAmount } from '../lib/units';
  import { addressText, typeText, walletDisplayName } from '../lib/wallet';
  import { recordsOf } from '../lib/walletdata.svelte';

  const MAX_NOTICES = 3;
  let seen = 0;

  $effect(() => {
    const total = wallets.newTxTotal;
    if (total <= seen) {
      seen = total;
      return;
    }
    const timer = setTimeout(async () => {
      const count = wallets.newTxTotal - seen;
      // The ones kept of them (the latest 100): a count larger than that makes one notice anyway.
      const batch = wallets.newTxs.slice(-Math.min(count, wallets.newTxs.length));
      seen = wallets.newTxTotal;
      // No notices of the desktop while the node catches up (WalletView::processNewTransaction).
      const desktop = !node.tip?.ibd;
      if (count > MAX_NOTICES) {
        toast('info', t('%1 new transactions', count), t('See the transactions page.'));
        if (desktop) host.notify(t('%1 new transactions', count), t('See the transactions page.')).catch(() => undefined);
        return;
      }
      // The records of the new transactions of the current wallet, asked for in one call.
      let fresh: Awaited<ReturnType<typeof recordsOf>> = [];
      try {
        fresh = await recordsOf([...new Set(batch.filter((b) => b.wallet === wallets.current).map((b) => b.txid))]);
      } catch {
        // The wallet went away meanwhile.
      }
      for (const { wallet, txid } of batch) {
        if (wallet !== wallets.current) {
          toast('info', t('New transaction'), t('In wallet %1', walletDisplayName(wallet)));
          if (desktop) host.notify(t('New transaction'), t('In wallet %1', walletDisplayName(wallet))).catch(() => undefined);
          continue;
        }
        // A block of a sidechain that pays its miner nothing (blind merged mining without fees) is no news.
        for (const r of fresh.filter((x) => x.txid === txid && !(x.type === 'mined' && x.amount === 0n))) {
          const incoming = r.amount >= 0n;
          const amount = prefs.mask ? '••••••' : formatAmount(r.amount, prefs.unit, { withUnit: true, plus: true });
          const lines = [t('Date: %1', formatTime(r.time)), t('Amount: %1', amount), t('Type: %1', typeText(r.type, r.role) || t('Other'))];
          if (wallets.list.length > 1) lines.push(t('Wallet: %1', walletDisplayName(wallet)));
          if (r.label) lines.push(t('Label: %1', r.label));
          else if (r.address) lines.push(t('Address: %1', addressText(r)));
          toast(incoming ? 'success' : 'info', incoming ? t('Incoming transaction') : t('Sent transaction'), lines.join('\n'));
          if (desktop) host.notify(incoming ? t('Incoming transaction') : t('Sent transaction'), lines.join('\n')).catch(() => undefined);
        }
      }
    }, 600);
    return () => clearTimeout(timer);
  });
</script>
