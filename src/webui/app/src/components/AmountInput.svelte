<script lang="ts">
  // Typing an amount, in a unit that can be switched here. The value is in satoshis (null while
  // the text is not an amount).
  import { formatAmount, parseAmount, UNITS, type Unit } from '../lib/units';
  import { prefs } from '../lib/state.svelte';

  let {
    value = $bindable(null),
    unit = $bindable(prefs.unit),
    id,
    label = 'Amount',
    max = undefined,
    required = false,
    placeholder = '0.00000000',
    min = undefined,
    minError = 'The amount is below the minimum',
    disabled = false,
    title = undefined,
    invalid = $bindable(''),
    extraError = '',
  }: {
    value?: bigint | null;
    unit?: Unit;
    id: string;
    label?: string;
    max?: bigint;
    required?: boolean;
    placeholder?: string;
    min?: bigint;
    minError?: string;
    disabled?: boolean;
    title?: string;
    /** Out: what is wrong with the text typed ('' when it is an amount, or empty). */
    invalid?: string;
    /** In: an error the page found (dust), shown under the field. */
    extraError?: string;
  } = $props();

  let text = $state(value !== null && value !== undefined ? formatAmount(value, unit, { separators: false, trim: true }) : '');
  let touched = $state(false);

  const parsed = $derived(text.trim() === '' ? null : parseAmount(text, unit));
  const error = $derived.by(() => {
    if (parsed === null) return required && touched ? 'Enter an amount' : '';
    if (!parsed.ok) return parsed.error;
    if (max !== undefined && parsed.sats > max) return 'The amount exceeds your balance';
    if (min !== undefined && parsed.sats < min) return minError;
    return '';
  });

  $effect(() => {
    invalid = error;
  });

  // The value given out last: a value set from outside (use available balance, a payment URI) is shown.
  let emitted: bigint | null | undefined = value;
  $effect(() => {
    const v = parsed && parsed.ok && !error ? parsed.sats : null;
    emitted = v;
    value = v;
  });
  $effect(() => {
    const v = value;
    if (v === emitted) return;
    emitted = v;
    text = v === null || v === undefined ? '' : formatAmount(v, unit, { separators: false, trim: true });
  });

  function switchUnit(next: Unit) {
    // The amount stays the same; the text follows the new unit.
    if (parsed && parsed.ok) text = formatAmount(parsed.sats, next, { separators: false, trim: true });
    unit = next;
  }
</script>

<div class="field">
  <label for={id}>{label}</label>
  <div class="amount-input">
    <input
      {id}
      type="text"
      inputmode="decimal"
      autocomplete="off"
      spellcheck="false"
      class="num"
      {placeholder}
      bind:value={text}
      onblur={() => (touched = true)}
      aria-invalid={error || extraError ? 'true' : undefined}
      aria-describedby={error || extraError ? `${id}-error` : undefined}
      {disabled}
      {title}
    />
    <select aria-label="Unit" {disabled} value={unit} onchange={(e) => switchUnit((e.currentTarget as HTMLSelectElement).value as Unit)}>
      {#each UNITS as u (u.id)}
        <option value={u.id}>{u.short}</option>
      {/each}
    </select>
  </div>
  {#if error || extraError}<span class="error" id="{id}-error">{error || extraError}</span>{/if}
</div>

<style>
  .amount-input {
    display: flex;
  }
  .amount-input input {
    border-top-right-radius: 0;
    border-bottom-right-radius: 0;
    text-align: right;
  }
  .amount-input select {
    width: auto;
    border-left: none;
    border-top-left-radius: 0;
    border-bottom-left-radius: 0;
    background-color: var(--surface-2);
  }
</style>
