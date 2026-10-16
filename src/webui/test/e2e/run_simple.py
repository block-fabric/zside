#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end test of Simple mode of chains-app (the default of a first start).

Starts chains-app on regtest with a temporary data directory, in a virtual
display, and has it run simple.js in its page, in Simple mode (the other
scripts run in Advanced, common.js): Home with its first steps, the sidebar,
Send, the history, Move coins through its four steps to a real deposit, a word
of the glossary, in the Midnight and Light themes; then the switch to Advanced
and back.

    run_simple.py --app build/bin/chains-app --tmp <dir> --shots <dir>
"""

import argparse
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
    parser.add_argument("--port", type=int, default=41210)
    parser.add_argument("--script", default="simple.js", help="the page script, run in Simple mode")
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

    print(f"== {args.script}", flush=True)
    page_script = base / args.script
    page_script.write_text(script(args.script, "window.__E2E_SIMPLE = true;\n"))
    cmd = ["-regtest", f"-port={args.port}", f"-rpcport={args.port + 1}", "-listen=0", "-dnsseed=0", "-fixedseeds=0",
           "-fallbackfee=0.0002", f"-datadir={datadir}",
           f"-webuiconfig={base / 'chains-app.json'}", f"-webuitestshots={args.shots.resolve()}", f"-webuitestscript={page_script}"]
    app = App(args.app, cmd, env, base / "simple.log")
    code = app.wait(1200)
    result = app.result()
    if code != 0 or not result or result["failures"]:
        failures.append(f"simple run: exit {code}, {result['failures'] if result else 'no result'}")
    check_shutdown(datadir, failures)
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
