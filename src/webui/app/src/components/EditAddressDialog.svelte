<script lang="ts" module>
  export type EditMode = 'new-send' | 'edit-send' | 'edit-receive';
</script>

<script lang="ts">
  import { t } from '../lib/i18n.svelte';
  // Add or edit an entry of the address book (EditAddressDialog).
  import Dialog from './Dialog.svelte';
  import { errorText } from '../lib/dialogs.svelte';
  import { toast } from '../lib/state.svelte';
  import { walletApi } from '../lib/wallet';
  import { refreshAddresses } from '../lib/walletdata.svelte';

  let {
    edit = $bindable(null),
    wallet,
  }: { edit?: { mode: EditMode; address: string; label: string } | null; wallet: string } = $props();

  let open = $state(false);
  let label = $state('');
  let address = $state('');
  let error = $state('');
  let busy = $state(false);

  $effect(() => {
    const e = edit;
    open = e !== null;
    error = '';
    if (e) {
      label = e.label;
      address = e.address;
    }
  });

  const title = $derived(edit?.mode === 'new-send' ? 'New sending address' : edit?.mode === 'edit-send' ? 'Edit sending address' : 'Edit receiving address');

  async function save() {
    if (!edit || busy) return;
    busy = true;
    error = '';
    try {
      const api = walletApi(wallet);
      if (edit.mode === 'new-send') {
        await api.addSending(address.trim(), label);
      } else {
        const changed = address.trim() !== edit.address;
        await api.editAddress(edit.address, label, edit.mode === 'edit-send' && changed ? address.trim() : undefined);
      }
      await refreshAddresses(true);
      toast('success', t('Address book'), edit.mode === 'new-send' ? t('Address added') : t('Address saved'));
      edit = null;
    } catch (e) {
      error = errorText(e);
    } finally {
      busy = false;
    }
  }
</script>

<Dialog bind:open {title} onclose={() => (edit = null)}>
  <form
    id="edit-address-form"
    class="stack"
    onsubmit={(e) => {
      e.preventDefault();
      save();
    }}
  >
    {#if error}<p class="error break" role="alert">{error}</p>{/if}
    <div class="field">
      <label for="edit-label">Label</label>
      <!-- svelte-ignore a11y_autofocus -->
      <input id="edit-label" type="text" bind:value={label} autocomplete="off" title="The label associated with this address list entry" autofocus />
    </div>
    <div class="field">
      <label for="edit-address">Address</label>
      <input
        id="edit-address"
        type="text"
        class="mono"
        bind:value={address}
        autocomplete="off"
        spellcheck="false"
        disabled={edit?.mode === 'edit-receive'}
        title="The address associated with this address list entry. This can only be modified for sending addresses."
      />
    </div>
  </form>
  {#snippet actions()}
    <button type="button" class="btn" onclick={() => (edit = null)}>Cancel</button>
    <button type="submit" form="edit-address-form" class="btn primary" disabled={busy || !address.trim()}>OK</button>
  {/snippet}
</Dialog>

<style>
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.8rem;
  }
</style>
