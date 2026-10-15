#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end smoke test of chains-app.

Starts chains-app on regtest with a temporary data directory, in a virtual
display (xvfb-run), and has it run a test script in its page (smoke.js) that
drives the interface through the bridge: RPC calls, errors and cancellation,
a wallet and its balance, events, preferences, escaping, shortcuts, a payment
URI handed over by a second start of the app, and pictures of every page in the
light and dark themes (and the other themes for a few pages). A second run
covers the intro (the choice of the data directory) of a first start.

    run_smoke.py --app build/bin/chains-app --tmp <dir> --shots <dir>

Nothing outside --tmp is touched: HOME, XDG_RUNTIME_DIR and the app settings
file are in it, and the node uses ports of its own.
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def script(name, prelude=""):
    return prelude + (HERE / "common.js").read_text() + "\n" + (HERE / name).read_text()


class App:
    """chains-app in a virtual display, its output followed line by line."""

    #: The longest the page may say nothing (a step of a page script waits 2 minutes at most): past
    #: this, the app is stuck, and is stopped with the last thing the page said.
    QUIET = 300

    def __init__(self, app, args, env, log_path):
        self.lines = []
        self.events = {}
        self.last = "nothing yet (the page script did not start)"
        self.last_time = time.time()
        self.log = open(log_path, "w", encoding="utf-8")
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1400x900x24", str(app)] + args
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True, bufsize=1, start_new_session=True)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.proc.stdout:
            self.log.write(line)
            self.log.flush()
            line = line.rstrip("\n")
            self.lines.append(line)
            if line.startswith("webui test: "):
                message = line[len("webui test: "):]
                print("  " + message, flush=True)
                key = message.split(" ", 1)[0]
                self.events[key] = message
                self.last = message
                self.last_time = time.time()

    def wait_event(self, key, timeout):
        until = time.time() + timeout
        while time.time() < until:
            if key in self.events:
                return self.events[key]
            if self.proc.poll() is not None and not self.reader.is_alive():
                return None
            time.sleep(0.1)
        print(f"chains-app: no {key} in {timeout} s; the page last said: {self.last}", flush=True)
        return None

    def wait(self, timeout, quiet=None):
        """The exit code of the app, or None when it runs past timeout, or says nothing for quiet seconds."""
        quiet = self.QUIET if quiet is None else quiet
        until = time.time() + timeout
        while self.proc.poll() is None:
            now = time.time()
            if now > until:
                print(f"chains-app did not stop in {timeout} s; the page last said: {self.last}", flush=True)
                self.stop()
                return None
            if now - self.last_time > quiet:
                print(f"chains-app: the page said nothing for {quiet} s, stuck after: {self.last}", flush=True)
                self.stop()
                return None
            time.sleep(0.2)
        self.reader.join(timeout=10)
        return self.proc.returncode

    def stop(self):
        """Stop the app (and its virtual display) if it still runs."""
        if self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, 15)
                self.proc.wait(timeout=60)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(self.proc.pid, 9)
                except ProcessLookupError:
                    pass
                self.proc.wait()
        self.reader.join(timeout=10)

    def result(self):
        message = self.events.get("RESULT")
        return json.loads(message[len("RESULT "):]) if message else None


def ports_taken(ports):
    """The ports of the list that are taken on 127.0.0.1 (a node left over from an earlier run)."""
    taken = []
    for port in ports:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            if s.connect_ex(("127.0.0.1", port)) == 0:
                taken.append(port)
    return taken


def short_tmp():
    """The temporary folder of this user for the tests (/tmp/claude-<uid> when it exists)."""
    candidate = Path(f"/tmp/claude-{os.getuid()}")
    return candidate if candidate.is_dir() else Path(os.environ.get("TMPDIR", "/tmp"))


def run_group(cmd, env, timeout):
    """Run a command in its own process group; on timeout, end the whole group (xvfb-run, Xvfb, the app)."""
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, 15)
        out, err = proc.communicate(timeout=30)
        return None, out, err
    return proc.returncode, out, err


