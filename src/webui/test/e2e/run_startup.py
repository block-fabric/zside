#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end test of the start of chains-app: the page leaves the splash.

Starts chains-app three times on one regtest data directory, in a virtual
display, with startup.js in its page: a new data directory; again, four times,
with no wallet to load and no settings of the app (the node is ready at once,
before the page has its language: the race that left the page on the splash);
again, with a wallet loaded at the start. Each time the page must be in the phase of the host and draw the
app (the dashboard's home), not the splash.

    run_startup.py --app build/bin/chains-app --tmp <dir> --shots <dir>
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
    parser.add_argument("--port", type=int, default=29510)
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
    # The start with no wallet is a race between the node and the page: four times, without the
    # settings of the app (as a first start of the app on a data directory in use).
    steps = ["first", "nowallet-1", "nowallet-2", "nowallet-3", "nowallet", "wallet"]
    for name in steps:
        step = name.split("-")[0]
        print(f"== {name}", flush=True)
        if step == "nowallet":
            (base / "chains-app.json").unlink(missing_ok=True)
        page_script = base / f"startup-{name}.js"
        page_script.write_text(script("startup.js", f"window.__E2E_STEP = {json.dumps(step)}; window.__E2E_LAST = {json.dumps(name == step)};\n"))
        cmd = ["-regtest", f"-port={args.port}", f"-rpcport={args.port + 1}", "-listen=0", "-dnsseed=0", "-fixedseeds=0",
               f"-datadir={datadir}", f"-webuiconfig={base / 'chains-app.json'}",
               f"-webuitestshots={args.shots.resolve()}", f"-webuitestscript={page_script}"]
        # The renderer of a desktop (not the software one of the other tests): the page takes as long
        # to get ready as on a desktop, and the race shows (it does not with the software renderer).
        desktop = {k: v for k, v in env.items() if k not in ("LIBGL_ALWAYS_SOFTWARE", "WEBKIT_DISABLE_DMABUF_RENDERER")}
        app = App(args.app, cmd, desktop if step == "nowallet" else env, base / f"{name}.log")
        code = app.wait(300)
        result = app.result()
        if result is None:
            failures.append(f"{name}: no result (exit code {code})")
        else:
            failures += [f"{name}: {f}" for f in result["failures"]]
        if code != 0:
            failures.append(f"{name}: exit code {code}")
        check_shutdown(datadir, failures)
    if failures:
        print("FAILED:")
        for f in failures:
            print("  " + f)
        return 1
    print("startup: every start reached the app")
    return 0


if __name__ == "__main__":
    sys.exit(main())
