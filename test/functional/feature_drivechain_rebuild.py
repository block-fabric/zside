#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""When the drivechain database is rebuilt from the blocks.

- -reindex-chainstate checks the format of the database as well: one derived under other drivechain
  parameters is wiped before the blocks are connected again.
- A pruned node that would have to rebuild it from blocks it no longer has refuses to start, and
  leaves the database as it was.
"""
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal


class DrivechainRebuildTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-prune=1", "-fastprune"]]

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 800)
        # (The template has no getdrivechaininfo: the chain and its coins stand for the state.)
        state = (node.getbestblockhash(), node.gettxoutsetinfo()["hash_serialized_3"])
        other = ["-testdrivechainparam=max_pending_bundles@63"]

        self.log.info("-reindex-chainstate wipes a database derived under other parameters before connecting the blocks")
        self.restart_node(0, extra_args=self.extra_args[0] + other)
        with node.assert_debug_log(["The sidechain database was derived under other drivechain parameters; it is rebuilt from the blocks"]):
            self.restart_node(0, extra_args=["-reindex-chainstate"])
        self.wait_until(lambda: node.getblockcount() == 800)
        # Marked as derived under the parameters it was built with: a restart uses it as it is.
        with node.assert_debug_log([], unexpected_msgs=["it is rebuilt from the blocks", "Bringing the sidechain database"]):
            self.restart_node(0)
        assert_equal((node.getbestblockhash(), node.gettxoutsetinfo()["hash_serialized_3"]), state)

        self.log.info("A pruned node does not wipe a database it cannot rebuild")
        pruned = node.pruneblockchain(400)
        assert pruned > 0
        assert "pruneheight" in node.getblockchaininfo()
        self.stop_node(0)
        node.assert_start_raises_init_error(
            extra_args=self.extra_args[0] + other,
            expected_msg="The sidechain database has to be rebuilt from the blocks \\(it is derived under other drivechain parameters\\), and this pruned node no longer has block",
            match=ErrorMatch.PARTIAL_REGEX)
        # Nothing was changed: under the former parameters the node starts from its database.
        with node.assert_debug_log([], unexpected_msgs=["it is rebuilt from the blocks", "Bringing the sidechain database"]):
            self.start_node(0)
        assert_equal((node.getbestblockhash(), node.gettxoutsetinfo()["hash_serialized_3"]), state)
        assert_equal(node.getblockcount(), 800)


if __name__ == "__main__":
    DrivechainRebuildTest(__file__).main()
