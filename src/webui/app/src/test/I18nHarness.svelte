<script lang="ts">
  // Words of a template (translated when built) next to text from outside (never translated).
  import SafeText from '../components/SafeText.svelte';
  import { t } from '../lib/i18n.svelte';

  let { hours = 5, label = 'Send', tabs = [{ id: 'a', label: 'Mask values' }] }: { hours?: number; label?: string; tabs?: { id: string; label: string }[] } = $props();
  const busy = false;
</script>

<main>
  <button title="Mask values">Send</button>
  <input placeholder="Enter address or label to search" />
  <p id="behind">{hours} hours behind</p>
  <p id="behind-own">{hours} behind</p>
  <p id="label"><SafeText text={label} /></p>
  <p id="bare">{label}</p>
  <span translate="no" id="no">Send</span>
  <b id="cond">{busy ? 'Working' : 'Send'}</b>
  {#each tabs as tab (tab.id)}<i id="tab-{tab.id}">{t(tab.label)}</i>{/each}
  <code>Send</code>
</main>
