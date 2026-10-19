// The icon of the app is the chain's own mark (app/scripts/appicon.py), never the coin of Bitcoin, on Linux
// (the window, the tray, the desktop entry) and on Windows (the program, its window and tray icon, the
// installer). User feedback 2026-10-19: the tray of every app showed the Bitcoin icon.
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

const ROOT = resolve(__dirname, '../../../../..');
const read = (p: string) => readFileSync(resolve(ROOT, p));

describe('the icon of the app', () => {
  it('is not the coin of Bitcoin', () => {
    const icon = read('src/webui/app/public/icon-256.png');
    expect(icon.subarray(1, 4).toString()).toBe('PNG');
    for (const bitcoin of ['share/pixmaps/bitcoin256.png', 'share/pixmaps/bitcoin.png']) {
      let other: Buffer | null = null;
      try {
        other = read(bitcoin);
      } catch {
        continue;
      }
      expect(icon.equals(other), bitcoin).toBe(false);
    }
  });

  it('is the one of the Windows program and installer, and the desktop entry finds it by the app id', () => {
    const rc = read('src/webui/res/chains-app-res.rc').toString();
    expect(rc).toMatch(/1 ICON DISCARDABLE "icons\/app\.ico"/);
    expect(rc).not.toMatch(/bitcoin/);
    expect(read('src/webui/res/setup-app.nsi.in').toString()).toMatch(/MUI_ICON .*src\/webui\/res\/icons\/app\.ico/);
    expect(read('src/webui/res/icons/app.ico').readUInt16LE(2)).toBe(1);
    expect(read('src/webui/CMakeLists.txt').toString()).toMatch(/app\/public\/icon-256\.png\s+DESTINATION \$\{CMAKE_INSTALL_DATADIR\}\/icons\/hicolor\/256x256\/apps/);
    // GTK names the window <bin>-app (X11 class, Wayland app id), as StartupWMClass of the desktop entry.
    expect(read('src/webui/window_gtk.cpp').toString()).toMatch(/g_set_prgname\(\(std::string\{chain::STEM\} \+ "-app"\)/);
    expect(read('share/app.desktop.in').toString()).toMatch(/StartupWMClass=@CLIENT_BIN_NAME@-app/);
  });
});
