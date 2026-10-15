<script lang="ts">
  // The round buttons at the right of the top bars: the notifications and the light/dark switch.
  import NotificationBell from './NotificationBell.svelte';
  import ThemeToggle from './ThemeToggle.svelte';
</script>

<div class="top-actions">
  <NotificationBell />
  <ThemeToggle />
</div>

<style>
  .top-actions {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    flex: none;
  }
</style>
