#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Headers a sidechain node takes from a peer whose message ends with one it cannot take yet.

The mainchain node is a stand-in (test_framework/sidechain_standin.py), so that the test can have the
mainchain commit to blocks the node has not seen.

- A headers message whose last header fails through no fault of the peer (BLOCK_TIME_FUTURE that does
  not wait: a header too far ahead of the node's clock): the headers taken before it are the peer's,
  and their blocks are asked for from it. The peer is not punished.
"""

import time

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CBlockHeader, msg_headers
from test_framework.p2p import P2PDataStore
from test_framework.sidechain_standin import StandInMainchain
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, rpc_port

SLOT = 3
# How far ahead of its clock a node takes a block (Bitcoin's MAX_FUTURE_BLOCK_TIME, the template's).
MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60


class SidechainHeadersTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.extra_args = [[
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=my wallet",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.main = StandInMainchain(SLOT)
        self.main.serve(self.main_rpc_port)
        self.setup_nodes()

    def run_test(self):
        try:
            self.run_headers_test()
        finally:
            self.main.shutdown()

    def commit(self, block_hash):
        self.main.add_block(bmm=block_hash)
        self.node.syncmainchain()

    def mine(self):
        candidate = self.node.createbmmblock(self.address)
        self.commit(candidate["hash"])
        assert_equal(self.node.getbestblockhash(), candidate["hash"])
        return candidate["hash"]

    def run_headers_test(self):
        self.node = node = self.nodes[0]
        self.address = node.getnewaddress()
        for _ in range(2):
            self.main.add_block()
        node.syncmainchain()
        # Out of the initial block download: the node fetches blocks from any peer.
        for _ in range(2):
            self.mine()
        assert not node.getblockchaininfo()["initialblockdownload"]

        self.log.info("The headers before one that fails through no fault of the peer are the peer's")
        tip = node.getbestblockhash()
        height = node.getblockcount()
        block_time = node.getblock(tip)["time"] + 1
        blocks = []
        prev = int(tip, 16)
        for i in range(3):
            block = create_block(prev, create_coinbase(height + 1 + i), ntime=block_time + i)
            block.solve()
            blocks.append(block)
            prev = block.hash_int
        # Too far ahead of the node's clock: BLOCK_TIME_FUTURE (time-too-new), which does not wait.
        ahead = create_block(prev, create_coinbase(height + 4), ntime=int(time.time()) + MAX_FUTURE_BLOCK_TIME + 3600)
        ahead.solve()
        # The mainchain commits to all of them: only the time of the last is wrong.
        for block in blocks + [ahead]:
            self.main.add_block(bmm=block.hash_hex)
        node.syncmainchain()

        peer = node.add_p2p_connection(P2PDataStore())
        with node.assert_debug_log(["time-too-new"]):
            peer.send_and_ping(msg_headers([CBlockHeader(b) for b in blocks + [ahead]]))
        assert_equal(node.getblockheader(blocks[-1].hash_hex)["height"], height + 3)
        # Their blocks are asked for, from the peer that sent them.
        peer.wait_until(lambda: blocks[0].hash_int in peer.getdata_requests, timeout=10)
        # Not punished.
        assert peer.is_connected
        assert_equal(len(node.getpeerinfo()), 1)


if __name__ == '__main__':
    SidechainHeadersTest(__file__).main()
