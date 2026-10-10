#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A stand-in for the mainchain node a sidechain node follows: an RPC server of the test.

It answers the calls a sidechain node makes of its mainchain node from a record the test writes, so
that a test can have the mainchain do what it wants, when it wants: commit to any block, deposit
anything, reorganise, vote, refuse a call, answer wrong or not at all.
"""

import base64
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Answered by createbmmrequest when a request for another block of the slot pays more.
OUTBID = "Outbid: the mempool holds a request for another block of this sidechain paying 0.001 CHN"


def block_hash(height, branch=0):
    return f"{branch:02x}{height:062x}"


class StandInMainchain:
    """What the stand-in mainchain node has, and how it answers."""

    def __init__(self, slot):
        self.lock = threading.RLock()
        self.slot = slot
        self.blocks = []
        self.sidechain = {"activationheight": 1, "proposalhash": "aa" * 32}
        # Answers that a real node gives in some states, or that an older or broken one gives.
        self.ibd = False
        self.no_proposed = False
        # getsidechainevents from this height on answers an error.
        self.fail_from = None
        # getsidechainevents answers, at this height, a block that does not follow the one before.
        self.stray_at = None
        # method -> error message to answer with, or HTTP status to answer with and no JSON.
        self.errors = {}
        self.statuses = {}
        # Credentials the node must give ("user:password"); any if not set.
        self.credentials = None
        # The prevblockhash createbmmrequest answers, if not the tip's.
        self.request_prev = None
        self.main_script = "0014" + "11" * 20
        self.calls = {}
        self.paths = {}
        self.requests = []
        self.received = []
        self.vouched = []

    def add_block(self, *, bmm=None, deposits=(), bundles=(), proposed=(), pending=None, branch=0):
        """Add a block on the tip. The bundles pending after it are, unless given, those pending after the
        block before but the ones it closed, with their scores (as a mainchain without votes has them)."""
        with self.lock:
            if pending is None:
                closed = {b["hash"] for b in bundles}
                pending = [p for p in self.blocks[-1]["pending"] if p["hash"] not in closed] if self.blocks else []
            height = len(self.blocks)
            block = {
                "height": height,
                "hash": block_hash(height, branch),
                "time": 1790900000 + height,
                "mediantime": 1790900000 + height,
                "deposits": [dict(d) for d in deposits],
                "bundles": [dict(b) for b in bundles],
                "proposed": list(proposed),
                "pending": [dict(p) for p in pending],
            }
            if height > 0:
                block["previousblockhash"] = self.blocks[-1]["hash"]
            if bmm is not None:
                block["bmm"] = bmm
            self.blocks.append(block)
            return block["hash"]

    def height(self):
        with self.lock:
            return len(self.blocks) - 1

    def events(self, height, count):
        if self.fail_from is not None and height >= self.fail_from:
            return None, {"code": -1, "message": "the stand-in is not answering this now"}
        answer = [dict(block) for block in self.blocks[height:height + count]]
        for block in answer:
            if block["height"] == self.stray_at:
                block.update({"hash": block_hash(self.stray_at, 0x77), "previousblockhash": block_hash(self.stray_at - 1, 0x78)})
        if self.no_proposed:
            for block in answer:
                block.pop("proposed")
        return answer, None

    def call(self, method, params):
        with self.lock:
            self.calls[method] = self.calls.get(method, 0) + 1
            if method in self.errors:
                return None, {"code": -26, "message": self.errors[method]}
            if method == "getsidechainevents":
                return self.events(params[1], params[2] if len(params) > 2 else 1)
            if method == "getblockcount":
                return len(self.blocks) - 1, None
            if method == "getblockchaininfo":
                return {"chain": "regtest", "blocks": len(self.blocks) - 1, "initialblockdownload": self.ibd}, None
            if method == "getsidechain":
                if self.sidechain is None:
                    return None, {"code": -8, "message": "No active sidechain in this slot"}
                return dict(self.sidechain, slot=self.slot), None
            if method == "vouchwithdrawalbundle":
                self.vouched.append(params[1] if len(params) > 1 else None)
                return None, None
            if method == "receivewithdrawalbundle":
                self.received.append(params[1])
                return None, None
            if method == "sendwithdrawalbundle":
                return {"sent": False}, None
            if method == "createbmmrequest":
                self.requests.append(params)
                prev = self.request_prev or self.blocks[-1]["hash"]
                return {"txid": "77" * 32, "prevblockhash": prev}, None
            if method == "validateaddress":
                if params[0] == "nonsense":
                    return {"isvalid": False}, None
                if params[0] == "malformed":
                    return {"isvalid": True, "scriptPubKey": "zz"}, None
                return {"isvalid": True, "scriptPubKey": self.main_script}, None
            return None, {"code": -32601, "message": "Method not found"}

    def serve(self, port):
        """Answer on the port until shutdown()."""
        standin = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                method = request["method"]
                with standin.lock:
                    standin.paths[method] = self.path
                    credentials = standin.credentials
                    status = standin.statuses.get(method)
                given = base64.b64decode(self.headers.get("Authorization", "Basic ")[6:]).decode()
                if credentials is not None and given != credentials:
                    self.send_response(401)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if status is not None:
                    body = b"not json"
                    self.send_response(status)
                else:
                    result, error = standin.call(method, request.get("params", []))
                    body = json.dumps({"result": result, "error": error, "id": request.get("id")}).encode()
                    self.send_response(200 if error is None else 500)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def shutdown(self):
        self.server.shutdown()
        self.server.server_close()
