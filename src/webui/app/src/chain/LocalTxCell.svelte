<script lang="ts">
  // The cell of the chain's own column of the Transactions page (chain/txinfo.svelte.ts CHAIN_TX_COLUMN): nothing in the
  // template; a sidechain shows what moved beside its coin (an asset, its amount, the other side) for the transaction `txid`.
  let { txid }: { txid: string } = $props();
</script>

<span hidden data-txid={txid}></span>
