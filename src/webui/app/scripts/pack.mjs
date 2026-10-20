// The pages of this chain as a pack for the Chains app (its doc/unified-app.md): the interface built to be served
// under /packs/<bin>/ of that app, which shows it in its own window and answers its host methods through the daemon of
// this chain (src/webui/remotehost.h). Writes the pages and pack.json:
//   {stem, name, hostApi, commit, brand, coin}
//   node scripts/pack.mjs --outDir <dir> [--commit <id>]
// hostApi is the version of what the pack asks of its host (the methods of the daemon's webuihost and those the app
// answers itself): the Chains app takes a pack only of the hostApi it speaks, and a daemon of the same.
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { build } from 'vite';

export const HOST_API = 1;

const args = process.argv.slice(2);
const arg = (name) => {
  const at = args.indexOf(name);
  return at >= 0 ? args[at + 1] : undefined;
};
const outDir = arg('--outDir');
if (!outDir) throw new Error('pack: --outDir <dir> is needed');
const chain = JSON.parse(readFileSync(new URL('../src/chain/chain.json', import.meta.url), 'utf8'));
if (chain.role !== 'sidechain') throw new Error(`pack: ${chain.name} is not a sidechain`);
const stem = chain.bin;
if (!/^[a-z0-9]+$/.test(stem)) throw new Error(`pack: the bin of chain.json is plain letters and digits, not ${stem}`);

await build({ configFile: 'vite.config.ts', logLevel: 'warn', base: `/packs/${stem}/`, build: { outDir, emptyOutDir: true } });
const pack = {
  stem,
  name: chain.name,
  hostApi: HOST_API,
  commit: arg('--commit') ?? '',
  brand: { color: chain.brand?.color ?? '', mark: chain.brand?.mark ?? [] },
  coin: chain.coin,
};
writeFileSync(join(outDir, 'pack.json'), `${JSON.stringify(pack, null, 2)}\n`);
console.log(`pack: ${chain.name} (${stem}) in ${outDir}`);
