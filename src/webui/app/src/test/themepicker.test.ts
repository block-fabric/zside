import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import ThemePicker from '../components/ThemePicker.svelte';

const groups = [
  { title: '', options: [{ id: 'midnight', name: 'Midnight' }, { id: 'mocha', name: 'Catppuccin Mocha' }, { id: 'dracula', name: 'Dracula' }] },
  { title: 'Your themes', options: [{ id: 'custom-1', name: 'Peach <b>', own: true }] },
];

describe('the theme list of the sidebar', () => {
  it('opens above its button, inside the window, and picks with the keyboard or a click', async () => {
    const onpick = vi.fn();
    render(ThemePicker, { value: 'mocha', groups, onpick });
    const button = screen.getByRole('button', { name: 'Theme' });
    expect(button.textContent).toContain('Catppuccin Mocha');
    expect(screen.queryByRole('listbox')).toBeNull();

    await fireEvent.keyDown(button, { key: 'Enter' });
    const list = await screen.findByRole('listbox', { name: 'Theme' });
    expect(button.getAttribute('aria-expanded')).toBe('true');
    // Anchored by its bottom above the button (upwards), never past the top of the window.
    expect(list.style.bottom).not.toBe('');
    expect(list.style.top).toBe('');
    expect(Number.parseFloat(list.style.maxHeight)).toBeGreaterThan(0);
    // On the theme in use, marked.
    expect(screen.getByRole('option', { name: /Catppuccin Mocha/ }).getAttribute('aria-selected')).toBe('true');
    expect(list.getAttribute('aria-activedescendant')).toBe(screen.getByRole('option', { name: /Catppuccin Mocha/ }).id);
    // The user's theme: its name as text, never translated.
    const own = screen.getByRole('option', { name: /Peach/ });
    expect(own.textContent).toContain('Peach <b>');
    expect(own.getAttribute('translate')).toBe('no');

    await fireEvent.keyDown(list, { key: 'ArrowDown' });
    await fireEvent.keyDown(list, { key: 'Enter' });
    expect(onpick).toHaveBeenCalledWith('dracula');
    expect(screen.queryByRole('listbox')).toBeNull();

    await fireEvent.click(button);
    await fireEvent.keyDown(screen.getByRole('listbox'), { key: 'Escape' });
    expect(screen.queryByRole('listbox')).toBeNull();

    await fireEvent.click(button);
    await fireEvent.click(screen.getByRole('option', { name: /Midnight/ }));
    expect(onpick).toHaveBeenLastCalledWith('midnight');
  });
});
