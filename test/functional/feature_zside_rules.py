#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the shielded rules where feature_zside.py does not: every reject reason through the mempool
(sendrawtransaction, testmempoolaccept) and blocks (generateblock, submitblock), the peer that relays
an invalid proof, a reorg that undoes a conflict of the wallet, the wallet's refusals, -orchardindex
off, an index whose best block is lost, -reindex and -reindex-chainstate."""

from decimal import Decimal
import os
import shutil

from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.messages import COIN, CTxOut, from_hex, CBlock, msg_tx, tx_from_hex
from test_framework.blocktools import add_witness_commitment
from test_framework.p2p import P2PInterface
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

MAX_MONEY = 21000000 * COIN
# The marker: OP_RETURN, OP_PUSHDATA2 and two bytes of length, the tag "ZORC", the version; then the bundle.
BUNDLE_AT = 1 + 3 + 4 + 1
ACTION_SIZE = 820


def marker_index(tx):
    return [i for i, o in enumerate(tx.vout) if o.scriptPubKey[:1] == b"\x6a" and o.scriptPubKey[4:8] == b"ZORC"][0]


class Bundle:
    """Where the parts of the bundle of a marker are (fewer than 253 actions)."""

    def __init__(self, script):
        assert_equal(script[1], 0x4d)
        self.actions = script[BUNDLE_AT]
        assert self.actions < 0xfd
        self.value_balance = BUNDLE_AT + 1 + self.actions * ACTION_SIZE + 1
        self.anchor = self.value_balance + 8
        self.proof = self.anchor + 32 + 3
        # The commitment of the first action's note: cv, nf, rk, then cmx.
        self.cmx = BUNDLE_AT + 1 + 3 * 32


def tamper(hex_tx, change):
    """The transaction, changed by change(tx, its marker script, where the parts of the bundle are)."""
    tx = tx_from_hex(hex_tx)
    i = marker_index(tx)
    script = bytearray(tx.vout[i].scriptPubKey)
    change(tx, script, Bundle(script))
    tx.vout[i].scriptPubKey = bytes(script)
    return tx


class ZSideRulesTest(SidechainTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def mine_txs(self, node=None):
        if node is None:
            self.sync_mempools()
        return self.bmm(node=node)

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

        self.run_wallet_refusals_test()
        self.run_mempool_rejects_test()
        self.run_block_rejects_test()
        self.run_conflict_undone_test()
        self.run_too_many_notes_test()
        self.run_index_off_test()
        self.run_index_lost_test()
        self.run_reindex_test()

    def run_wallet_refusals_test(self):
        side, other = self.nodes
        self.log.info("What the wallet refuses before it proves anything")
        address = other.getshieldedaddress()
        taddress = other.getnewaddress()
        assert_raises_rpc_error(-5, "Not a shielded address", side.shieldcoins, 1, taddress)
        assert_raises_rpc_error(-8, "No recipients", side.sendshielded, {})
        assert_raises_rpc_error(-5, "Not an address: nonsense", side.sendshielded, {"nonsense": 1})
        assert_raises_rpc_error(-8, "A memo is at most 512 bytes of text", side.sendshielded, {address: 1}, "x" * 513)
        assert_raises_rpc_error(-4, "Invalid amount", side.sendshielded, {address: 0})
        assert_raises_rpc_error(-5, "Not an address", side.unshieldcoins, 1, "nonsense")
        assert_raises_rpc_error(-4, "The amount does not pay the fee of", side.unshieldcoins, Decimal("0.00001"), taddress, True)
        # More shielded outputs than a transaction the network relays has actions for.
        many = {other.getshieldedaddress(True): Decimal("0.001") for _ in range(40)}
        assert_equal(len(many), 40)
        assert_raises_rpc_error(-4, "Too many recipients, or notes to spend, for one transaction the network relays", side.sendshielded, many)
        assert_raises_rpc_error(-4, "Not enough shielded funds: 5.00 available", side.sendshielded, {address: 6})
        # Melt/cast: no negative spread; a step id that is a number.
        assert_raises_rpc_error(-4, "spread must not be negative", side.meltcoins, -1)
        assert_raises_rpc_error(-4, "spread must not be negative", side.castcoins, Decimal("0.01"), -1)
        assert_raises_rpc_error(-8, 'id must be a step number or "all"', side.cancelmeltcast, "first")
        assert_equal(side.cancelmeltcast("all"), 0)
        assert_equal(side.listmeltcast(), [])

        self.log.info("What bumpshieldedfee refuses")
        assert_raises_rpc_error(-4, "The wallet has no such transaction", side.bumpshieldedfee, "11" * 32)
        confirmed = side.listshieldedtransactions(1)[0]["txid"]
        assert_raises_rpc_error(-4, "It is not waiting for a block", side.bumpshieldedfee, confirmed)
        txid = side.sendshielded({address: Decimal("0.1")})["txid"]
        self.sync_mempools()
        assert_raises_rpc_error(-4, "The fee of 0.50 is more than the wallet pays (-maxtxfee)", side.bumpshieldedfee, txid, Decimal("0.5"))
        # The recipient spent none of its notes in it.
        assert_raises_rpc_error(-4, "It spends no note of the account", other.bumpshieldedfee, txid)
        assert_equal(other.getshieldedbalance()["pending"], Decimal("0.1"))
        self.mine_txs()

        self.log.info("Paid, but not in a block yet: not spendable, and said so")
        other.createwallet("fresh")
        fresh = other.get_wallet_rpc("fresh")
        side.sendshielded({fresh.getshieldedaddress(): Decimal("0.2")})
        self.sync_mempools()
        assert_raises_rpc_error(-4, "Not enough shielded funds in blocks yet: 0.00 available, 0.20 waiting for a block", fresh.sendshielded, {address: Decimal("0.1")})
        self.mine_txs()
        # In a block: it goes.
        fresh.sendshielded({address: Decimal("0.1")})
        self.mine_txs()
        other.unloadwallet("fresh")

        self.log.info("The history is newest first, as many as asked")
        history = side.listshieldedtransactions(3)
        assert_equal(len(history), 3)
        assert_equal(history[0]["txid"], side.listshieldedtransactions(1)[0]["txid"])
        assert all(a["time"] >= b["time"] for a, b in zip(history, history[1:]))
        assert_equal(history[1]["txid"], txid)
        self.check_in_sync()

    def run_mempool_rejects_test(self):
        side, other = self.nodes
        self.log.info("A shielded payment, made on one node apart from the other")
        to = side.getshieldedaddress()
        self.disconnect_nodes(0, 1)
        txid = other.sendshielded({to: Decimal("0.1")})["txid"]
        raw = other.getrawtransaction(txid)
        tx = tx_from_hex(raw)
        assert_equal(len(tx.vin), 0)
        # Only the marker: no output is ever in the coins (no "already known" for it).
        assert_equal(len(tx.vout), 1)
        assert_equal(side.testmempoolaccept([raw])[0]["allowed"], True)

        def rejected(tx, reason):
            res = side.testmempoolaccept([tx.serialize().hex()], 0)[0]
            assert_equal(res["allowed"], False)
            assert_equal(res["reject-reason"], reason)
            assert_raises_rpc_error(-26, reason, side.sendrawtransaction, tx.serialize().hex(), 0, 1)

        def value_balance(value):
            def change(tx, script, b):
                script[b.value_balance:b.value_balance + 8] = value.to_bytes(8, "little", signed=True)
            return change

        self.log.info("Each shielded reason, through testmempoolaccept and sendrawtransaction")
        # Read, the bundle is no bundle: an anchor that is no field element.
        def bad_anchor(tx, script, b):
            script[b.anchor:b.anchor + 32] = b"\xff" * 32
        rejected(tamper(raw, bad_anchor), "bad-zs-bundle")

        def two_markers(tx, script, b):
            tx.vout.append(CTxOut(0, bytes(script)))
        rejected(tamper(raw, two_markers), "bad-zs-markers")

        # A field element, but no root the tree ever had: the commitment of one of its own notes.
        def unknown_anchor(tx, script, b):
            script[b.anchor:b.anchor + 32] = script[b.cmx:b.cmx + 32]
        rejected(tamper(raw, unknown_anchor), "bad-zs-anchor")

        # Out of range: nothing taken out of the pool for the inputs, and the range checked first.
        rejected(tamper(raw, value_balance(MAX_MONEY + 1)), "bad-zs-value-balance")
        # In range, more than the pool holds (the fee it would pay is absurd: the pool is checked first).
        pool = int(side.getshieldedinfo()["pool"] * COIN)
        rejected(tamper(raw, value_balance(pool + 1)), "bad-zs-pool-negative")

        # Taken out of the pool, the marker carries nothing.
        def marker_value(tx, script, b):
            tx.vout[marker_index(tx)].nValue = 1
        rejected(tamper(raw, marker_value), "bad-zs-marker-value")

        def proof_byte(tx, script, b):
            script[b.proof + 100] ^= 1
        bad_proof = tamper(raw, proof_byte)
        rejected(bad_proof, "bad-zs-proof")

        self.log.info("A peer that relays an invalid proof is disconnected; one that relays a bundle the pool refuses is not")
        honest = side.add_p2p_connection(P2PInterface())
        with side.assert_debug_log(["was not accepted: bad-zs-anchor"]):
            honest.send_and_ping(msg_tx(tamper(raw, unknown_anchor)))
        assert honest.is_connected
        liar = side.add_p2p_connection(P2PInterface())
        with side.assert_debug_log(["shielded transaction with an invalid proof"]):
            liar.send_without_ping(msg_tx(bad_proof))
            liar.wait_for_disconnect()
        honest.sync_with_ping()
        side.disconnect_p2ps()

        self.log.info("Mined, its nullifiers are spent: sent again, a double spend")
        self.connect_nodes(0, 1)
        self.mine_txs(node=other)
        self.sync_blocks()
        for node in self.nodes:
            res = node.testmempoolaccept([raw])[0]
            assert_equal(res["reject-reason"], "bad-zs-double-spend")
        assert_raises_rpc_error(-26, "bad-zs-double-spend", side.sendrawtransaction, raw)
        self.check_in_sync()

    def run_block_rejects_test(self):
        side, other = self.nodes
        self.log.info("Blocks: one with a shielded transaction that breaks a rule is refused, by the check of a template and on submission")
        self.mine_txs()
        txid = other.sendshielded({side.getshieldedaddress(): Decimal("0.1")})["txid"]
        raw = other.getrawtransaction(txid)
        self.sync_mempools()

        def proof_byte(tx, script, b):
            script[b.proof + 100] ^= 1
        bad_proof = tamper(raw, proof_byte).serialize().hex()

        def template(txs):
            return self.generateblock(side, self.side_address, txs, False, sync_fun=self.no_op)
        assert_raises_rpc_error(-25, "TestBlockValidity failed: bad-zs-proof", template, [bad_proof])
        # The same transaction twice: CheckBlock refuses it first.
        assert_raises_rpc_error(-25, "bad-txns-duplicate", template, [raw, raw])

        def submit(change_block):
            """A block of the transaction, changed, committed to by the mainchain, then submitted."""
            block = from_hex(CBlock(), template([raw])["hex"])
            change_block(block)
            block.hashMerkleRoot = block.calc_merkle_root()
            block.solve()
            request = self.main.createbmmrequest(SLOT, block.hash_hex, Decimal("0.001"))
            assert_equal(request["prevblockhash"], self.main.getbestblockhash())
            self.mine_main()
            return side.submitblock(block.serialize().hex())

        tip = side.getbestblockhash()
        # A coinbase may not carry a bundle.
        def coinbase_marker(block):
            block.vtx[0].vout.append(CTxOut(0, tx_from_hex(raw).vout[0].scriptPubKey))
        assert_equal(submit(coinbase_marker), "bad-zs-coinbase")
        assert_equal(side.getbestblockhash(), tip)

        def invalid_proof(block):
            block.vtx[1] = tx_from_hex(bad_proof)
            # The witness commitment of the coinbase, for the transaction's new wtxid.
            add_witness_commitment(block)
        assert_equal(submit(invalid_proof), "bad-zs-proof")
        assert_equal(side.getbestblockhash(), tip)
        # The valid transaction still goes into the next block.
        self.mine_txs()
        assert_equal(side.gettransaction(txid)["confirmations"], 1)
        self.check_in_sync()

        self.log.info("A valid spend whose anchor a reorg took away: out of the mempool, and refused in a block")
        other.createwallet("anchored")
        anchored = other.get_wallet_rpc("anchored")
        side.sendshielded({anchored.getshieldedaddress(): Decimal("0.3")})
        block = self.mine_txs()
        # Its only note, one block deep: the spend is anchored at the tree that block left.
        spend = anchored.sendshielded({side.getshieldedaddress(): Decimal("0.1")})["txid"]
        spend_hex = anchored.gettransaction(spend)["hex"]
        self.sync_mempools()
        side.invalidateblock(block)
        assert spend not in side.getrawmempool()
        assert_raises_rpc_error(-25, "TestBlockValidity failed: bad-zs-anchor", template, [spend_hex])
        assert_equal(side.testmempoolaccept([spend_hex])[0]["reject-reason"], "bad-zs-anchor")
        side.reconsiderblock(block)
        assert_equal(side.getbestblockhash(), block)
        side.sendrawtransaction(spend_hex)
        self.mine_txs()
        assert_equal(anchored.gettransaction(spend)["confirmations"], 1)
        other.unloadwallet("anchored")
        self.check_in_sync()

    def run_conflict_undone_test(self):
        side, other = self.nodes
        self.log.info("A spend conflicted by a block is no longer conflicted once the block is undone")
        to = other.getshieldedaddress()
        backup = os.path.join(self.options.tmpdir, "side-wallet.dat")
        side.backupwallet(backup)
        other.restorewallet("copy", backup)
        copy = other.get_wallet_rpc("copy")
        txid = side.sendshielded({to: Decimal("0.1")})["txid"]
        self.sync_mempools()
        self.disconnect_nodes(0, 1)
        bumped = copy.bumpshieldedfee(txid)["txid"]
        conflicting = self.bmm(node=other, expect=False)
        assert bumped in other.getblock(conflicting)["tx"]
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert txid not in side.getrawmempool()
        assert_equal(side.gettransaction(txid)["confirmations"], -1)
        assert_equal(side.gettransaction(txid)["walletconflicts"], [bumped])
        # Undone: the block's spend goes back to the mempool, and the wallet's own spend is merely inactive.
        side.invalidateblock(conflicting)
        assert bumped in side.getrawmempool()
        assert_equal(side.gettransaction(txid)["confirmations"], 0)
        assert_equal(side.gettransaction(bumped)["confirmations"], 0)
        # And back.
        side.reconsiderblock(conflicting)
        assert_equal(side.getbestblockhash(), conflicting)
        assert_equal(side.gettransaction(txid)["confirmations"], -1)
        assert_equal(side.gettransaction(bumped)["confirmations"], 1)
        other.unloadwallet("copy")
        self.check_in_sync()

    def run_too_many_notes_test(self):
        side, other = self.nodes
        self.log.info("A payment that would take more notes than a transaction the network relays can spend")
        other.createwallet("dust")
        dust = other.get_wallet_rpc("dust")
        for _ in range(3):
            side.sendshielded({dust.getshieldedaddress(True): Decimal("0.001") for _ in range(14)})
        self.mine_txs()
        assert_equal(dust.getshieldedbalance()["notes"], 42)
        assert_raises_rpc_error(-4, "too many for one transaction the network relays", dust.sendshielded, {side.getshieldedaddress(): Decimal("0.04")})
        # Less goes, in one transaction of as many notes as it takes (each note pays its own action, of about 3 kB).
        txid = dust.sendshielded({side.getshieldedaddress(): Decimal("0.005")})["txid"]
        assert_greater_than(len(dust.gettransaction(txid)["hex"]) // 2, 10 * 3000)
        self.mine_txs()
        other.unloadwallet("dust")
        self.check_in_sync()

    def run_index_off_test(self):
        side, other = self.nodes
        self.log.info("Without -orchardindex, the node has no tree for the wallet to spend with")
        self.restart_node(1, extra_args=self.extra_args[1] + ["-orchardindex=0"])
        self.connect_nodes(0, 1)
        info = other.getshieldedinfo()
        assert "index" not in info
        assert_equal(info["root"], side.getshieldedinfo()["root"])
        assert_raises_rpc_error(-4, "its -orchardindex is off", other.sendshielded, {side.getshieldedaddress(): Decimal("0.01")})
        self.restart_node(1)
        self.connect_nodes(0, 1)
        self.wait_until(lambda: other.getshieldedinfo()["index"]["synced"])
        assert_equal(other.getshieldedinfo()["index"]["root"], other.getshieldedinfo()["root"])
        other.sendshielded({side.getshieldedaddress(): Decimal("0.01")})
        self.mine_txs()
        self.check_in_sync()

    def run_index_lost_test(self):
        side, other = self.nodes
        self.log.info("An index whose best block the node does not have builds itself again")
        self.disconnect_nodes(0, 1)
        # A block only the second node has.
        other.sendshielded({other.getshieldedaddress(): Decimal("0.01")})
        self.bmm(node=other, expect=False)
        self.wait_until(lambda: other.getshieldedinfo()["index"]["synced"])
        root = side.getshieldedinfo()["root"]
        self.stop_nodes()
        index = os.path.join("indexes", "orchard")
        shutil.rmtree(side.chain_path / index)
        shutil.copytree(other.chain_path / index, side.chain_path / index)
        with side.assert_debug_log(["best block of orchardindex not found: building it again"]):
            self.start_node(0)
            self.wait_until(lambda: side.getshieldedinfo()["index"]["synced"])
        info = side.getshieldedinfo()
        assert_equal(info["root"], root)
        assert_equal(info["index"]["root"], root)
        self.start_node(1)
        self.connect_nodes(0, 1)
        self.sync_blocks()
        self.wait_until(lambda: side.getshieldedinfo()["index"]["synced"])
        assert_equal(side.getshieldedinfo()["index"]["root"], other.getshieldedinfo()["root"])
        self.check_in_sync()

    def run_reindex_test(self):
        side, other = self.nodes
        self.log.info("-reindex and -reindex-chainstate rebuild the same pool, and the wallet sees the same")
        self.mine_txs()
        tip = side.getbestblockhash()
        info = side.getshieldedinfo()
        balance = side.getshieldedbalance()
        notes = side.listshieldednotes(True)
        for option in ["-reindex", "-reindex-chainstate"]:
            self.restart_node(0, extra_args=self.extra_args[0] + [option])
            self.wait_until(lambda: side.getbestblockhash() == tip)
            self.wait_until(lambda: side.getshieldedinfo()["index"]["synced"])
            # The blocks connected again are still in the validation queue: the index rewinds to each
            # and takes it in again (-reindex-chainstate, the index ahead), so its tree is that of the tip
            # once the queue is through.
            side.syncwithvalidationinterfacequeue()
            assert_equal(side.getshieldedinfo(), info)
            assert_equal(side.getshieldedbalance(), balance)
            assert_equal(side.listshieldednotes(True), notes)
        self.restart_node(0)
        self.connect_nodes(0, 1)
        # Still spends.
        side.sendshielded({other.getshieldedaddress(): Decimal("0.01")})
        self.mine_txs()
        self.check_in_sync()


if __name__ == "__main__":
    ZSideRulesTest(__file__).main()
