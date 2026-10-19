<script lang="ts">
  // A tab of Settings of the chain's own (chain/settings.ts LOCAL_SETTINGS_TABS): nothing in the template.
  let { tab }: { tab: string } = $props();
</script>

<span hidden data-tab={tab}></span>
