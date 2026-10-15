#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end test of the node and Chains pages of chains-app.

Starts chains-app on regtest with a temporary data directory, in a virtual display, on a
private session bus where a pretend desktop (fake_desktop.py) has a tray and a notification
service, next to a second node (chainsd) to be its peer. The page runs node.js: a sidechain
proposed, acked and activated, deposits, a withdrawal bundle voted on, the block explorer, solo
mining, a stamped file, a proof of funds, a multisig spend, the crypto tools, the console, peers
banned and unbanned, the settings, the tray and the notifications, the sidechain nodes (with
pretend programs); pictures of every page in light and dark. At the end it resets the options,
which shuts the app down: the backup of the settings is checked here.

    run_node.py --app build/bin/chains-app --tmp <dir> --shots <dir>

Nothing outside --tmp is touched: HOME, the session bus, the data directories are in it, and the
nodes use ports of their own. Only the processes started here are stopped.
"""

import argparse
import base64
import json
import os
import shutil
import signal
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_smoke import App, check_shutdown, environment, ports_taken, script, short_tmp  # noqa: E402

HERE = Path(__file__).resolve().parent

FAKE_NODE = """#!/bin/sh
# A pretend sidechain node: it says how it was started, and runs until its client stops it.
dir=$(dirname "$0")
echo "$@" > "$dir/../fakechain-args.txt"
touch "$dir/../fakechain-running"
for a in "$@"; do
  case "$a" in -datadir=*) data="${a#-datadir=}";; esac
done
mkdir -p "$data/regtest"
printf 'first line, cut\\n2026-10-14T00:00:00Z fake log line\\n' > "$data/regtest/debug.log"
"""

FAKE_CLI = """#!/bin/sh
# The client of the pretend sidechain node.
dir=$(dirname "$0")
cmd=""
for a in "$@"; do
  case "$a" in -*) ;; *) if [ -z "$cmd" ]; then cmd="$a"; else last="$a"; fi ;; esac
done
if [ ! -f "$dir/../fakechain-running" ]; then
  echo "error: Could not connect to the server 127.0.0.1:18443" >&2
  exit 1
fi
case "$cmd" in
  getblockcount) echo 7 ;;
  getmainchaininfo) echo '{"connected": true, "height": 140}' ;;
  getbmminfo) if [ -f "$dir/../fakechain-mining" ]; then echo '{"mining": true, "idle": false, "blocks": 1}'; else echo '{"mining": false}'; fi ;;
  listwallets) echo '["main"]' ;;
  getnewaddress) echo sideaddress ;;
  setbmm) if [ "$last" = "sideaddress" ]; then touch "$dir/../fakechain-mining"; else rm -f "$dir/../fakechain-mining"; fi; echo null ;;
  requestbmmblock) echo "{\\"transactions\\": 2, \\"amount\\": $last}" ;;
  stop) rm -f "$dir/../fakechain-running"; echo "Fake stopping" ;;
  *) echo "error: unknown command $cmd" >&2; exit 1 ;;
