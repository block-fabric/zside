#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""How fast chains-app answers, on a wallet of 20,000 records (doc/webui-layouts.md, "Performance").

Starts chains-app on regtest in a virtual display with software GL (as the demo under WSLg), twice on
one data directory, with perf.js in its page: first to make the wallet "big" (40 transactions of 500
outputs); then again, measured: the start (first paint; the home drawn with the balance), each page
opened, typing in the search of the history, scrolling it, a new block. Prints the numbers, writes them
to <shots>/perf.json, and fails when one is over its limit (a regression).

    run_perf.py --app build/bin/chains-app --tmp <dir> --shots <dir>
"""

import argparse
import json
import os
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_smoke import App, check_shutdown, environment, script  # noqa: E402

# Limits (ms; calls): well above what the app takes on the machine of the tests (doc/webui-layouts.md,
# "Performance"), so that a regression shows and a busy machine does not.
LIMITS = {
    # The start (start.*) is reported, not limited: it is mostly the node's (the chain and the wallet
    # loaded) and the machine's load (7 to 27 s on a busy machine for the same data).
    "page.median": 120,
    "page.max": 500,
    "theme.switchMax": 400,
    "search.keyMax": 150,
    "search.filtered": 2500,
    "scroll.frameMedian": 50,
    "scroll.homeFrameMedian": 50,
    "block.worstFrame": 400,
    # A page that asks the node every 2 s asks nothing while the window is hidden (what is left: the
    # app's own calls, not the page's).
    "hidden.calls10s.mine": 4,
    "hidden.calls10s.node": 4,
}


def cpu_ms(root):
    """The processor time (user and system, ms) of a process and all its descendants."""
    tick = os.sysconf("SC_CLK_TCK")
    children = {}
    stats = {}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            fields = (entry / "stat").read_text().rsplit(")", 1)[1].split()
        except OSError:
            continue
        pid = int(entry.name)
        children.setdefault(int(fields[1]), []).append(pid)
        stats[pid] = int(fields[11]) + int(fields[12])
    total = 0
    todo = [root]
    while todo:
        pid = todo.pop()
        total += stats.get(pid, 0)
        todo += children.get(pid, [])
    return total * 1000 // tick


def follow(app, timeout, numbers):
    """Wait for the app, reading the marks "CPU start|end <name>" of the page as they come."""
    until = time.time() + timeout
    seen = 0
    started = {}
    while app.proc.poll() is None and time.time() < until:
        lines = app.lines[seen:]
        seen += len(lines)
        for line in lines:
            if line.startswith("webui test: CPU "):
                _, what, name = line[len("webui test: "):].split()
                if what == "start":
                    started[name] = cpu_ms(app.proc.pid)
                elif name in started:
                    numbers[name] = cpu_ms(app.proc.pid) - started.pop(name)
        time.sleep(0.05)
    return app.wait(max(1, until - time.time()))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--tmp", required=True, type=Path, help="folder for the data directory (made anew)")
    parser.add_argument("--shots", required=True, type=Path, help="folder for the numbers (perf.json)")
    parser.add_argument("--port", type=int, default=29530)
    parser.add_argument("--no-limits", action="store_true", help="measure only")
    parser.add_argument("--data", type=Path, help="a data directory made by an earlier run (<tmp>/datadir, copied): no setup")
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
    steps = ["setup", "measure"]
    if args.data:
        shutil.copytree(args.data / "datadir", datadir)
        shutil.copy(args.data / "chains-app.json", base / "chains-app.json")
        steps = ["measure"]
    else:
        datadir.mkdir()
    failures = []
    numbers = {}
    for step in steps:
        print(f"== {step}", flush=True)
        launched = int(time.time() * 1000)
        page_script = base / f"perf-{step}.js"
        page_script.write_text(script("perf.js", f"window.__E2E_STEP = {json.dumps(step)}; window.__E2E_LAUNCHED = {launched};\n"))
        cmd = ["-regtest", f"-port={args.port}", f"-rpcport={args.port + 1}", "-listen=0", "-dnsseed=0", "-fixedseeds=0",
               "-fallbackfee=0.0002", f"-datadir={datadir}", f"-webuiconfig={base / 'chains-app.json'}",
               f"-webuitestshots={args.shots.resolve()}", f"-webuitestscript={page_script}"]
        app = App(args.app, cmd, env, base / f"{step}.log")
        code = follow(app, 1500, numbers)
        result = app.result()
        if result is None:
            failures.append(f"{step}: no result (exit code {code})")
        else:
            failures += [f"{step}: {f}" for f in result["failures"]]
        if code != 0:
            failures.append(f"{step}: exit code {code}")
        check_shutdown(datadir, failures)
        for line in app.lines:
            if line.startswith("webui test: PERF "):
                name, value = line[len("webui test: PERF "):].split()
                numbers[name] = int(value)
    (args.shots / "perf.json").write_text(json.dumps(numbers, indent=2) + "\n")
    print("== numbers (ms; scroll.fps in frames per second)")
    for name, value in numbers.items():
        limit = LIMITS.get(name)
        over = limit is not None and value > limit
        print(f"  {name:28} {value:7}{f'   (limit {limit})' if limit else ''}{'   OVER' if over else ''}")
        if over and not args.no_limits:
            failures.append(f"{name}: {value} ms, over {limit} ms")
    missing = [name for name in LIMITS if name not in numbers]
    if missing:
        failures.append(f"not measured: {missing}")
    if failures:
        print("FAILED:\n  " + "\n  ".join(failures), flush=True)
        return 1
    print("OK", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
