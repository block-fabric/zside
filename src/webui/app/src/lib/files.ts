// Files of the user: chosen in a file chooser of the desktop (the host remembers which), then read
// or written through the host. The page cannot reach any other file.
import { request } from './bridge';

export interface FileFilter {
  name: string;
  patterns: string[];
}

export const FILTERS = {
  wallet: { name: 'Wallet Data', patterns: ['*.dat'] },
  walletBackup: { name: 'Wallet Data', patterns: ['*.dat', '*.wallet'] },
  all: { name: 'All Files', patterns: ['*'] },
  psbt: { name: 'Partially Signed Transaction (Binary)', patterns: ['*.psbt'] },
  csv: { name: 'Comma separated file', patterns: ['*.csv'] },
  png: { name: 'PNG Image', patterns: ['*.png'] },
  theme: { name: 'Theme (JSON)', patterns: ['*.json'] },
} satisfies Record<string, FileFilter>;

export interface ChooseOptions {
  save?: boolean;
  title: string;
  /** The name proposed when saving. */
  name?: string;
  filters?: FileFilter[];
}

/** The path the user chose, or null when cancelled. */
export function chooseFile(options: ChooseOptions): Promise<string | null> {
  return request<string | null>('host', 'file.choose', { ...options, save: !!options.save, filters: options.filters ?? [] });
}

export function readFile(path: string): Promise<{ path: string; size: number; base64: string }> {
  return request('host', 'file.read', { path });
}

export function writeText(path: string, text: string): Promise<string> {
  return request('host', 'file.write', { path, text });
}

export function writeBase64(path: string, base64: string): Promise<string> {
  return request('host', 'file.write', { path, base64 });
}

/** Choose where to save, and save text there; the path, or null when cancelled. */
export async function saveTextAs(options: Omit<ChooseOptions, 'save'>, text: string): Promise<string | null> {
  const path = await chooseFile({ ...options, save: true });
  if (!path) return null;
  await writeText(path, text);
  return path;
}

export async function saveBase64As(options: Omit<ChooseOptions, 'save'>, base64: string): Promise<string | null> {
  const path = await chooseFile({ ...options, save: true });
  if (!path) return null;
  await writeBase64(path, base64);
  return path;
}

/** Choose a file and read it; null when cancelled. */
export async function openFile(options: Omit<ChooseOptions, 'save'>): Promise<{ path: string; size: number; base64: string } | null> {
  const path = await chooseFile({ ...options, save: false });
  if (!path) return null;
  return readFile(path);
}

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

export function bytesToBase64(bytes: Uint8Array): string {
  let out = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const a = bytes[i];
    const b = i + 1 < bytes.length ? bytes[i + 1] : 0;
    const c = i + 2 < bytes.length ? bytes[i + 2] : 0;
    const triple = (a << 16) | (b << 8) | c;
    out += B64[(triple >> 18) & 63] + B64[(triple >> 12) & 63];
    out += i + 1 < bytes.length ? B64[(triple >> 6) & 63] : '=';
    out += i + 2 < bytes.length ? B64[triple & 63] : '=';
  }
  return out;
}

export function base64ToBytes(text: string): Uint8Array {
  const clean = text.replace(/[^A-Za-z0-9+/]/g, '');
  const out: number[] = [];
  for (let i = 0; i < clean.length; i += 4) {
    const v = [0, 1, 2, 3].map((k) => (i + k < clean.length ? B64.indexOf(clean[i + k]) : -1));
    const triple = (v[0] << 18) | (v[1] << 12) | ((v[2] < 0 ? 0 : v[2]) << 6) | (v[3] < 0 ? 0 : v[3]);
    out.push((triple >> 16) & 255);
    if (v[2] >= 0) out.push((triple >> 8) & 255);
    if (v[3] >= 0) out.push(triple & 255);
  }
  return new Uint8Array(out);
}

/** A file name without the characters file systems refuse. */
export function safeFileName(name: string): string {
  return name.replace(/[\\/:*?"<>|\u0000-\u001f]/g, '_').slice(0, 180);
}
