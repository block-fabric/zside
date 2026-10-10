#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""When the drivechain database is rebuilt from the blocks.

- -reindex-chainstate checks the format of the database as well: one derived under other drivechain
  parameters is wiped before the blocks are connected again.
- A block file it is rebuilt from that is missing stops the node at startup, with an error that says
  how to recover.
- A pruned node that would have to rebuild it from blocks it no longer has refuses to start, and
  leaves the database as it was.
- A block of the active chain that breaks the rules under the new parameters: the chainstate goes
  back to the block before it, which is marked invalid.
"""
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.messages import tx_from_hex
from test_framework.script import CScript, OP_NOP5, OP_TRUE
from test_framework.util import assert_equal, assert_raises_rpc_error

# The escrow output of a sidechain slot (OP_DRIVECHAIN is OP_NOP5).
def escrow_script(slot):
    return CScript([OP_NOP5, bytes([slot]), OP_TRUE])


class DrivechainRebuildTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-prune=1", "-fastprune"], []]
        # The last part takes a wallet, if there is one.
        self.uses_wallet = None

    def setup_network(self):
        # Two chains of their own.
        self.setup_nodes()

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 800, sync_fun=self.no_op)
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

        self.log.info("A block file the database is rebuilt from that cannot be read: the node does not start, and says how to recover")
        blocks_dir = node.blocks_path
        blk = blocks_dir / "blk00001.dat"
        assert (blocks_dir / "blk00002.dat").exists()
        self.stop_node(0)
        saved = blk.read_bytes()
        # (A missing file is caught earlier, when the block index is loaded: the file stays, its blocks go.)
        blk.write_bytes(bytes(len(saved)))
        with node.assert_debug_log(["The sidechain database was derived under other drivechain parameters; it is rebuilt from the blocks"]):
            node.assert_start_raises_init_error(
                extra_args=self.extra_args[0] + other,
                expected_msg="Error loading the sidechain database: Failed to read block [0-9a-f]{64}\\. Restart with -reindex\\.+"
                             "\nPlease restart with -reindex or -reindex-chainstate to recover\\.",
                match=ErrorMatch.FULL_REGEX)
        # With the file back, it is rebuilt; under the former parameters again, it is the same.
        blk.write_bytes(saved)
        with node.assert_debug_log(["Bringing the sidechain database from height 0 to the chain tip at height 800"]):
            self.start_node(0, extra_args=self.extra_args[0] + other)
        assert_equal(node.getblockcount(), 800)
        with node.assert_debug_log(["The sidechain database was derived under other drivechain parameters; it is rebuilt from the blocks"]):
            self.restart_node(0)
        assert_equal((node.getbestblockhash(), node.gettxoutsetinfo()["hash_serialized_3"]), state)

        self.log.info("A pruned node does not wipe a database it cannot rebuild")
        pruned = node.pruneblockchain(400)
        assert pruned > 0
        assert "pruneheight" in node.getblockchaininfo()
        # The pruned blocks are gone (a sidechain has no getsidechainevents to ask them about).
        assert_raises_rpc_error(-1, "Block not available (pruned data)", node.getblock, node.getblockhash(1))
        node.getblock(node.getblockhash(790))
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

        if not self.is_wallet_compiled():
            return
        self.log.info("A block of the active chain that the new parameters make invalid: the chainstate goes back below it")
        other_node = self.nodes[1]
        # Four slots: an escrow output of slot 5 is subject to no drivechain rule.
        few_slots = ["-testdrivechainparam=max_sidechains@4"]
        self.restart_node(1, extra_args=few_slots)
        other_node.createwallet("escrow")
        address = other_node.getnewaddress()
        self.generatetoaddress(other_node, 110, address, sync_fun=self.no_op)
        funded = other_node.fundrawtransaction(other_node.createrawtransaction([], [{address: 1}]))["hex"]
        tx = tx_from_hex(funded)
        [escrow] = [out for out in tx.vout if out.nValue == 100000000]
        escrow.scriptPubKey = escrow_script(5)
        signed = other_node.signrawtransactionwithwallet(tx.serialize().hex())["hex"]
        invalid = self.generateblock(other_node, address, [signed], sync_fun=self.no_op)["hash"]
        last = self.generate(other_node, 2, sync_fun=self.no_op)[-1]
        assert_equal(other_node.getblockcount(), 113)
        # With all the slots, it is a deposit to a slot no sidechain holds.
        with other_node.assert_debug_log(["The sidechain database was derived under other drivechain parameters",
                                          f"block {invalid} breaks the drivechain rules (bad-dc-inactive-sidechain)",
                                          f"Block {invalid} at height 111 of the active chain breaks the drivechain rules: the chainstate goes back to height 110",
                                          "of the 1 transactions of the blocks taken back at startup returned to the mempool"]):
            self.restart_node(1, extra_args=[])
        assert_equal(other_node.getblockcount(), 110)
        assert_equal({tip["hash"]: tip["status"] for tip in other_node.getchaintips()}[last], "invalid")
        # It stays so, and the node goes on.
        self.restart_node(1, extra_args=[])
        assert_equal(other_node.getblockcount(), 110)
        self.generate(other_node, 1, sync_fun=self.no_op)
        assert_equal(other_node.getblockcount(), 111)
        assert other_node.getbestblockhash() != invalid


if __name__ == "__main__":
    DrivechainRebuildTest(__file__).main()
