<script lang="ts">
  // Text that comes from the chain, peers, the wallet or the user (a label, a memo, the title of a
  // sidechain, the version string of a peer, a payment URI): shown exactly as it is. It is never looked up
  // in the catalogs of the app (src/lib/i18n.svelte.ts), and translate="no" keeps the translator of the
  // browser off it too. src/test/i18n-lint.test.ts finds such fields shown without it.
  // dir="auto" isolates it as to direction (unicode-bidi: isolate): a mark that turns text around (U+202E)
  // inside it cannot turn the words of the app around it (audit port-1 #2).
  let { text, class: className = '', title = undefined }: { text: string | null | undefined; class?: string; title?: string } = $props();
</script>

<span class={className || undefined} translate="no" dir="auto" {title}>{text ?? ''}</span>
