#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The release of the app as a user gets it (target deploy-app, built with WEBUI_TEST_HOOKS=OFF).

Audit port-1 #6: no release had ever been built or run. This unpacks the package (the Linux tarball, or the
Windows zip), checks what is in it (the app and the programs of the node, the desktop entry and the icon on
Linux, the README and the licence; SHA256SUMS), that the app has none of the hooks of the tests, then runs it
as a user would, with no test script: on regtest, with its RPC server on, against a live mainchain node (when
MAINCHAIN_BIN_DIR or --mainchain-bin is given, for the app of a sidechain). Through the command line program
of the package it checks that the node of the app runs, follows the mainchain in its slot, has the RPC
methods of the chain, and stops cleanly when asked.

On Windows (--windows, from WSL), the zip is unpacked and run in a folder of its own in the temporary folder
of the Windows user, with -webuiregisteruri=0 (the URI scheme is not registered), and the folder is removed
at the end; only the processes started from it are ever stopped.

    run_release.py --package build/release-app/<stem>-app-<version>-<arch>-linux-gnu.tar.gz --tmp <dir>
    run_release.py --windows --package build_win/release-app/<stem>-app-<version>-win64.zip --tmp <dir>
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from run_sidechain import ACTIVATION_PERIOD, PASSWORD, SLOT, USER, Rpc, start_mainchain  # noqa: E402

CHAIN = json.loads((HERE.parents[1] / "app/src/chain/chain.json").read_text())
STEM = CHAIN["bin"]
SIDECHAIN = CHAIN["role"] == "sidechain"
#: What only a build for the tests has (cmake/check_test_hooks.cmake).
HOOKS = [b"test.confirmAnswer", b"test.fileAnswer", b"test.confirmed", b"-webuitestscript=<file>"]