def environment(base):
    env = dict(os.environ)
    home = base / "home"
    runtime = base / "runtime"
    home.mkdir(parents=True, exist_ok=True)
    runtime.mkdir(parents=True, exist_ok=True)
    runtime.chmod(0o700)
    env["HOME"] = str(home)
    # The socket of the app goes in a private folder of the user under TMPDIR: the path of a socket
    # is short (108 bytes), so TMPDIR is the shortest folder of the tests that can be used.
    env.pop("XDG_RUNTIME_DIR", None)
    env["TMPDIR"] = str(short_tmp())
    env["XDG_CONFIG_HOME"] = str(home / ".config")
    # Software rendering in the virtual display.
    env["LIBGL_ALWAYS_SOFTWARE"] = "1"
    env["WEBKIT_DISABLE_DMABUF_RENDERER"] = "1"
    return env


def check_shutdown(datadir, failures):
    log = datadir / "regtest" / "debug.log"
    text = log.read_text(errors="replace") if log.exists() else ""
    if "Shutdown done" not in text or "webui: node stopped" not in text:
        failures.append(f"no clean shutdown in {log}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--tmp", required=True, type=Path, help="folder for the data directories (made anew)")
    parser.add_argument("--shots", required=True, type=Path, help="folder for the pictures")
    parser.add_argument("--port", type=int, default=29470)
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
    failures = []
    common = ["-regtest", f"-port={args.port}", f"-rpcport={args.port + 1}", "-listen=0", "-dnsseed=0", "-fixedseeds=0",
              f"-webuiconfig={base / 'chains-app.json'}", f"-webuitestshots={args.shots.resolve()}"]

    # 1. The first start: the intro chooses the data directory.
    print("== intro", flush=True)
    intro_dir = base / "intro-datadir"
    intro_script = base / "intro.js"
    intro_script.write_text(script("intro.js", f"window.__E2E_DATADIR = {json.dumps(str(intro_dir))};\n"))
    app = App(args.app, common + ["-choosedatadir", f"-webuitestscript={intro_script}"], env, base / "intro.log")
    code = app.wait(300)
    result = app.result()
    if code != 0 or not result or result["failures"]:
        failures.append(f"intro run: exit {code}, {result['failures'] if result else 'no result'}")
    if not (intro_dir / "regtest").is_dir():
        failures.append("the intro made no data directory")
    else:
        check_shutdown(intro_dir, failures)
    shots = list(result["shots"]) if result else []

    # 2. The smoke test.
    print("== smoke", flush=True)
    datadir = base / "datadir"
    datadir.mkdir()
    smoke_script = base / "smoke.js"
    smoke_script.write_text(script("smoke.js"))
    app = App(args.app, common + [f"-datadir={datadir}", f"-webuitestscript={smoke_script}"], env, base / "smoke.log")
    ready = app.wait_event("READY_FOR_URI", 300)
    if ready:
        address = ready.split(" ")[1]
        uri = f"chains:{address}?amount=1.5&label=Smoke%20test"
        code, out, err = run_group(["xvfb-run", "-a", str(args.app), "-regtest", f"-datadir={datadir}", f"-webuiconfig={base / 'chains-app.json'}", uri], env, 120)
        print(f"  second start: exit {code}: {out.strip()}", flush=True)
        if code != 0 or "handed" not in out:
            failures.append(f"second start with a URI: exit {code}, {out.strip()} {err.strip()[-300:]}")
    else:
        failures.append("the smoke script never got to the URI step")
    code = app.wait(900)
    result = app.result()
    if code != 0 or not result or result["failures"]:
        failures.append(f"smoke run: exit {code}, {result['failures'] if result else 'no result'}")
    check_shutdown(datadir, failures)
    if result:
        shots += result["shots"]
        print(f"  {result['passed']} checks passed", flush=True)

    missing = [s for s in shots if not (args.shots / f"{s}.png").is_file()]
    if missing:
        failures.append(f"missing pictures: {missing}")
    print(f"== {len(shots)} pictures in {args.shots}", flush=True)
    if failures:
        print("FAILED:\n  " + "\n  ".join(failures), flush=True)
        return 1
    print("OK", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
