<script lang="ts">
  // What this chain sends beside its coin (an asset, a name...), at the top of the Send page
  // (pages/Send.svelte). The template sends only the coin: nothing here, and the kit's form stays. A chain
  // that sends more shows its choice here and, with something else than the coin chosen, its own form, and
  // sets `active` so that the kit's form steps aside.
  let { active = $bindable(false) }: { active?: boolean } = $props();
</script>
