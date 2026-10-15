<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  // Copies text to the clipboard of the desktop.
  import Icon from './Icon.svelte';
  import { host } from '../lib/rpc';
  import { toast } from '../lib/state.svelte';

  let { text, label = 'Copy', small = true, iconOnly = true }: { text: string; label?: string; small?: boolean; iconOnly?: boolean } = $props();
  let done = $state(false);

  async function copy() {
    try {
      await host.copy(text);
      done = true;
      setTimeout(() => (done = false), 1400);
    } catch (e) {
      toast('error', t('Could not copy'), e instanceof Error ? e.message : String(e));
    }
  }
</script>

<button type="button" class="btn ghost" class:small class:icon={iconOnly} title={label} aria-label={label} onclick={copy}>
  <Icon name={done ? 'check' : 'copy'} size={small ? 15 : 18} />
  {#if !iconOnly}<span>{done ? 'Copied' : label}</span>{/if}
</button>
