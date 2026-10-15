// The preprocessor of the words of the templates (i18n-preprocess.mjs), for vite.config.ts.
import type { PreprocessorGroup } from 'svelte/compiler';

export function transform(content: string, filename?: string): { code: string; map: unknown } | null;
export default function i18nPreprocess(): PreprocessorGroup;
