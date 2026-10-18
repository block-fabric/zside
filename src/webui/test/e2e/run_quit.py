#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end test of the confirmation before a quit (quit.js, app.quitGuard).

Starts the app twice on one regtest data directory, in a virtual display: once ended with "Quit anyway", once
with a second close while the question shows. The app must stop each time, cleanly, with exit code 0.

    run_quit.py --app build/bin/chains-app --tmp <dir> --shots <dir>
        [--prepare <page script>]   run first, once (a chain's page then keeps work a quit would interrupt)
        [--expect <text>]           the text the chain's page gives (else the test gives its own)
"""

import argparse
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_smoke import App, check_shutdown, environment, script  # noqa: E402

MESSAGE = "Work is still under way (e2e). If you quit now it resumes the next time you open the app."


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--tmp", required=True, type=Path, help="folder for the data directory (made anew)")
    parser.add_argument("--shots", required=True, type=Path, help="folder for the pictures")
    parser.add_argument("--port", type=int, default=49610)
    parser.add_argument("--prepare", type=Path)
    parser.add_argument("--expect")
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
    datadir = base / "datadir"
    datadir.mkdir()
    failures = []
    common = ["-regtest", f"-port={args.port}", f"-rpcport={args.port + 1}", "-listen=0", "-dnsseed=0", "-fixedseeds=0",
              f"-datadir={datadir}", f"-webuiconfig={base / 'app.json'}", f"-webuitestshots={args.shots.resolve()}"]
    steps = (["prepare"] if args.prepare else []) + ["accept", "again"]
    for step in steps:
        print(f"== {step}", flush=True)
        page_script = base / f"quit-{step}.js"
        if step == "prepare":
            page_script.write_text(script(str(args.prepare.resolve())))
        else:
            prelude = f"window.__E2E_STEP = {json.dumps(step)}; window.__E2E_MESSAGE = {json.dumps(MESSAGE)};"
            if args.expect:
                prelude += f" window.__E2E_FROM_PAGE = true; window.__E2E_EXPECT = {json.dumps(args.expect)};"
            page_script.write_text(script("quit.js", prelude + "\n"))
        app = App(args.app, common + [f"-webuitestscript={page_script}"], env, base / f"{step}.log")
        code = app.wait(400)
        result = app.result()
        if result is None:
            failures.append(f"{step}: no result (exit code {code})")
        else:
            failures += [f"{step}: {f}" for f in result["failures"]]
        if code != 0:
            failures.append(f"{step}: exit code {code}")
        check_shutdown(datadir, failures)
    if failures:
        print("FAILED:")
        for f in failures:
            print("  " + f)
        return 1
    print("quit: asked before every quit, kept open, quit anyway, and a second close quits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
