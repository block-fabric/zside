<script lang="ts">
  import DataTable from '../components/DataTable.svelte';
  import type { Column } from '../lib/table';

  let { count }: { count: number } = $props();
  const rows = $derived(Array.from({ length: count }, (_, n) => ({ id: String(n), n })));
  const columns: Column<{ id: string; n: number }>[] = [{ key: 'n', label: 'Number', value: (r) => r.n }];
</script>

<div class="holder"><DataTable {rows} {columns} key={(r) => r.id} label="Numbers" /></div>
