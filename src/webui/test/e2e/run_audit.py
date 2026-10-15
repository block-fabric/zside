#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end test of the fixes of the audit of chains-app (doc/webui-audit.md).

Runs chains-app on regtest in a virtual display, several times:

  1. audit.js: the sync overlay, a block with a sidechain proposal whose title
     is not valid UTF-8 (built here and submitted over RPC), long calls, a long
     history kept up to date in parts, keys unlocked for one call, the contrast
     of every theme, aligned tables, the frame, settings that wait for Apply;
  2. audit_restart.js: the same data directory again: a change never applied
     is gone, an applied one stays;
  3. audit_nowallet.js: -disablewallet;
  4. audit_narrow.js: windows 700 and 480 pixels wide;
  5. audit_signer.js: a hardware wallet (the mock signer of the functional tests);
  6. audit_lang.js: -lang=fr.

    run_audit.py --app build/bin/chains-app --tmp <dir> --shots <dir>
"""

import argparse
import base64
import json
import shutil
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[3] / "test" / "functional"))

from run_smoke import App, check_shutdown, environment, script  # noqa: E402

MOCK_SIGNER = HERE.parents[3] / "test" / "functional" / "mocks" / "signer.py"


class Rpc:
    def __init__(self, port, user, password):
        self.url = f"http://127.0.0.1:{port}/"
        self.auth = "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()

    def __getattr__(self, method):
        def call(*params):
            body = json.dumps({"jsonrpc": "1.0", "id": 1, "method": method, "params": list(params)}).encode()
            req = urllib.request.Request(self.url, body, {"Authorization": self.auth, "Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=60) as resp:
                answer = json.loads(resp.read())
            if answer.get("error"):
                raise RuntimeError(answer["error"])
            return answer["result"]

        return call


def submit_bad_proposal(rpc):
    """A block whose coinbase proposes a sidechain with a title that is not valid UTF-8 (audit H1)."""
    from test_framework.blocktools import add_witness_commitment, create_block
    from test_framework.drivechain import PROPOSAL_TAG, message
    from test_framework.messages import CTxOut

    template = rpc.getblocktemplate({"rules": ["segwit"]})
    title = b"Bad \xff\xfe title"
    payload = bytes([7]) + bytes([0, len(title)]) + title + b"desc \xc3\x28" + bytes(32) + bytes(20)
    block = create_block(tmpl=template)
    block.vtx[0].vout.append(CTxOut(0, message(PROPOSAL_TAG, payload)))
    add_witness_commitment(block)
    block.solve()
    return rpc.submitblock(block.serialize().hex())


def run(app_path, base, name, script_name, args, env, shots, failures, timeout=1200, on_event=None, prelude=""):
    print(f"== {name}", flush=True)
    path = base / f"{name}.js"
    path.write_text(script(script_name, prelude))
    app = App(app_path, args + [f"-webuitestscript={path}", f"-webuitestshots={shots.resolve()}"], env, base / f"{name}.log")
    if on_event:
        on_event(app)
    code = app.wait(timeout)
    result = app.result()
    if code != 0 or not result or result["failures"]:
        failures.append(f"{name}: exit {code}, {result['failures'] if result else 'no result'}")
    if result:
        print(f"  {result['passed']} checks passed", flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--tmp", required=True, type=Path)
    parser.add_argument("--shots", required=True, type=Path)
    parser.add_argument("--port", type=int, default=29530)
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
    # English, whatever the locale of the machine running the tests (the language run sets -lang).
    env["LANG"] = "C.UTF-8"
    env.pop("LC_ALL", None)
    env.pop("LC_MESSAGES", None)
    failures = []
    port = args.port
    common = ["-regtest", "-listen=0", "-dnsseed=0", "-fixedseeds=0"]

    # 1. The main run, with an RPC server for the block made here.
    datadir = base / "datadir"
    datadir.mkdir()
    config = base / "chains-app.json"
    rpc_args = ["-server", f"-rpcport={port + 1}", "-rpcuser=audit", "-rpcpassword=audit", "-rpcbind=127.0.0.1", "-rpcallowip=127.0.0.1"]
    main_args = common + [f"-port={port}", f"-datadir={datadir}", f"-webuiconfig={config}"] + rpc_args

    def submit_block(app):
        if not app.wait_event("READY_FOR_BLOCK", 300):
            failures.append("audit: the script never asked for the block")
            return
        result = submit_bad_proposal(Rpc(port + 1, "audit", "audit"))
        print(f"  block with a title that is not UTF-8: {result or 'accepted'}", flush=True)
        if result is not None:
            failures.append(f"the block was refused: {result}")

    run(args.app, base, "audit", "audit.js", main_args, env, args.shots, failures, on_event=submit_block)
    check_shutdown(datadir, failures)
    settings = json.loads((datadir / "regtest" / "settings.json").read_text())
    print(f"  settings.json at the stop: prune={settings.get('prune')!r} par={settings.get('par')!r}", flush=True)

    # 2. Again: the change never applied is taken back, the applied one stays.
    run(args.app, base, "audit-restart", "audit_restart.js", common + [f"-port={port}", f"-datadir={datadir}", f"-webuiconfig={config}"], env, args.shots, failures)
    settings = json.loads((datadir / "regtest" / "settings.json").read_text())
    if settings.get("prune") not in (None, "0", 0):
        failures.append(f"settings.json kept the unapplied prune: {settings}")

    # 3. Without wallets.
    nowallet = base / "nowallet"
    nowallet.mkdir()
    # A chains.conf that turns on a debug option of the app: ignored, only the command line counts (audit L11).
    (nowallet / "chains.conf").write_text("webuidevtools=1\n")
    run(args.app, base, "audit-nowallet", "audit_nowallet.js", common + [f"-port={port + 2}", f"-datadir={nowallet}", "-disablewallet", f"-webuiconfig={base / 'nowallet.json'}"], env, args.shots, failures)
    check_shutdown(nowallet, failures)

    # 4. Narrow windows.
    for width in (700, 480):
        narrow = base / f"narrow{width}"
        narrow.mkdir()
        conf = base / f"narrow{width}.json"
        conf.write_text(json.dumps({"window": {"width": width, "height": 760, "maximized": False}}))
        run(args.app, base, f"audit-narrow{width}", "audit_narrow.js", common + [f"-port={port + 3}", f"-datadir={narrow}", f"-webuiconfig={conf}"], env, args.shots, failures)

    # 5. A hardware wallet: the mock signer of the functional tests, run where it finds its answers.
    signer_dir = base / "signer"
    signer_dir.mkdir()
    wrapper = signer_dir / "signer.sh"
    wrapper.write_text(f"#!/bin/sh\ncd '{signer_dir}' && exec '{sys.executable}' '{MOCK_SIGNER}' \"$@\"\n")
    wrapper.chmod(0o755)
    hww = base / "hww"
    hww.mkdir()
    run(args.app, base, "audit-signer", "audit_signer.js", common + [f"-port={port + 5}", f"-datadir={hww}", f"-signer={wrapper}", "-keypool=10", f"-webuiconfig={base / 'hww.json'}"],
        env, args.shots, failures, prelude=f"window.__SIGNER_DIR = {json.dumps(str(signer_dir))};\n")

    # 6. In French.
    french = base / "french"
    french.mkdir()
    run(args.app, base, "audit-lang", "audit_lang.js", common + [f"-port={port + 4}", f"-datadir={french}", "-lang=fr", f"-webuiconfig={base / 'french.json'}"], env, args.shots, failures)

    if failures:
        print("FAILED:\n  " + "\n  ".join(failures), flush=True)
        return 1
    print("OK", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
