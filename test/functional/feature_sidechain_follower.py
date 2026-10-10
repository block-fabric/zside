#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test how a sidechain node follows a mainchain node that answers what the test wants.

The mainchain node is a stand-in, an RPC server of the test, so that answers no real node gives can
be tried: blocks that do not follow each other, blocks without their median time or pending bundles,
amounts that are none, a node on another chain, a node behind the record, a reorg deeper than the
node can take back, commitments to blocks nobody has, another sidechain in the slot.
"""

import json
import re
from decimal import Decimal
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from test_framework.descriptors import descsum_create
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, rpc_port

SLOT = 3


def block_hash(height, branch=0):
    return f"{branch:02x}{height:062x}"


class Mainchain:
    """What the stand-in mainchain node has, and how it answers."""

    def __init__(self):
        self.lock = threading.Lock()
        self.blocks = []
        self.sidechain = {"activationheight": 1, "proposalhash": "aa" * 32}
        # Answers that a real node does not give.
        self.no_mediantime = False
        self.no_pending = False
        self.bad_amount = False
        self.broken_after = None
        self.calls = {}
        for _ in range(3):
            self.add_block()

    def add_block(self, branch=0, bmm=None):
        height = len(self.blocks)
        block = {
            "height": height,
            "hash": block_hash(height, branch),
            "time": 1790900000 + height,
            "mediantime": 1790900000 + height,
            "deposits": [],
            "bundles": [],
            "proposed": [],
            "pending": [],
        }
        if height > 0:
            block["previousblockhash"] = self.blocks[-1]["hash"]
        if bmm is not None:
            block["bmm"] = bmm
        self.blocks.append(block)

    def events(self, height, count):
        answer = [dict(block) for block in self.blocks[height:height + count]]
        if self.no_mediantime:
            for block in answer:
                block.pop("mediantime")
        if self.no_pending:
            for block in answer:
                block.pop("pending")
        if self.bad_amount and answer:
            # An amount that is none: AmountFromValue throws, which must not stop the node.
            answer[-1]["deposits"] = [{"destination": "x", "amount": "lots", "txid": "11" * 32, "burnindex": 0}]
        if self.broken_after is not None and answer and answer[0]["height"] == self.broken_after:
            # The block on record, then one that does not follow it.
            stray = dict(self.blocks[0])
            stray.update({"height": self.broken_after + 1, "hash": block_hash(self.broken_after + 1, branch=7), "previousblockhash": block_hash(5, branch=9)})
            answer = [answer[0], stray]
        return answer

    def call(self, method, params):
        with self.lock:
            self.calls[method] = self.calls.get(method, 0) + 1
            if method == "getsidechainevents":
                return self.events(params[1], params[2] if len(params) > 2 else 1), None
            if method == "getblockcount":
                return len(self.blocks) - 1, None
            if method == "getblockchaininfo":
                return {"chain": "regtest", "blocks": len(self.blocks) - 1, "initialblockdownload": False}, None
            if method == "getsidechain":
                if self.sidechain is None:
                    return None, {"code": -8, "message": "No active sidechain in this slot"}
                return dict(self.sidechain, slot=SLOT), None
            if method == "vouchwithdrawalbundle":
                return None, None
            if method == "createbmmrequest":
                # A request made, said in a way no real node says it: the block it follows is no hash.
                return {"txid": "11" * 32, "prevblockhash": "not a hash"}, None
            return None, {"code": -32601, "message": "Method not found"}


def make_handler(mainchain):
    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            result, error = mainchain.call(request["method"], request.get("params", []))
            body = json.dumps({"result": result, "error": error, "id": request.get("id")}).encode()
            self.send_response(200 if error is None else 500)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    return Handler


class SidechainFollowerTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.extra_args = [[
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
        ]]

    def setup_network(self):
        self.mainchain = Mainchain()
        self.server = ThreadingHTTPServer(("127.0.0.1", self.main_rpc_port), make_handler(self.mainchain))
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        super().setup_network()

    def on_record(self, height):
        node = self.nodes[0]
        self.wait_until(lambda: node.getmainchaininfo()["height"] == height and node.getmainchaininfo()["connected"])

    def run_test(self):
        node = self.nodes[0]
        main = self.mainchain
        self.on_record(2)

        self.log.info("Blocks that do not follow each other: an error, asked again later, not at once for ever")
        with main.lock:
            main.add_block()
            main.add_block()
            main.broken_after = 2
            main.calls = {}
        self.wait_until(lambda: "do not follow each other" in node.getmainchaininfo().get("error", ""))
        with main.lock:
            main.calls = {}
        time.sleep(3)
        with main.lock:
            calls = main.calls.get("getsidechainevents", 0)
        # One call a second or so, as with any error; a node that spins makes thousands.
        assert calls <= 10, f"{calls} calls in 3 seconds"
        assert_equal(node.getmainchaininfo()["height"], 2)
        with main.lock:
            main.broken_after = None
        self.on_record(4)

        self.log.info("Blocks without their median time: the mainchain node is too old to follow")
        with main.lock:
            main.add_block()
            main.no_mediantime = True
        self.wait_until(lambda: "median time" in node.getmainchaininfo().get("error", ""))
        assert_equal(node.getmainchaininfo()["height"], 4)
        with main.lock:
            main.no_mediantime = False
        self.on_record(5)

        self.log.info("Blocks without the bundles pending after them: the mainchain node is too old to follow")
        with main.lock:
            main.add_block()
            main.no_pending = True
        self.wait_until(lambda: "pending after blocks" in node.getmainchaininfo().get("error", ""))
        assert_equal(node.getmainchaininfo()["height"], 5)
        with main.lock:
            main.no_pending = False
        self.on_record(6)

        self.log.info("An amount that is none: a bad answer, asked again later; the node keeps running")
        with main.lock:
            main.add_block()
            main.bad_amount = True
        self.wait_until(lambda: "not an amount" in node.getmainchaininfo().get("error", ""))
        time.sleep(2)
        assert_equal(node.getmainchaininfo()["height"], 6)
        assert_equal(node.getblockcount(), 0)
        with main.lock:
            main.bad_amount = False
        self.on_record(7)

        self.log.info("Commitments to blocks nobody announced do not make the node think it is catching up")
        with main.lock:
            for i in range(3):
                main.add_block(bmm=f"{0xee:02x}{i:062x}")
            main.calls = {}
        # A node that has just started tells its mainchain node what it vouches for (none here), once it
        # has caught up; one that takes junk commitments for blocks to catch up with never does.
        self.restart_node(0, extra_args=self.extra_args[0] + ["-maxtipage=2000000000"])
        self.on_record(10)
        self.wait_until(lambda: main.calls.get("vouchwithdrawalbundle", 0) > 0)

        self.log.info("A mainchain node on another chain: the record is not dropped")
        with main.lock:
            good = main.blocks
            main.blocks = []
            for _ in range(12):
                main.add_block(branch=5)
        self.wait_until(lambda: "on another chain" in node.getmainchaininfo().get("error", ""))
        assert_equal(node.getmainchaininfo()["height"], 10)
        assert_equal(node.getmainchaininfo()["bestblockhash"], block_hash(10, 0))
        with main.lock:
            main.blocks = good
        self.on_record(10)

        self.log.info("A mainchain node behind the record, on the same chain: the node waits, nothing is dropped")
        with main.lock:
            # Restarted after losing its last blocks, say: its tip is the record's block at that height.
            main.blocks = good[:7]
        self.wait_until(lambda: "behind the record" in node.getmainchaininfo().get("error", ""))
        time.sleep(2)
        assert_equal(node.getmainchaininfo()["height"], 10)
        assert_equal(node.getmainchaininfo()["bestblockhash"], block_hash(10, 0))
        with main.lock:
            main.blocks = good
        self.on_record(10)

        self.log.info("A reorg that drops more commitments than the node can take back waits for the operator")
        with main.lock:
            for i in range(2885):
                main.add_block(bmm=f"{0xdd:02x}{i:062x}")
        self.on_record(10 + 2885)
        with main.lock:
            # The mainchain node says the blocks above height 8 are gone, replaced by a shorter branch.
            main.blocks = main.blocks[:9]
            for _ in range(5):
                main.add_block(branch=3)
        self.wait_until(lambda: "call syncmainchain true" in node.getmainchaininfo().get("error", ""))
        assert_equal(node.getmainchaininfo()["height"], 10 + 2885)
        assert_equal(node.syncmainchain(True), 13)
        assert_equal(node.getmainchaininfo()["bestblockhash"], block_hash(13, 3))
        self.on_record(13)

        self.log.info("A commitment request the mainchain node answers with junk: an error, and no block waits for it")
        address = node.deriveaddresses(descsum_create(f"raw(0014{'11' * 20})"))[0]
        assert_raises_rpc_error(-1, "the mainchain node sent something that is not a hash", node.requestbmmblock, address, Decimal("0.0001"))
        assert_equal(main.calls.get("createbmmrequest"), 1)
        assert_equal(node.getblockcount(), 0)

        self.log.info("Another sidechain in the slot: the node stops rather than follow it")
        with main.lock:
            main.sidechain = {"activationheight": 6, "proposalhash": "bb" * 32}
            main.add_block()
        node.wait_until_stopped(expect_error=True, expected_stderr=re.compile("is another one"))
        self.server.shutdown()


if __name__ == '__main__':
    SidechainFollowerTest(__file__).main()
