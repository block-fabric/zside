<script lang="ts">
  import AmountInput from '../components/AmountInput.svelte';

  let value: bigint | null = $state(null);
  /** Set the value from outside, as "use available balance" does. */
  export function set(v: bigint | null) {
    value = v;
  }
</script>

<AmountInput id="harness-amount" bind:value />
<span data-testid="value">{value === null ? '' : String(value)}</span>