def check(failures, name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'} {name}{'' if ok or not detail else ' (' + str(detail) + ')'}", flush=True)
    if not ok:
        failures.append(name)


def win(cmd):
    return subprocess.run(["cmd.exe", "/d", "/c", cmd], capture_output=True, text=True, cwd="/mnt/c").stdout.strip()


def to_wsl(path):
    return subprocess.run(["wslpath", "-u", path], capture_output=True, text=True, check=True).stdout.strip()


def to_win(path):
    return subprocess.run(["wslpath", "-w", str(path)], capture_output=True, text=True, check=True).stdout.strip()


def stop_ours_windows(folder_win):
    ps = ("Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith('%s', 'OrdinalIgnoreCase') } "
          "| ForEach-Object { $_.ProcessId }") % folder_win.replace("'", "''")
    out = subprocess.run(["powershell.exe", "-NoProfile", "-Command", ps], capture_output=True, text=True, cwd="/mnt/c").stdout
    for pid in out.split():
        if pid.isdigit():
            subprocess.run(["taskkill.exe", "/PID", pid, "/T", "/F"], capture_output=True, cwd="/mnt/c")


def unpack(package, into):
    if package.name.endswith(".tar.gz"):
        with tarfile.open(package) as tar:
            tar.extractall(into, filter="data")
    else:
        with zipfile.ZipFile(package) as z:
            z.extractall(into)
    tops = [p for p in into.iterdir()]
    return tops[0] if len(tops) == 1 and tops[0].is_dir() else into


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--tmp", required=True, type=Path)
    parser.add_argument("--windows", action="store_true", help="run the Windows zip on the Windows of the machine (from WSL)")
    parser.add_argument("--mainchain-bin", type=Path, default=os.environ.get("MAINCHAIN_BIN_DIR"))
    parser.add_argument("--port", type=int, default=39710)
    args = parser.parse_args()
    failures = []

    base = args.tmp.resolve()
    if base.exists():
        shutil.rmtree(base)
    base.mkdir(parents=True)

    print(f"== the package {args.package.name}", flush=True)
    sums = args.package.parent / "SHA256SUMS"
    digest = hashlib.sha256(args.package.read_bytes()).hexdigest()
    check(failures, "SHA256SUMS names the package with its hash", sums.exists() and f"{digest}  {args.package.name}" in sums.read_text(), sums)
    root = unpack(args.package, base / "unpacked")
    if args.windows:
        # The folder of the installation: the app at the top, the programs of the node in daemon.
        app = root / f"{STEM}-app.exe"
        bindir = root / "daemon"
        exe = ".exe"
        expected = [app, bindir / f"{STEM}d.exe", bindir / f"{STEM}-cli.exe", root / "COPYING.txt", root / "readme.txt"]
        setup = args.package.parent / args.package.name.replace(".zip", "-setup.exe")
        check(failures, "the installer is next to the zip, in SHA256SUMS", setup.exists() and setup.name in sums.read_text(), setup)
    else:
        bindir = root / "bin"
        app = bindir / f"{STEM}-app"
        exe = ""
        expected = [app, bindir / f"{STEM}d", bindir / f"{STEM}-cli", root / "COPYING", root / f"share/applications/{STEM}-app.desktop", root / "README.md"]
    missing = [str(p.relative_to(root)) for p in expected if not p.exists()]
    check(failures, "the app, the node, its command line program, the licence and the readme (and the desktop entry on Linux) are in it", not missing, missing)
    del exe
    if not args.windows:
        icons = list(root.glob(f"share/icons/**/{STEM}*.png")) + list(root.glob(f"share/pixmaps/{STEM}*"))
        check(failures, "the icon of the app is in it", bool(icons))
        desktop = (root / f"share/applications/{STEM}-app.desktop").read_text() if (root / f"share/applications/{STEM}-app.desktop").exists() else ""
        check(failures, f"the desktop entry opens {CHAIN['uriScheme']}: links with the app", f"x-scheme-handler/{CHAIN['uriScheme']}" in desktop, desktop[:200])
    data = app.read_bytes() if app.exists() else b""
    found = [h.decode() for h in HOOKS if h in data]
    check(failures, "the app has none of the hooks of the tests", app.exists() and not found, found)
    if failures:
        print(f"FAILED: {failures}")
        return 1

    # --- Run it as a user would: no test script ---
    main_proc = miner_wallet = None
    folder_win = None
    proc = None
    try:
        node_args = ["-regtest", f"-port={args.port + 2}", f"-rpcport={args.port + 3}", "-listen=0", "-dnsseed=0", "-fixedseeds=0", "-server",
                     "-rpcuser=rel", "-rpcpassword=release-test", "-min"]
        if SIDECHAIN and args.mainchain_bin:
            print("== the mainchain", flush=True)
            main_proc, main_rpc = start_mainchain(args.mainchain_bin / "chainsd", base, args.port)
            main_rpc("createwallet", "miner")
            miner_wallet = Rpc(args.port, "miner")
            coinbase = miner_wallet("getnewaddress")
            miner_wallet("generatetoaddress", 110, coinbase)
            main_rpc("createsidechainproposal", SLOT, "Release test", "The sidechain of the release test", "11" * 32, "22" * 20)
            miner_wallet("generatetoaddress", ACTIVATION_PERIOD + 1, coinbase)
            password_file = base / "mainchain.password"
            password_file.write_text(PASSWORD + "\n")
            password_file.chmod(0o600)
            mainchain_host = "127.0.0.1"
            if args.windows:
                # The mainchain node runs in WSL: the app on Windows reaches it at localhost (WSL forwards it).
                mainchain_host = "127.0.0.1"
            node_args += [f"-sidechainslot={SLOT}", f"-mainchainrpcconnect={mainchain_host}", f"-mainchainrpcport={args.port}", f"-mainchainrpcuser={USER}",
                          f"-mainchainrpcpassword={PASSWORD}" if args.windows else f"-mainchainrpcpasswordfile={password_file}", "-mainchainrpcwallet=miner"]

        print("== the app of the release", flush=True)
        if args.windows:
            temp_win = win("echo %TEMP%")
            folder_win = temp_win + f"\\{STEM}-app-release-test"
            folder = Path(to_wsl(folder_win))
            if folder.exists():
                shutil.rmtree(folder)
            shutil.copytree(root, folder)
            datadir_win = folder_win + "\\data"
            (folder / "data").mkdir()
            app_cmd = [str(folder / f"{STEM}-app.exe")] + node_args + [f"-datadir={datadir_win}", f"-webuiconfig={folder_win}\\app.json", "-webuiregisteruri=0"]
            cli_cmd = [str(folder / "daemon" / f"{STEM}-cli.exe"), "-regtest", f"-rpcport={args.port + 3}", "-rpcuser=rel", "-rpcpassword=release-test"]
            env = None
            datadir = folder / "data"
        else:
            datadir = base / "data"
            datadir.mkdir()
            env = dict(os.environ, HOME=str(base), XDG_CONFIG_HOME=str(base / "config"), XDG_DATA_HOME=str(base / "share"), NO_AT_BRIDGE="1")
            app_cmd = ["xvfb-run", "-a", "-s", "-screen 0 1280x800x24", str(app)] + node_args + [f"-datadir={datadir}", f"-webuiconfig={base / 'app.json'}"]
            cli_cmd = [str(bindir / f"{STEM}-cli"), "-regtest", f"-rpcport={args.port + 3}", "-rpcuser=rel", "-rpcpassword=release-test"]
        log = open(base / "app.log", "w")
        proc = subprocess.Popen(app_cmd, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=not args.windows,
                                cwd="/mnt/c" if args.windows else None)

        def cli(*params, timeout=60):
            r = subprocess.run(cli_cmd + list(params), capture_output=True, text=True, timeout=timeout, cwd="/mnt/c" if args.windows else None)
            if r.returncode != 0:
                raise RuntimeError(r.stderr.strip() or r.stdout.strip())
            return r.stdout.strip()

        info = None
        until = time.time() + 240
        while time.time() < until and proc.poll() is None:
            try:
                info = json.loads(cli("getblockchaininfo", timeout=20))
                break
            except Exception:  # noqa: BLE001 (not up yet)
                time.sleep(1)
        check(failures, "the app of the release starts its node, which answers its command line program", bool(info) and info.get("chain") == "regtest", info or proc.poll())
        if info:
            commands = cli("help")
            check(failures, "the node of the app has the RPC methods of the chain", ("getmainchaininfo" in commands) == SIDECHAIN, commands[:120])
            if SIDECHAIN and args.mainchain_bin:
                connected = None
                until = time.time() + 120
                while time.time() < until:
                    connected = json.loads(cli("getmainchaininfo"))
                    if connected.get("connected"):
                        break
                    time.sleep(1)
                check(failures, "it follows the live mainchain, in its slot", bool(connected) and connected.get("connected") and connected.get("slot") == SLOT, connected)
            cli("stop")
            try:
                code = proc.wait(120)
            except subprocess.TimeoutExpired:
                code = None
            check(failures, "it stops when asked, and the app ends with exit 0", code == 0, code)
            check(failures, "its data directory has the files of a node that ran", (datadir / "regtest" / "blocks").exists())
    finally:
        if proc and proc.poll() is None:
            if args.windows:
                stop_ours_windows(folder_win)
            else:
                try:
                    os.killpg(proc.pid, 15)
                except ProcessLookupError:
                    pass
            proc.wait(30)
        if args.windows and folder_win:
            stop_ours_windows(folder_win)
            time.sleep(2)
            shutil.rmtree(Path(to_wsl(folder_win)), ignore_errors=True)
            print(f"  removed {folder_win}", flush=True)
        if main_proc:
            try:
                Rpc(args.port)("stop")
            except Exception:  # noqa: BLE001
                pass
            try:
                main_proc.wait(60)
            except subprocess.TimeoutExpired:
                main_proc.kill()

    if failures:
        print(f"FAILED: {failures}")
        return 1
    print("release: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
