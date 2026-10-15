#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end tests of the Windows build of chains-app, run from WSL on the Windows of the machine.

Copies chains-app.exe (and test_chains-app.exe, if given) to a folder of its own in the temporary
folder of the Windows user, and runs there, through the interop of WSL:

  - the unit tests of the host (test_chains-app.exe), with --unit;
  - the page scripts of the Linux end-to-end tests (smoke.js, layouts.js, titlebar.js, walletcard.js), each on
    regtest with a data directory of its own and ports of its own, with -webuitestscript: the
    window, the title bar, the bridge, the single instance (a second start hands a payment URI to
    the first one through the named pipe), and the pictures of the pages, copied back to --shots;
  - for the app of a sidechain (chain.json role "sidechain"), sidechain.js instead: the app on Windows
    following a regtest mainchain node run here in WSL (chainsd of --mainchain-bin, reached through the
    localhost of Windows), a deposit, merged mining, a withdrawal and its bundle paid (run_sidechain.py).

Nothing outside that folder is touched: the app settings file and the data of WebView2 are in it,
the chains: URI scheme is not registered (-webuitestscript), and only the processes started from
the folder are ever stopped. The folder is removed at the end unless --keep.

    run_windows.py --app build_win/bin/<stem>-app.exe [--unit build_win/bin/test_<stem>-app.exe] \\
        --shots <dir> [--scripts smoke,layouts,titlebar,walletcard | sidechain] [--mainchain-bin <dir>]

The folder is named after the program (<stem>-app-test), so that the tests of two chains never share one.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from run_smoke import script  # noqa: E402

PORTS = {"smoke": 39470, "layouts": 39490, "titlebar": 39510, "walletcard": 39550, "sidechain": 39570}
CHAIN = json.loads((HERE.parents[1] / "app/src/chain/chain.json").read_text())
SIDECHAIN = CHAIN["role"] == "sidechain"


def win(cmd):
    """Output of a command of cmd.exe."""
    return subprocess.run(["cmd.exe", "/d", "/c", cmd], capture_output=True, text=True, cwd="/mnt/c").stdout.strip()


def to_wsl(path):
    return subprocess.run(["wslpath", "-u", path], capture_output=True, text=True, check=True).stdout.strip()


def to_win(path):
    return subprocess.run(["wslpath", "-w", str(path)], capture_output=True, text=True, check=True).stdout.strip()


def stop_ours(folder_win):
    """Stop the processes started from the test folder (and only those)."""
    ps = ("Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith('%s', 'OrdinalIgnoreCase') } "
          "| ForEach-Object { $_.ProcessId }") % folder_win.replace("'", "''")
    out = subprocess.run(["powershell.exe", "-NoProfile", "-Command", ps], capture_output=True, text=True, cwd="/mnt/c").stdout
    for pid in out.split():
        if pid.isdigit():
            print(f"  stopping {pid}", flush=True)
            subprocess.run(["taskkill.exe", "/PID", pid, "/T", "/F"], capture_output=True, cwd="/mnt/c")


CAPTURE_PS = r"""
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Capture {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[Capture]::SetProcessDPIAware() | Out-Null
$p = Get-Process -Id {pid}
$h = $p.MainWindowHandle
$r = New-Object Capture+RECT
[Capture]::GetWindowRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[Capture]::PrintWindow($h, $dc, 2) | Out-Null
$g.ReleaseHdc($dc)
$bmp.Save('{path}', [System.Drawing.Imaging.ImageFormat]::Png)
"""


def capture_window(pid, path_win):
    """A picture of the whole window of the app, with its frame (PrintWindow, so nothing over it shows)."""
    ps = CAPTURE_PS.replace("{pid}", str(pid)).replace("{path}", path_win.replace("'", "''"))
    return subprocess.run(["powershell.exe", "-NoProfile", "-Command", ps], capture_output=True, text=True, cwd="/mnt/c").returncode == 0


