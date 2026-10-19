<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  // Notices of new transactions of the wallets (BitcoinGUI::incomingTransaction): date, amount,
  // type, label or address. Many at once (a rescan) make one notice, as in chains-qt.
  // One notice per transaction (noticeRecordOf): its halves to the wallet itself are merged, the net shown,
  // with what the chain knows of it (the asset it moved: chain/txinfo.svelte.ts) when it knows more.
  import { host } from '../lib/rpc';
  import { node, prefs, toast, wallets } from '../lib/state.svelte';
  import { formatTime } from '../lib/format';
  import { formatAmount } from '../lib/units';
  import { addressText, noticeRecordOf, typeText, walletDisplayName } from '../lib/wallet';
  import { recordsOf } from '../lib/walletdata.svelte';
  import { CHAIN_TX_CSV, CHAIN_TX_FILTER, chainTxOf, refreshChainTx, type ChainTx } from '../chain/txinfo.svelte';
  import { chainTxHidden, chainTxNotice } from '../lib/chaintx';

  /** What the chain knows of the transactions, waited for a little (its store reads them again on the news). */
  async function chainTxs(wallet: string, txids: string[]): Promise<Map<string, ChainTx>> {
    const out = new Map<string, ChainTx>();
    if (!CHAIN_TX_FILTER || txids.length === 0) return out;
    refreshChainTx(wallet);
    for (let i = 0; i < 12; i++) {
      for (const txid of txids) {
        const c = chainTxOf(txid);
        if (c) out.set(txid, c);
      }
      if (out.size === txids.length || wallets.current !== wallet) break;
      await new Promise((r) => setTimeout(r, 250));
    }
    return out;
  }

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
      const ownTxids = [...new Set(batch.filter((b) => b.wallet === wallets.current).map((b) => b.txid))];
      const shownWallet = wallets.current;
      try {
        fresh = await recordsOf(ownTxids);
      } catch {
        // The wallet went away meanwhile.
      }
      const known = shownWallet === null ? new Map<string, ChainTx>() : await chainTxs(shownWallet, ownTxids);
      const told = new Set<string>();
      for (const { wallet, txid } of batch) {
        if (wallet !== wallets.current) {
          toast('info', t('New transaction'), t('In wallet %1', walletDisplayName(wallet)));
          if (desktop) host.notify(t('New transaction'), t('In wallet %1', walletDisplayName(wallet))).catch(() => undefined);
          continue;
        }
        // One notice per transaction (its halves merged); a block of a sidechain that pays its miner nothing
        // (blind merged mining without fees) is no news.
        if (told.has(txid)) continue;
        told.add(txid);
        const r = noticeRecordOf(fresh, txid);
        if (!r) continue;
        const chainTx = known.get(txid);
        // A transaction the chain hides (an asset the wallet ignores): no notice.
        if (chainTx && chainTxHidden(txid)) continue;
        const extra = chainTx ? chainTxNotice(txid) : undefined;
        // A net of nothing out of the wallet's own coins (a move to itself) is a send, not an income of 0.
        const incoming = r.amount > 0n || (r.amount === 0n && r.debit === 0n);
        const amount = prefs.mask ? '••••••' : formatAmount(r.amount, prefs.unit, { withUnit: true, plus: true });
        const lines = [t('Date: %1', formatTime(r.time)), t('Amount: %1', amount), t('Type: %1', chainTx?.type || typeText(r.type, r.role) || t('Other'))];
        // What it did beyond the coin (the asset, its number, how much, the other side), as the CSV export says it.
        if (chainTx && !prefs.mask) CHAIN_TX_CSV.forEach((column, i) => { if (chainTx.csv[i]) lines.push(`${t(column)}: ${chainTx.csv[i]}`); });
        if (extra?.warning) lines.push(extra.warning);
        if (wallets.list.length > 1) lines.push(t('Wallet: %1', walletDisplayName(wallet)));
        if (r.label) lines.push(t('Label: %1', r.label));
        else if (r.address) lines.push(t('Address: %1', addressText(r)));
        toast(extra?.warning ? 'warning' : incoming ? 'success' : 'info', incoming ? t('Incoming transaction') : t('Sent transaction'), lines.join('\n'), extra?.actions?.length ? 9000 : 4500, extra?.actions);
        if (desktop) host.notify(incoming ? t('Incoming transaction') : t('Sent transaction'), lines.join('\n')).catch(() => undefined);
      }
    }, 600);
    return () => clearTimeout(timer);
  });
</script>
