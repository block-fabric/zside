<script lang="ts">
  // What this chain keeps doing whichever page shows, from the start of the app (views/Shell.svelte mounts it once the
  // node runs): a sidechain that sends something by itself later (the registration of a name reserved, say) starts its
  // store here, so that it goes on after a restart and on any page. The template does nothing; a sidechain replaces this
  // file, so that a merge of the template never conflicts with it.
</script>
