<script lang="ts">
  // The privacy indicator of zSide, on every balance, address and transaction of its pages: Private (in
  // the shielded pool: nobody but the sender and the recipient sees the amount or who paid whom) or Public
  // (on the chain for all to see), with a sentence that says why in its tooltip.
  import Icon from '../../components/Icon.svelte';
  import { t } from '../../lib/i18n.svelte';

  let { kind, why = '', bare = false }: { kind: 'private' | 'public' | 'mixed'; why?: string; bare?: boolean } = $props();

  const word = $derived(kind === 'private' ? t('Private') : kind === 'public' ? t('Public') : t('Partly public'));
  const tip = $derived(
    why ||
      (kind === 'private'
        ? t('In the shielded pool: only the sender and the recipient see the amount, the memo and who paid whom.')
        : kind === 'public'
          ? t('Transparent: the amount and the address are on the chain for anyone to see.')
          : t('Part of it shows on the chain: the amount that went in or out of the pool.')),
  );
</script>

<span class="zs-tag {kind}" class:bare title={tip} aria-label={`${word}: ${tip}`}>
  <Icon name={kind === 'private' ? 'shield' : 'eye'} size={13} />{word}
</span>
