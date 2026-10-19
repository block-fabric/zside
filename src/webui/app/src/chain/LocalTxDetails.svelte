<script lang="ts">
  // The details of a transaction of the wallet (components/TxDetailsView.svelte), the chain's part: nothing in the
  // template; a sidechain shows there what the transaction did beyond its coin (BitAssets: the asset moved, its
  // amount, the other side, the operation's own facts), above the details of the coin.
  let { wallet, txid }: { wallet: string; txid: string } = $props();
</script>

<span hidden data-wallet={wallet} data-txid={txid}></span>
