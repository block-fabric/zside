<script lang="ts">
  // The coin panel of the Send page with its selection bound, shown for the test.
  import CoinPanel from '../components/CoinPanel.svelte';
  import type { Outpoint } from '../lib/wallet';

  let { amounts }: { amounts: bigint[] } = $props();
  let selected: Outpoint[] = $state([]);
</script>

<CoinPanel wallet="w" bind:selected {amounts} subtractFee={false} fee={{ confTarget: 6 }} />
<output data-testid="selected">{JSON.stringify(selected)}</output>
