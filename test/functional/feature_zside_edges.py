#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the shielded pool where feature_zside.py and feature_zside_rules.py do not: the orchard index
after an unclean shutdown that followed the removal of a block."""

from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.util import assert_equal, assert_greater_than


class ZSideEdgesTest(SidechainTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def mine_txs(self, node=None):
        if node is None:
            self.sync_mempools()
        return self.bmm(node=node)

    def mine_without_txs(self, node):
        """A block of no transaction but the coinbase, on the node's tip, committed to by the mainchain."""
        block = self.generateblock(node, self.side_address, [], False, sync_fun=self.no_op)
        self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        assert_equal(node.submitblock(block["hex"]), None)
        assert_equal(node.getbestblockhash(), block["hash"])
        return block["hash"]

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain, fund both wallets, and shield")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "zSide", "Private payments")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(), 20)
        main.createsidechaindeposit(SLOT, other.getnewaddress(), 5)
        self.mine_main()
        self.bmm()
        side.shieldcoins(5)
        other.shieldcoins(2)
        self.mine_txs()
        assert_equal(side.getshieldedbalance()["available"], 5)
        assert_equal(other.getshieldedbalance()["available"], 2)

        self.run_index_crash_test()

    def run_index_crash_test(self):
        side, other = self.nodes
        self.log.info("The orchard index after an unclean shutdown that followed the removal of a block: the tree of its best block")
        # (Stopped cleanly first: what the node did so far is not lost with the process.)
        self.restart_node(0)
        self.connect_nodes(0, 1)
        self.mine_txs()
        self.disconnect_nodes(0, 1)
        side.sendshielded({side.getshieldedaddress(True): Decimal("0.1")})
        block = self.bmm(node=side, expect=False)
        # Flushed with the block: the index's best block on disk is it.
        side.gettxoutsetinfo()
        side.syncwithvalidationinterfacequeue()
        with_block = side.getshieldedinfo()
        assert_equal(with_block["index"]["synced"], True)
        assert_equal((with_block["index"]["notes"], with_block["index"]["root"]), (with_block["notes"], with_block["root"]))
        # Taken away, another block in its place: the index takes the block away, then takes this one in.
        side.invalidateblock(block)
        self.mine_without_txs(side)
        side.syncwithvalidationinterfacequeue()
        without = side.getshieldedinfo()
        assert_equal((without["index"]["notes"], without["index"]["root"]), (without["notes"], without["root"]))
        assert_greater_than(with_block["notes"], without["notes"])
        # Stopped before anything is flushed: the chain on disk still ends with the block.
        side.kill_process()
        self.start_node(0)
        assert_equal(side.getbestblockhash(), block)
        self.wait_until(lambda: side.getshieldedinfo()["index"]["synced"])
        assert_equal(side.getshieldedinfo(), with_block)
        # The wallet spends every note, the block's too: the index finds them all.
        available = side.getshieldedbalance()["available"]
        notes = side.getshieldedbalance()["notes"]
        txid = side.sendshielded({side.getshieldedaddress(True): available - Decimal("0.01")})["txid"]
        spent = [n for n in side.listshieldednotes(True) if n.get("spent_by") == txid]
        assert_equal(len(spent), notes)
        self.connect_nodes(0, 1)
        # (Made while the nodes were apart, the other has not heard of it: the first node mines it.)
        self.bmm()
        assert_equal(side.gettransaction(txid)["confirmations"], 1)
        info = side.getshieldedinfo()
        assert_equal((info["index"]["notes"], info["index"]["root"]), (info["notes"], info["root"]))
        self.check_in_sync()


if __name__ == "__main__":
    ZSideEdgesTest(__file__).main()