esac
"""


def rpc(datadir, port, method, params=None):
    cookie = (datadir / "regtest" / ".cookie").read_text().strip()
    request = urllib.request.Request(f"http://127.0.0.1:{port}/", json.dumps({"jsonrpc": "1.0", "id": 1, "method": method, "params": params or []}).encode(),
                                     {"Authorization": "Basic " + base64.b64encode(cookie.encode()).decode()})
    return json.load(urllib.request.urlopen(request, timeout=30))["result"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--tmp", required=True, type=Path, help="folder for the data directories (made anew)")
    parser.add_argument("--shots", required=True, type=Path, help="folder for the pictures")
    parser.add_argument("--port", type=int, default=29490)
    args = parser.parse_args()
    # Stopped from outside (timeout, kill): the cleanup below still runs, and stops what was started here.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(124))

    for program in ["xvfb-run", "dbus-daemon"]:
        if shutil.which(program) is None:
            print(f"{program} is needed", file=sys.stderr)
            return 2
    chainsd = args.app.resolve().parent / "chainsd"
    if not chainsd.is_file():
        print(f"no chainsd next to {args.app}", file=sys.stderr)
        return 2
    peer_port = args.port + 10
    taken = ports_taken([args.port, args.port + 1, peer_port, peer_port + 1])
    if taken:
        print(f"ports {taken} are taken (a node left from an earlier run?): stop it, or give another --port", file=sys.stderr)
        return 2
    base = args.tmp.resolve()
    if base.exists():
        shutil.rmtree(base)
    base.mkdir(parents=True)
    args.shots.mkdir(parents=True, exist_ok=True)
    env = environment(base)
    files = base / "files"
    files.mkdir()
    (files / "stamp-me.txt").write_text("chains")
    bindir = files / "bin"
    bindir.mkdir()
    for name, body in [("fakechaind", FAKE_NODE), ("fakechain-cli", FAKE_CLI)]:
        (bindir / name).write_text(body)
        (bindir / name).chmod(0o755)
    datadir = base / "datadir"
    datadir.mkdir()
    failures = []
    started = []

    # A private session bus, with a pretend desktop on it: a tray and a notification service.
    # The socket of the bus in a short folder: the path of a socket is short (108 bytes).
    bus = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address=1", f"--address=unix:tmpdir={short_tmp()}"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=env)
    started.append(bus)
    env["DBUS_SESSION_BUS_ADDRESS"] = bus.stdout.readline().strip()
    desktop_out = base / "desktop.json"
    desktop = subprocess.Popen([sys.executable, str(HERE / "fake_desktop.py"), "--out", str(desktop_out), "--click", "Node window"],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    started.append(desktop)
    if "ready" not in desktop.stdout.readline():
        failures.append("the pretend desktop did not start")

    # A second node, to be a peer.
    peer_dir = base / "peer"
    peer_dir.mkdir()
    app = None
    try:
        subprocess.run([str(chainsd), "-regtest", f"-datadir={peer_dir}", f"-port={peer_port}", f"-rpcport={peer_port + 1}", "-bind=127.0.0.1",
                        "-dnsseed=0", "-fixedseeds=0", "-daemonwait"], check=True, env=env, stdout=subprocess.DEVNULL, timeout=120)
        print("== node", flush=True)
        page = base / "node.js"
        page.write_text(script("node.js", f"window.__E2E_FILES = {json.dumps(str(files))};\nwindow.__E2E_BIN = {json.dumps(str(bindir))};\n"))
        cmd = ["-regtest", f"-port={args.port}", f"-rpcport={args.port + 1}", "-listen=0", "-dnsseed=0", "-fixedseeds=0", "-server",
               f"-addnode=127.0.0.1:{peer_port}", "-fallbackfee=0.0002", f"-datadir={datadir}",
               f"-webuiconfig={base / 'chains-app.json'}", f"-webuitestshots={args.shots.resolve()}", f"-webuitestscript={page}"]
        app = App(args.app, cmd, env, base / "node.log")
        code = app.wait(1800)
        result = app.result()
        if code != 0 or not result or result["failures"]:
            failures.append(f"node run: exit {code}, {result['failures'] if result else 'no result'}; the page last said: {app.last}")
        # The app may stop before the page can say it asked for the reset: what the reset did is checked below.
        if "failed" in app.events.get("RESET", ""):
            failures.append(f"no reset of the options: {app.events.get('RESET')}")
        check_shutdown(datadir, failures)

        # The reset of the options: settings.json backed up and emptied, the settings of the app too.
        if not (datadir / "regtest" / "settings.json.bak").is_file():
            failures.append("no backup of settings.json after the reset")
        elif "dbcache" not in (datadir / "regtest" / "settings.json.bak").read_text():
            failures.append("the backup of settings.json has not the dbcache that was set")
        settings = json.loads((datadir / "regtest" / "settings.json").read_text()) if (datadir / "regtest" / "settings.json").is_file() else {}
        if "dbcache" in settings:
            failures.append(f"settings.json kept the dbcache after the reset: {settings}")
        if not (datadir / "regtest" / "guisettings.json.bak").is_file():
            failures.append("no backup of the settings of the app after the reset")
        config = json.loads((base / "chains-app.json").read_text())
        if not config.get("reset"):
            failures.append(f"the intro is not asked for after the reset: {config}")
        if config.get("prefs", {}).get("regtest"):
            failures.append("the preferences survived the reset")

        # The pretend sidechain node was started pointed at this node.
        started_args = (files / "fakechain-args.txt").read_text() if (files / "fakechain-args.txt").is_file() else ""
        for expected in ["-regtest -sidechainslot=5", f"-mainchainrpcport={args.port + 1}", "-mainchainrpccookiefile=", "-mainchainrpcwallet=miner", "-daemon"]:
            if expected not in started_args:
                failures.append(f"the sidechain node was not started with {expected}: {started_args!r}")

        # The desktop saw the tray icon with its menu, the click, and the notifications.
        desktop_record = json.loads(desktop_out.read_text()) if desktop_out.is_file() else {}
        labels = [item["label"] for item in desktop_record.get("menu", []) if item.get("type") != "separator"]
        if not desktop_record.get("items"):
            failures.append("no tray icon registered with the desktop")
        if labels != ["Show / Hide", "Send", "Receive", "Sign message…", "Verify message…", "Options…", "Node window", "Quit"]:
            failures.append(f"the menu of the tray: {labels}")
        props = desktop_record.get("properties", {})
        if props.get("Id") != "chains-app" or props.get("Status") != "Active" or not props.get("IconPixmapSizes") or "[regtest]" not in props.get("Title", ""):
            failures.append(f"the tray icon: {props}")
        if desktop_record.get("clicked") != "Node window":
            failures.append(f"the menu of the tray was not clicked: {desktop_record.get('errors')}")
        notes = desktop_record.get("notifications", [])
        if not any("transaction" in n["summary"] and "Amount:" in n["body"] and n["image"] for n in notes):
            failures.append(f"no notification of a transaction on the desktop: {notes}")

        if result:
            missing = [s for s in result["shots"] if not (args.shots / f"{s}.png").is_file()]
            if missing:
                failures.append(f"missing pictures: {missing}")
            print(f"  {result['passed']} checks passed, {len(result['shots'])} pictures in {args.shots}", flush=True)
    finally:
        # Even when the runner is interrupted, or a check above throws: nothing started here is left running.
        if app is not None:
            app.stop()
        try:
            rpc(peer_dir, peer_port + 1, "stop")
        except Exception as e:  # noqa: BLE001
            print(f"  the peer did not stop by RPC: {e}", flush=True)
        for process in reversed(started):
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
        for _ in range(100):
            if not (peer_dir / "regtest" / ".lock").exists() or not (peer_dir / "regtest" / ".cookie").exists():
                break
            time.sleep(0.2)

    if failures:
        print("FAILED:\n  " + "\n  ".join(failures), flush=True)
        return 1
    print("OK", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
