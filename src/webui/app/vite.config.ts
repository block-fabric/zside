// The interface of chains-app. Built into the program (src/webui/cmake/embed.cmake),
// served from chains-app://app/ with a strict content security policy.
import { defineConfig } from 'vitest/config';
import { svelteTesting } from '@testing-library/svelte/vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
// The words of the templates, opted in to translation (src/lib/i18n.svelte.ts).
import i18nPreprocess from './scripts/i18n-preprocess.mjs';

export default defineConfig({
  plugins: [svelte({ preprocess: [i18nPreprocess()] }), svelteTesting()],
  base: '/',
  build: {
    outDir: 'dist',
    target: 'es2022',
    // One script and one style sheet: what the inline document of other platforms needs too.
    cssCodeSplit: false,
    modulePreload: { polyfill: false },
    assetsInlineLimit: 0,
    sourcemap: false,
    reportCompressedSize: false,
    rolldownOptions: {
      output: {
        codeSplitting: false,
      },
    },
  },
  test: {
    environment: 'jsdom',
    include: ['src/**/*.test.ts'],
    // The colours of the built-in themes, read from themes.css (?raw) by lib/customthemes.ts: the tests need its text.
    css: { include: [/themes\.css/] },
  },
});
