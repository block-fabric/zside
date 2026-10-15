// The hooks a chain made from the template fills with its own (audit port-2 #7): its icons
// (chain/icons.ts), its cards on the home (chain/LocalCards.svelte), what it sends beside its coin
// (chain/LocalSend.svelte), Alt+6 and on for its pages (chain/local.ts `shortcut`), the weight of its
// blocks (app.info.blockWeight, from the node). Here with a pretend chain of the template's: one page with Alt+6
// that needs a wallet, one icon.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it, vi } from 'vitest';

vi.mock('../chain/local', () => ({
  LOCAL_PAGES: [{ id: 'names', title: 'Names', icon: 'names', section: 'chain', summary: 'Names', wallet: true, shortcut: 'Alt+6' }],
  LOCAL_COMMANDS: [],
}));
vi.mock('../chain/icons', () => ({ LOCAL_ICONS: { names: 'M4 5h16v14H4z' } }));

const src = (p: string) => readFileSync(join(__dirname, '..', p), 'utf8');

describe('the hooks of a chain of its own', () => {
  it('Alt+6 opens a page of the chain that names it, once a wallet is there', async () => {
    const { handleShortcut } = await import('../lib/keyboard');
    const { nav } = await import('../lib/nav.svelte');
    const { wallets } = await import('../lib/state.svelte');
    wallets.list = [];
    expect(handleShortcut(new KeyboardEvent('keydown', { key: '6', altKey: true }))).toBe(true);
    expect(nav.page).not.toBe('names');
    wallets.list = [{ name: 'w' } as never];
    handleShortcut(new KeyboardEvent('keydown', { key: '6', altKey: true }));
    expect(nav.page).toBe('names');
    // Never over the kit's: Alt+1 is still the overview.
    handleShortcut(new KeyboardEvent('keydown', { key: '1', altKey: true }));
    expect(nav.page).toBe('overview');
  });

  it("the chain's icons are icons of the kit", async () => {
    const { ICONS } = await import('../components/Icon.svelte');
    expect((ICONS as Record<string, string>).names).toBe('M4 5h16v14H4z');
    expect(ICONS.overview).toBeTruthy();
  });

  it('the home shows the cards of the chain, Send its own form', () => {
    expect(src('views/home/SideHome.svelte')).toMatch(/import LocalCards from '..\/..\/chain\/LocalCards.svelte'[\s\S]*<LocalCards \/>/);
    expect(src('pages/Send.svelte')).toMatch(/<LocalSend bind:active=\{sendingOwn\} \/>\s*\{#if !sendingOwn\}/);
    // The template's own hooks are empty.
    expect(src('chain/LocalSend.svelte')).toMatch(/active = \$bindable\(false\)/);
  });

  it('blocks are drawn against the weight the node says for its network (app.info.blockWeight)', async () => {
    const { render } = await import('@testing-library/svelte');
    const BlockBox = (await import('../components/BlockBox.svelte')).default;
    const { app } = await import('../lib/state.svelte');
    const block = { height: 5, hash: 'ab'.repeat(32), time: 1_700_000_000, nTx: 10, weight: 3_000_000, subsidy: null, fees: null, feeRate: null, tag: '', bmm: [] };
    app.info = { blockWeight: 6_000_000 } as never;
    const { container } = render(BlockBox, { block, secs: 60, median: 60, now: 1_700_000_060, onselect: () => {} });
    expect((container.querySelector('.blk-face') as HTMLElement).style.getPropertyValue('--fill')).toBe('50%');
    // The host says it (app.cpp, consensus.max_block_weight of the network).
    expect(readFileSync(join(__dirname, '../../../app.cpp'), 'utf8')).toMatch(/info\.pushKV\("blockWeight", Params\(\)\.GetConsensus\(\)\.max_block_weight\)/);
  });
});
