#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end test of the wallet pages of chains-app.

Starts chains-app on regtest with a temporary data directory, in a virtual
display, and has it run wallet.js in its page: wallets created (one encrypted),
a payment request, coins mined, a payment sent with the passphrase through the
confirmation, the history with details and a fee bump, a PSBT created, saved,
loaded from the file, signed and broadcast, encryption, unlocking for a while,
a new passphrase, messages signed and verified, the receiving addresses (a tab
of Receive), backups and a restore, choosing the coins in the Send page; with pictures of every wallet page in the light and
dark themes. The file choosers are answered by the test (test.fileAnswer).

    run_wallet.py --app build/bin/chains-app --tmp <dir> --shots <dir>
"""

import argparse
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_smoke import App, check_shutdown, environment, script  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--tmp", required=True, type=Path, help="folder for the data directory (made anew)")
    parser.add_argument("--shots", required=True, type=Path, help="folder for the pictures")
    parser.add_argument("--port", type=int, default=29480)
    args = parser.parse_args()

    if shutil.which("xvfb-run") is None:
        print("xvfb-run is needed", file=sys.stderr)
        return 2
    base = args.tmp.resolve()
    if base.exists():
        shutil.rmtree(base)
    base.mkdir(parents=True)
    args.shots.mkdir(parents=True, exist_ok=True)
    env = environment(base)
    files = base / "files"
    files.mkdir()
    datadir = base / "datadir"
    datadir.mkdir()
    failures = []

    print("== wallet", flush=True)
    wallet_script = base / "wallet.js"
    wallet_script.write_text(script("wallet.js", f"window.__E2E_FILES = {json.dumps(str(files))};\n"))
    cmd = ["-regtest", f"-port={args.port}", f"-rpcport={args.port + 1}", "-listen=0", "-dnsseed=0", "-fixedseeds=0",
           "-fallbackfee=0.0002", f"-datadir={datadir}",
           f"-webuiconfig={base / 'chains-app.json'}", f"-webuitestshots={args.shots.resolve()}", f"-webuitestscript={wallet_script}"]
    app = App(args.app, cmd, env, base / "wallet.log")
    code = app.wait(1200)
    result = app.result()
    if code != 0 or not result or result["failures"]:
        failures.append(f"wallet run: exit {code}, {result['failures'] if result else 'no result'}")
    check_shutdown(datadir, failures)

    # What the page saved through the file choosers.
    for name in ["request.png", "transactions.csv", "payment.psbt", "receiving.csv", "alice-backup.dat", "alice-watchonly.dat"]:
        if not (files / name).is_file() or (files / name).stat().st_size == 0:
            failures.append(f"no file {name}")
    if (files / "request.png").is_file() and not (files / "request.png").read_bytes().startswith(b"\x89PNG"):
        failures.append("request.png is not a PNG picture")
    if (files / "payment.psbt").is_file() and not (files / "payment.psbt").read_bytes().startswith(b"psbt\xff"):
        failures.append("payment.psbt is not a binary PSBT")
    csv = (files / "transactions.csv").read_text() if (files / "transactions.csv").is_file() else ""
    if not csv.startswith('"Confirmed","Date","Type","Label","Address","Amount (CHN)","ID"\n') or '"Sent to","Bob"' not in csv:
        failures.append(f"transactions.csv: {csv[:200]!r}")
    if (files / "receiving.csv").is_file() and '"Rainy day"' not in (files / "receiving.csv").read_text():
        failures.append("receiving.csv has no Rainy day")

    if result:
        missing = [s for s in result["shots"] if not (args.shots / f"{s}.png").is_file()]
        if missing:
            failures.append(f"missing pictures: {missing}")
        print(f"  {result['passed']} checks passed, {len(result['shots'])} pictures in {args.shots}", flush=True)
    if failures:
        print("FAILED:\n  " + "\n  ".join(failures), flush=True)
        return 1
    print("OK", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
