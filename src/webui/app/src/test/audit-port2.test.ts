// The fixes of the kit from audit port-2 (doc/webui-audit.md; apps/AUDIT-port-2.md): the code block of a
// dialog that shows the marks turning text around (#16), the tables of Home as cards in a narrow panel and
// the balance of a sidechain's Home that never cuts its unit (#14).
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { beforeAll, describe, expect, it } from 'vitest';
import DialogHost from '../components/DialogHost.svelte';
import HomeTables from '../views/home/HomeTables.svelte';
import { ask } from '../lib/dialogs.svelte';
import { revealBidi } from '../lib/format';

const src = (p: string) => readFileSync(join(__dirname, '..', p), 'utf8');

beforeAll(() => {
  const proto = HTMLDialogElement.prototype as unknown as Record<string, unknown>;
  proto.showModal ??= function (this: HTMLDialogElement) {
    this.setAttribute('open', '');
  };
  proto.close ??= function (this: HTMLDialogElement) {
    this.removeAttribute('open');
  };
});

describe('audit port-2', () => {
  it('#16: a code block writes out the marks that would turn it around; the copy keeps them', async () => {
    expect(revealBidi('‮gnp.exe')).toBe('[U+202E]gnp.exe');
    expect(revealBidi('a⁦b⁩‏c؜')).toBe('a[U+2066]b[U+2069][U+200F]c[U+061C]');
    expect(revealBidi('مرحبا hello')).toBe('مرحبا hello');
    render(DialogHost);
    void ask({ title: 'Message', message: 'Sign this?', code: '‮pay 1 to Bob', choices: [{ id: 'ok', label: 'OK' }] });
    const pre = await waitFor(() => {
      const p = document.querySelector('pre');
      expect(p).toBeTruthy();
      return p as HTMLPreElement;
    });
    expect(pre.textContent).toBe('[U+202E]pay 1 to Bob');
    expect(pre.getAttribute('dir')).toBe('auto');
    expect(pre.getAttribute('translate')).toBe('no');
  });

  it('#14: the tables of Home label every cell, and are cards in a panel too narrow for their columns', async () => {
    render(HomeTables, {
      recent: [],
      history: [],
      sidechains: [],
      bundles: [],
      bmm: {},
      bmmSeries: {},
      bmmWindow: 24,
      blocks: [{ height: 12, hash: 'ab'.repeat(32), time: 1_700_000_000, txs: 3 }],
      peers: [],
      now: 1_700_000_060,
      hide: ['sidechains'],
    });
    await fireEvent.click(screen.getByRole('tab', { name: /Blocks/ }));
    const cells = [...document.querySelectorAll('table[aria-label="Latest blocks"] tbody td')] as HTMLElement[];
    expect(cells.map((c) => c.dataset.label)).toEqual(['Block', 'Hash', 'Transactions', 'Age', 'Actions']);
    const css = src('views/home/HomeTables.svelte');
    expect(css).toMatch(/\.scroll \{[^}]*container-type: inline-size/);
    expect(css).toMatch(/@container \(max-width: 52rem\) \{[\s\S]*thead \{\s*display: none;/);
  });

  it("#14: the balance of a sidechain's Home is sized to fit its characters, the unit never cut", () => {
    const home = src('views/home/SideHome.svelte');
    expect(home).toMatch(/class="value big-amount fit"[^>]*style:--chars=\{totalChars\}/);
    expect(home).toMatch(/\.value\.fit :global\(\.amount\) \{\s*font-size: min\(1em, calc\(100cqi \/ \(var\(--chars, 12\) \* 0\.62\)\)\);/);
  });

  it('#2: a cell aligned to the end that is too narrow cuts the end of its content, never its start', () => {
    const css = src('components/DataTable.svelte');
    expect(css).toMatch(/\.cell\.end \{[^}]*justify-content: safe flex-end;/);
    expect(css).toMatch(/\.cell\.end > :global\(\*\) \{\s*min-width: 0;\s*overflow: hidden;\s*text-overflow: ellipsis;/);
  });
});
