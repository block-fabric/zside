#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A sidechain node keeps a good history: -prune below 10000 MiB refuses to start.

The mainchain node is a stand-in (test_framework/sidechain_standin.py).

- -prune=2, 550, 4999, 5000 or 9999: the node does not start, and says why;
- -prune=10000 and more: the node starts, pruning to that target, and its blocks are mined and kept;
- -prune=1 (manual pruning) and -prune=0 (off) are taken;
- regtest without -sidechainslot is not a sidechain: Bitcoin's minimum (550 MiB).
"""

from test_framework.sidechain_standin import StandInMainchain
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, rpc_port

SLOT = 3
MIB = 1024 * 1024
ERROR = "Error: -prune must be at least 10000 MiB on a sidechain (0 turns pruning off, 1 allows manual pruning, which keeps as much)."


class SidechainPruneTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=my wallet",
        ]
        self.extra_args = [self.side_args]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.main = StandInMainchain(SLOT)
        self.main.serve(self.main_rpc_port)
        self.setup_nodes()

    def run_test(self):
        try:
            self.run_prune_test()
        finally:
            self.main.shutdown()

    def mine(self, count):
        node = self.nodes[0]
        for _ in range(count):
            candidate = node.createbmmblock(self.address)
            self.main.add_block(bmm=candidate["hash"])
            node.syncmainchain()
            assert_equal(node.getbestblockhash(), candidate["hash"])

    def run_prune_test(self):
        node = self.nodes[0]
        for _ in range(2):
            self.main.add_block()
        node.syncmainchain()
        self.address = node.getnewaddress()
        self.mine(5)
        self.stop_node(0)

        self.log.info("-prune below 10000 MiB: the sidechain node refuses to start")
        for value in (2, 550, 4999, 5000, 9999):
            node.assert_start_raises_init_error(self.side_args + [f"-prune={value}"], ERROR, match=ErrorMatch.FULL_TEXT)

        self.log.info("-prune of 10000 MiB and more: pruning to that target")
        for value in (10000, 12000):
            self.start_node(0, self.side_args + [f"-prune={value}"])
            info = node.getblockchaininfo()
            assert info["pruned"]
            assert info["automatic_pruning"]
            assert_equal(info["prune_target_size"], value * MIB)
            self.mine(2)
            # Nothing pruned: the blocks are far below the target, and within the depth a reorg can reach.
            assert_equal(node.getblockchaininfo()["pruneheight"], 0)
            node.getblock(node.getblockhash(1))
            self.stop_node(0)

        self.log.info("-prune=1 (manual pruning) and -prune=0 (off) are taken")
        self.start_node(0, self.side_args + ["-prune=1"])
        info = node.getblockchaininfo()
        assert info["pruned"]
        assert not info["automatic_pruning"]
        self.stop_node(0)
        self.start_node(0, self.side_args + ["-prune=0"])
        assert not node.getblockchaininfo()["pruned"]
        self.stop_node(0)

        self.log.info("Regtest without a slot is not a sidechain: Bitcoin's minimum")
        node.assert_start_raises_init_error(["-prune=549"], "Error: Prune configured below the minimum of 550 MiB.  Please use a higher number.", match=ErrorMatch.FULL_TEXT)


if __name__ == '__main__':
    SidechainPruneTest(__file__).main()
