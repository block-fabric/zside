// The build of the interface: the main page (vite.config.ts), then the page of the dialogs of the host
// (vite.dialog.config.ts) into the same folder.
//   node scripts/build.mjs [--outDir <dir>] [--emptyOutDir]
import { build } from 'vite';

const args = process.argv.slice(2);
const at = args.indexOf('--outDir');
const outDir = at >= 0 ? args[at + 1] : 'dist';
const emptyOutDir = args.includes('--emptyOutDir') || at < 0;

await build({ configFile: 'vite.config.ts', logLevel: 'warn', build: { outDir, emptyOutDir } });
await build({ configFile: 'vite.dialog.config.ts', logLevel: 'warn', build: { outDir, emptyOutDir: false } });
