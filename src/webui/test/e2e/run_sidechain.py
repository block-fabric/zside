#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end test of the app of a sidechain against a mainchain.

Starts a regtest node of the mainchain (chainsd, from MAINCHAIN_BIN_DIR or --mainchain-bin), activates
this sidechain in its slot there, mines a block of the mainchain every second (a live mainchain), and
starts the app of the sidechain on regtest in a virtual display, following that node with the user
name and a password file. Its page runs sidechain.js, which does what a user does: a deposit address,
a deposit by the helper of the mainchain wallet, merged mining turned on (blocks of this chain mined
by the mainchain), the deposit credited, a withdrawal to an address of the mainchain wallet, the
bundle voted on by the miners of the mainchain and paid; with pictures of every page of the sidechain
on the way. The test then checks on the mainchain that the withdrawal was paid.

    run_sidechain.py --app build/bin/<stem>-app --tmp <dir> --shots <dir> [--mainchain-bin <dir>]

A sidechain made from the template runs its own page script the same way: --script <its .js, in this
folder>, with --app-arg for options of the app and --js-var NAME=VALUE for values the script reads
(window.__E2E_<NAME>); window.__E2E_DIR is the folder of the run (for files the script chooses). --file
NAME=TEXT writes a file in the folder of the run before the app starts (a validator's key file, say), and
{tmp} in an --app-arg is that folder; --width starts the window that wide (700 for the narrow pictures).
Only sidechain.js makes a withdrawal, checked here on the mainchain.

Nothing outside --tmp is touched; the nodes use ports of their own (--port and the three after it).
"""

import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from decimal import Decimal
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_smoke import App, check_shutdown, environment, script  # noqa: E402

SLOT = 5
# Regtest parameters of the mainchain (CRegTestParams there).
ACTIVATION_PERIOD = 20
USER, PASSWORD = "main", "e2e-secret"


class Rpc:
    """JSON-RPC to the mainchain node."""

    def __init__(self, port, wallet=None):
        self.url = f"http://127.0.0.1:{port}/" + (f"wallet/{wallet}" if wallet else "")
        self.auth = "Basic " + base64.b64encode(f"{USER}:{PASSWORD}".encode()).decode()

    def __call__(self, method, *params):
        body = json.dumps({"jsonrpc": "1.0", "id": method, "method": method, "params": list(params)}).encode()
        request = urllib.request.Request(self.url, data=body, headers={"Authorization": self.auth, "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                reply = json.loads(response.read(), parse_float=Decimal)
        except urllib.error.HTTPError as e:
            reply = json.loads(e.read(), parse_float=Decimal)
        if reply.get("error"):
            raise RuntimeError(f"{method}: {reply['error']}")
        return reply["result"]


def start_mainchain(binary, base, port):
    datadir = base / "mainchain"
    datadir.mkdir()
    proc = subprocess.Popen(
        [str(binary), "-regtest", f"-datadir={datadir}", f"-rpcport={port}", f"-port={port + 1}", "-listen=0", "-server",
         f"-rpcuser={USER}", f"-rpcpassword={PASSWORD}", "-fallbackfee=0.0002", "-printtoconsole=0", "-dnsseed=0", "-fixedseeds=0"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    node = Rpc(port)
    for _ in range(240):
        try:
            node("getblockcount")
            return proc, node
        except (OSError, RuntimeError, urllib.error.URLError):
            if proc.poll() is not None:
                raise RuntimeError("the mainchain node stopped")
            time.sleep(0.25)
    raise RuntimeError("the mainchain node did not start")


class Miner(threading.Thread):
    """A block of the mainchain every `interval` seconds, until stopped."""

    def __init__(self, wallet, address, interval):
        super().__init__(daemon=True)
        self.wallet, self.address, self.interval = wallet, address, interval
        self.stop = threading.Event()
        self.blocks = 0
        self.error = None

    def run(self):
        while not self.stop.wait(self.interval):
            try:
                self.wallet("generatetoaddress", 1, self.address)
                self.blocks += 1
            except Exception as e:  # noqa: BLE001 (keep mining: the node may be busy)
                self.error = str(e)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--tmp", required=True, type=Path, help="folder for the data directories (made anew)")
    parser.add_argument("--shots", required=True, type=Path, help="folder for the pictures")
    parser.add_argument("--mainchain-bin", type=Path, default=os.environ.get("MAINCHAIN_BIN_DIR"), help="folder of chainsd (default: $MAINCHAIN_BIN_DIR)")
    parser.add_argument("--port", type=int, default=29640)
    parser.add_argument("--interval", type=float, default=1.0, help="seconds between two blocks of the mainchain")
    parser.add_argument("--script", default="sidechain.js", help="the page script, in this folder (default: sidechain.js)")
    parser.add_argument("--app-arg", action="append", default=[], help="an option given to the app (repeat for more)")
    parser.add_argument("--js-var", action="append", default=[], help="NAME=VALUE: window.__E2E_<NAME> = VALUE (JSON) for the page script")
    parser.add_argument("--file", action="append", default=[], help="NAME=TEXT: a file of the folder of the run, written before the app starts (\\n in TEXT a new line; {tmp} in --app-arg is that folder)")
    parser.add_argument("--width", type=int, default=0, help="the width of the window, in pixels (default: the app's own, 1180)")
    args = parser.parse_args()

    if shutil.which("xvfb-run") is None:
        print("xvfb-run is needed", file=sys.stderr)
        return 2
    chainsd = (args.mainchain_bin / "chainsd") if args.mainchain_bin else None
    if not chainsd or not chainsd.is_file():
        print(f"no mainchain node: set MAINCHAIN_BIN_DIR or --mainchain-bin (looked for {chainsd})", file=sys.stderr)
        return 2
    base = args.tmp.resolve()
    if base.exists():
        shutil.rmtree(base)
    base.mkdir(parents=True)
    args.shots.mkdir(parents=True, exist_ok=True)
    env = environment(base)
    failures = []

    print("== the mainchain", flush=True)
    main_proc, main = start_mainchain(chainsd, base, args.port)
    miner = None
    try:
        main("createwallet", "miner")
        wallet = Rpc(args.port, "miner")
        coinbase = wallet("getnewaddress")
        wallet("generatetoaddress", 110, coinbase)
        main("createsidechainproposal", SLOT, "E2E sidechain", "The sidechain of the end-to-end test", "11" * 32, "22" * 20)
        wallet("generatetoaddress", ACTIVATION_PERIOD + 1, coinbase)
        if [s["slot"] for s in main("listactivesidechains")] != [SLOT]:
            raise RuntimeError("the sidechain did not activate")
        print(f"  slot {SLOT} active at block {main('getblockcount')}", flush=True)
        # A live mainchain: a block every second, which the miner of the sidechain's blocks commits in.
        miner = Miner(wallet, coinbase, args.interval)
        miner.start()

        print("== the app of the sidechain", flush=True)
        password_file = base / "mainchain.password"
        password_file.write_text(PASSWORD + "\n")
        password_file.chmod(0o600)
        datadir = base / "datadir"
        datadir.mkdir()
        page_script = base / args.script
        chain = json.loads((Path(__file__).resolve().parents[2] / "app/src/chain/chain.json").read_text())
        prelude = f"window.__E2E_SLOT = {SLOT};\nwindow.__E2E_MAINCHAIN = {json.dumps(chain['mainchain']['name'])};\n" + f"window.__E2E_DIR = {json.dumps(str(base))};\n"
        for var in args.js_var:
            name, value = var.split("=", 1)
            prelude += f"window.__E2E_{name} = {value};\n"
        page_script.write_text(script(args.script, prelude))
        for item in args.file:
            name, text = item.split("=", 1)
            if "/" in name or name.startswith("."):
                raise RuntimeError(f"--file {name}: a name in the folder of the run")
            (base / name).write_text(text.replace("\\n", "\n") + "\n")
            (base / name).chmod(0o600)
        if args.width:
            (base / "app.json").write_text(json.dumps({"window": {"width": args.width, "height": 860, "maximized": False}}))
        cmd = ["-regtest", f"-sidechainslot={SLOT}", f"-port={args.port + 2}", f"-rpcport={args.port + 3}", "-listen=0", "-dnsseed=0", "-fixedseeds=0",
               "-fallbackfee=0.0002", f"-datadir={datadir}", f"-mainchainrpcport={args.port}", f"-mainchainrpcuser={USER}",
               f"-mainchainrpcpasswordfile={password_file}", "-mainchainrpcwallet=miner",
               f"-webuiconfig={base / 'app.json'}", f"-webuitestshots={args.shots.resolve()}", f"-webuitestscript={page_script}"] + [a.replace("{tmp}", str(base)) for a in args.app_arg]
        app = App(args.app, cmd, env, base / "sidechain.log")
        code = app.wait(1800)
        result = app.result()
        if code != 0 or not result or result["failures"]:
            failures.append(f"sidechain run: exit {code}, {result['failures'] if result else 'no result'}")
        check_shutdown(datadir, failures)

        # The withdrawal, paid on the mainchain to the address of its wallet the page chose.
        paid_to = app.events.get("WITHDRAWN")
        if args.script != "sidechain.js":
            pass
        elif not paid_to:
            failures.append("the page made no withdrawal")
        else:
            address, amount = paid_to.split(" ")[1:3]
            deadline = time.time() + 120
            received = Decimal(0)
            while time.time() < deadline:
                received = wallet("getreceivedbyaddress", address, 1)
                if received >= Decimal(amount):
                    break
                time.sleep(1)
            print(f"  {address} received {received} on the mainchain", flush=True)
            if received != Decimal(amount):
                failures.append(f"the mainchain paid {received} to {address}, not {amount}")
        if miner.error:
            print(f"  (the miner of the mainchain once said: {miner.error})", flush=True)
        if result:
            missing = [s for s in result["shots"] if not (args.shots / f"{s}.png").is_file()]
            if missing:
                failures.append(f"missing pictures: {missing}")
            print(f"  {result['passed']} checks passed, {len(result['shots'])} pictures in {args.shots}", flush=True)
    except Exception as e:  # noqa: BLE001
        failures.append(f"error: {e}")
    finally:
        if miner:
            miner.stop.set()
            miner.join(timeout=30)
        try:
            main("stop")
            main_proc.wait(timeout=120)
        except Exception:  # noqa: BLE001
            main_proc.kill()
            main_proc.wait(timeout=30)

    if failures:
        print("FAILED:\n  " + "\n  ".join(failures), flush=True)
        return 1
    print("OK", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