class Run:
    """chains-app on Windows; what the test script says is read from the debug log."""

    def __init__(self, exe, args, log_path, datadir):
        self.datadir = datadir
        self.log = open(log_path, "w", encoding="utf-8")
        self.proc = subprocess.Popen([str(exe)] + args, stdout=self.log, stderr=subprocess.STDOUT, cwd="/mnt/c")
        self.events = {}
        self.seen = 0

    def poll_log(self):
        log = self.datadir / "regtest" / "debug.log"
        if not log.exists():
            return
        with open(log, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        for line in lines[self.seen:]:
            at = line.find("webui test: ")
            if at < 0:
                continue
            message = line[at + len("webui test: "):]
            print("  " + message, flush=True)
            self.events[message.split(" ", 1)[0]] = message
        self.seen = len(lines)

    def wait_event(self, key, timeout):
        until = time.time() + timeout
        while time.time() < until:
            self.poll_log()
            if key in self.events:
                return self.events[key]
            if self.proc.poll() is not None:
                self.poll_log()
                return self.events.get(key)
            time.sleep(0.5)
        return None

    def wait(self, timeout):
        until = time.time() + timeout
        while time.time() < until and self.proc.poll() is None:
            self.poll_log()
            time.sleep(0.5)
        self.poll_log()
        if self.proc.poll() is None:
            return None
        return self.proc.returncode

    def result(self):
        message = self.events.get("RESULT")
        return json.loads(message[len("RESULT "):]) if message else None


def check_shutdown(datadir, failures):
    log = datadir / "regtest" / "debug.log"
    text = log.read_text(errors="replace") if log.exists() else ""
    if "Shutdown done" not in text or "webui: node stopped" not in text:
        failures.append(f"no clean shutdown in {log}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--unit", type=Path, help="test_chains-app.exe, to run its tests on Windows first")
    parser.add_argument("--shots", required=True, type=Path, help="folder for the pictures (on the Linux side)")
    parser.add_argument("--scripts", default="sidechain" if SIDECHAIN else "smoke,layouts,titlebar,walletcard")
    parser.add_argument("--mainchain-bin", type=Path, default=os.environ.get("MAINCHAIN_BIN_DIR"), help="folder of chainsd, for sidechain.js (default: $MAINCHAIN_BIN_DIR)")
    parser.add_argument("--keep", action="store_true", help="keep the folder on Windows")
    args = parser.parse_args()

    temp_win = win("echo %TEMP%")
    if not temp_win or "%" in temp_win:
        print("No Windows here (cmd.exe)", file=sys.stderr)
        return 2
    stem = args.app.name.removesuffix(".exe").removesuffix("-app")
    folder_win = temp_win + f"\\{stem}-app-test"
    folder = Path(to_wsl(folder_win))
    stop_ours(folder_win)
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    args.shots.mkdir(parents=True, exist_ok=True)
    failures = []
    try:
        exe = folder / f"{stem}-app.exe"
        shutil.copy2(args.app, exe)

        if args.unit:
            print(f"== test_{stem}-app", flush=True)
            unit = folder / f"test_{stem}-app.exe"
            shutil.copy2(args.unit, unit)
            started = time.time()
            proc = subprocess.run([str(unit), "--log_level=test_suite", "--", f"-testdatadir={folder_win}\\unit"], capture_output=True, text=True, cwd="/mnt/c", timeout=900)
            (folder / "unit.log").write_text(proc.stdout + proc.stderr)
            tail = (proc.stdout + proc.stderr).strip().splitlines()[-3:]
            print("  " + "\n  ".join(tail) + f"\n  exit {proc.returncode} in {time.time() - started:.0f} s", flush=True)
            shutil.copy2(folder / "unit.log", args.shots / "unit.log")
            if proc.returncode != 0:
                failures.append(f"test_{stem}-app: exit {proc.returncode}")

        for name in [s for s in args.scripts.split(",") if s]:
            print(f"== {name}", flush=True)
            port = PORTS.get(name, 39530)
            base = folder / name
            datadir = base / "datadir"
            datadir.mkdir(parents=True)
            mainchain = None
            extra = []
            if name == "sidechain":
                # The mainchain, here in WSL; the app on Windows reaches it through the localhost of Windows.
                from run_sidechain import Miner, Rpc, SLOT, ACTIVATION_PERIOD, PASSWORD, USER, start_mainchain  # noqa: E402
                main_proc, main = start_mainchain(args.mainchain_bin / "chainsd", base, port + 10)
                main("createwallet", "miner")
                wallet = Rpc(port + 10, "miner")
                coinbase = wallet("getnewaddress")
                wallet("generatetoaddress", 110, coinbase)
                main("createsidechainproposal", SLOT, "E2E sidechain", "The sidechain of the end-to-end test", "11" * 32, "22" * 20)
                wallet("generatetoaddress", ACTIVATION_PERIOD + 1, coinbase)
                miner = Miner(wallet, coinbase, 1.0)
                miner.start()
                mainchain = (main_proc, main, miner, wallet)
                (base / "mainchain.password").write_text(PASSWORD + "\n")
                extra = [f"-sidechainslot={SLOT}", f"-mainchainrpcport={port + 10}", f"-mainchainrpcuser={USER}",
                         f"-mainchainrpcpasswordfile={to_win(base / 'mainchain.password')}", "-mainchainrpcwallet=miner"]
                (base / f"{name}.js").write_text(script(f"{name}.js", f"window.__E2E_SLOT = {SLOT};\nwindow.__E2E_MAINCHAIN = {json.dumps(CHAIN['mainchain']['name'])};\n"))
            else:
                (base / f"{name}.js").write_text(script(f"{name}.js"))
            # The data of WebView2 in the folder too (read by the runtime from the environment).
            env_dir = to_win(base / "webview2")
            os.environ["WEBVIEW2_USER_DATA_FOLDER"] = env_dir
            os.environ["WSLENV"] = "WEBVIEW2_USER_DATA_FOLDER/w"
            common = ["-regtest", f"-port={port}", f"-rpcport={port + 1}", "-listen=0", "-dnsseed=0", "-fixedseeds=0", "-fallbackfee=0.0002", "-webuiregisteruri=0",
                      f"-datadir={to_win(datadir)}", f"-webuiconfig={to_win(base / 'app.json')}"] + extra
            run = Run(exe, common + [f"-webuitestshots={to_win(base / 'shots')}", f"-webuitestscript={to_win(base / (name + '.js'))}"], base / "app.log", datadir)
            if name == "smoke":
                ready = run.wait_event("READY_FOR_URI", 300)
                if ready:
                    pid_file = datadir / "regtest" / f"{CHAIN['bin']}d.pid"
                    if pid_file.exists():
                        frame = base / "shots" / "window-frame.png"
                        frame.parent.mkdir(parents=True, exist_ok=True)
                        if not capture_window(pid_file.read_text().strip(), to_win(frame)):
                            failures.append("no picture of the window")
                    address = ready.split(" ")[1]
                    uri = f"{CHAIN['uriScheme']}:{address}?amount=1.5&label=Smoke%20test"
                    second = subprocess.run([str(exe)] + common + [uri], capture_output=True, text=True, cwd="/mnt/c", timeout=120)
                    out = (second.stdout + second.stderr).strip()
                    print(f"  second start: exit {second.returncode}: {out}", flush=True)
                    if second.returncode != 0 or "handed" not in out:
                        failures.append(f"second start with a URI: exit {second.returncode}, {out[-300:]}")
                else:
                    failures.append("the smoke script never got to the URI step")
            code = run.wait(1800 if name == "sidechain" else 1200)
            if code is None:
                failures.append(f"{name}: did not stop")
                stop_ours(folder_win)
            result = run.result()
            if code != 0 or not result or result["failures"]:
                failures.append(f"{name}: exit {code}, {result['failures'] if result else 'no result'}")
            check_shutdown(datadir, failures)
            shots = base / "shots"
            count = 0
            if shots.is_dir():
                for png in shots.glob("*.png"):
                    shutil.copy2(png, args.shots / png.name)
                    count += 1
            if result:
                missing = [s for s in result["shots"] if not (args.shots / f"{s}.png").is_file()]
                if missing:
                    failures.append(f"{name}: missing pictures: {missing}")
                print(f"  {result['passed']} checks passed, {count} pictures", flush=True)
            shutil.copy2(datadir / "regtest" / "debug.log", args.shots / f"{name}-debug.log") if (datadir / "regtest" / "debug.log").exists() else None
            if mainchain:
                main_proc, main, miner, wallet = mainchain
                paid = run.events.get("WITHDRAWN")
                if paid:
                    address, amount = paid.split(" ")[1:3]
                    for _ in range(120):
                        if float(wallet("getreceivedbyaddress", address, 1)) >= float(amount):
                            break
                        time.sleep(1)
                    received = wallet("getreceivedbyaddress", address, 1)
                    print(f"  {address} received {received} on the mainchain", flush=True)
                    if float(received) != float(amount):
                        failures.append(f"sidechain: the mainchain paid {received}, not {amount}")
                else:
                    failures.append("sidechain: no withdrawal")
                miner.stop.set()
                miner.join(timeout=30)
                try:
                    main("stop")
                    main_proc.wait(timeout=120)
                except Exception:  # noqa: BLE001
                    main_proc.kill()
    finally:
        stop_ours(folder_win)
        if not args.keep:
            for _ in range(10):
                try:
                    shutil.rmtree(folder)
                    break
                except OSError:
                    time.sleep(2)
            if folder.exists():
                print(f"could not remove {folder_win}", flush=True)
    if failures:
        print("FAILED:\n  " + "\n  ".join(failures), flush=True)
        return 1
    print("OK", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
