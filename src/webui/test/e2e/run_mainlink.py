#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end test of the alert of a sidechain's app when its node cannot reach the mainchain node.

Starts a regtest node of the mainchain (chainsd, from MAINCHAIN_BIN_DIR or --mainchain-bin) with this
sidechain active in its slot and a block every second, and the app of the sidechain following it. Its page
runs mainlink.js, which says when to break the mainchain node; this runner does it:

    STOP_MAIN    stop the mainchain node             (the down banner, the status bar, the bell)
    START_MAIN   start it again                      (the banner goes, the toast of the link back)
    SYNC_MAIN    restart it in initial block download (-maxtipage=1, no new blocks): the syncing banner
    NORMAL_MAIN  restart it as before, blocks again  (the banner goes)

    run_mainlink.py --app build/bin/<stem>-app --tmp <dir> --shots <dir> [--mainchain-bin <dir>] [--port N]

The nodes use --port and the three after it. Nothing outside --tmp is touched.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sidechain import ACTIVATION_PERIOD, PASSWORD, SLOT, USER, Miner, Rpc  # noqa: E402
from run_smoke import App, check_shutdown, environment, ports_taken, script  # noqa: E402


class Mainchain:
    """The mainchain node, stopped and started again on the same data directory."""

    def __init__(self, binary, base, port):
        self.binary, self.datadir, self.port = binary, base / "mainchain", port
        self.datadir.mkdir()
        self.proc = None
        self.rpc = Rpc(port)

    def start(self, *extra):
        self.proc = subprocess.Popen(
            [str(self.binary), "-regtest", f"-datadir={self.datadir}", f"-rpcport={self.port}", f"-port={self.port + 1}", "-listen=0", "-server",
             f"-rpcuser={USER}", f"-rpcpassword={PASSWORD}", "-fallbackfee=0.0002", "-printtoconsole=0", "-dnsseed=0", "-fixedseeds=0", *extra],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(240):
            try:
                self.rpc("getblockcount")
                return
            except Exception:  # noqa: BLE001 (starting)
                if self.proc.poll() is not None:
                    raise RuntimeError("the mainchain node stopped")
                time.sleep(0.25)
        raise RuntimeError("the mainchain node did not start")

    def stop(self):
        if self.proc is None or self.proc.poll() is not None:
            return
        try:
            self.rpc("stop")
            self.proc.wait(timeout=120)
        except Exception:  # noqa: BLE001
            self.proc.kill()
            self.proc.wait(timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--tmp", required=True, type=Path, help="folder for the data directories (made anew)")
    parser.add_argument("--shots", required=True, type=Path, help="folder for the pictures")
    parser.add_argument("--mainchain-bin", type=Path, default=os.environ.get("MAINCHAIN_BIN_DIR"), help="folder of chainsd (default: $MAINCHAIN_BIN_DIR)")
    parser.add_argument("--port", type=int, default=29680)
    args = parser.parse_args()

    if shutil.which("xvfb-run") is None:
        print("xvfb-run is needed", file=sys.stderr)
        return 2
    chainsd = (args.mainchain_bin / "chainsd") if args.mainchain_bin else None
    if not chainsd or not chainsd.is_file():
        print(f"no mainchain node: set MAINCHAIN_BIN_DIR or --mainchain-bin (looked for {chainsd})", file=sys.stderr)
        return 2
    taken = ports_taken(range(args.port, args.port + 4))
    if taken:
        print(f"ports taken: {taken}", file=sys.stderr)
        return 2
    base = args.tmp.resolve()
    if base.exists():
        shutil.rmtree(base)
    base.mkdir(parents=True)
    args.shots.mkdir(parents=True, exist_ok=True)
    env = environment(base)
    failures = []

    print("== the mainchain", flush=True)
    main = Mainchain(chainsd, base, args.port)
    main.start()
    miner = None
    app = None
    try:
        main.rpc("createwallet", "miner")
        wallet = Rpc(args.port, "miner")
        coinbase = wallet("getnewaddress")
        wallet("generatetoaddress", 110, coinbase)
        main.rpc("createsidechainproposal", SLOT, "E2E sidechain", "The sidechain of the end-to-end test", "11" * 32, "22" * 20)
        wallet("generatetoaddress", ACTIVATION_PERIOD + 1, coinbase)
        miner = Miner(wallet, coinbase, 1.0)
        miner.start()

        print("== the app of the sidechain", flush=True)
        datadir = base / "datadir"
        datadir.mkdir()
        page_script = base / "mainlink.js"
        chain = json.loads((Path(__file__).resolve().parents[2] / "app/src/chain/chain.json").read_text())
        page_script.write_text(script("mainlink.js", f"window.__E2E_MAINCHAIN = {json.dumps(chain['mainchain']['name'])};\n"))
        password_file = base / "mainchain.password"
        password_file.write_text(PASSWORD + "\n")
        password_file.chmod(0o600)
        cmd = ["-regtest", f"-sidechainslot={SLOT}", f"-port={args.port + 2}", f"-rpcport={args.port + 3}", "-listen=0", "-dnsseed=0", "-fixedseeds=0",
               f"-datadir={datadir}", f"-mainchainrpcport={args.port}", f"-mainchainrpcuser={USER}", f"-mainchainrpcpasswordfile={password_file}", "-mainchainrpcwallet=miner",
               f"-webuiconfig={base / 'app.json'}", f"-webuitestshots={args.shots.resolve()}", f"-webuitestscript={page_script}"]
        app = App(args.app, cmd, env, base / "sidechain.log")

        def step(key, timeout=600):
            if app.wait_event(key, timeout) is None:
                raise RuntimeError(f"the page did not get to {key}")

        step("STOP_MAIN")
        miner.paused = True
        main.stop()
        print("  mainchain node stopped", flush=True)
        step("START_MAIN")
        main.start()
        main.rpc("loadwallet", "miner")
        miner.paused = False
        print("  mainchain node started", flush=True)
        step("SYNC_MAIN")
        miner.paused = True
        main.stop()
        # The tip older than a second: in initial block download until a new block, which nobody mines.
        time.sleep(2)
        main.start("-maxtipage=1")
        main.rpc("loadwallet", "miner")
        print("  mainchain node in initial block download", flush=True)
        step("NORMAL_MAIN")
        main.stop()
        main.start()
        main.rpc("loadwallet", "miner")
        miner.paused = False
        print("  mainchain node back to normal", flush=True)

        code = app.wait(900)
        result = app.result()
        if code != 0 or not result or result["failures"]:
            failures.append(f"run: exit {code}, {result['failures'] if result else 'no result'}")
        check_shutdown(datadir, failures)
        if result:
            missing = [s for s in result["shots"] if not (args.shots / f"{s}.png").is_file()]
            if missing:
                failures.append(f"missing pictures: {missing}")
            print(f"  {result['passed']} checks passed, {len(result['shots'])} pictures in {args.shots}", flush=True)
    except Exception as e:  # noqa: BLE001
        failures.append(f"error: {e}")
    finally:
        if app is not None:
            app.stop()
        if miner:
            miner.stop.set()
            miner.join(timeout=30)
        main.stop()

    if failures:
        print("FAILED:\n  " + "\n  ".join(failures), flush=True)
        return 1
    print("OK", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
