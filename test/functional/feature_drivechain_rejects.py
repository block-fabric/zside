#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Every drivechain rule, broken one at a time, through the node.

- Blocks: one block per rule, built by hand and given to submitblock, which has to answer with
  exactly the reason of that rule; the chain and the sidechain database stay as they were.
- The mempool: the transactions among them, given to testmempoolaccept and sendrawtransaction,
  are refused for the same reasons; BMM requests that compete in a package are not replaced.
- The state survives a restart, -reindex-chainstate and -reindex, and a reorg that takes back
  the blocks of the setup and puts them back.

The reasons a block cannot be refused for here (another rule refuses it first) are checked by
the unit tests (drivechain_tests/every_reject_reason).
"""
from decimal import Decimal

from test_framework.blocktools import add_witness_commitment, create_block
from test_framework.drivechain import (
    VOTE_ABSTAIN,
    VOTE_DOWNVOTE,
    VOTE_TWO_BYTES,
    ack_script,
    bmm_accept_script,
    bmm_request_script,
    bundle_script,
    destination_script,
    escrow_script,
    proposal_script,
    vote_script,
)
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut
from test_framework.script import CScript, OP_RETURN, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet import MiniWallet

# Regtest drivechain parameters, see CRegTestParams.
ACTIVATION_PERIOD = 20
FEE = 10_000


class DrivechainRejectsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-fallbackfee=0.0002"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.node = self.nodes[0]
        node = self.node
        self.wallet = MiniWallet(node)
        self.generate(self.wallet, 110)
        self.generate(node, 101)

        self.log.info("Setup: sidechains in slots 1 and 2, 10 coins in each escrow, a bundle pending in slot 1")
        # This node votes for nothing: the scores stay as the setup leaves them.
        node.setdefaultwithdrawalvote("abstain")
        node.createsidechainproposal(1, "one")
        self.generate(node, 1)
        node.createsidechainproposal(2, "two")
        self.generate(node, ACTIVATION_PERIOD)
        assert_equal([s["slot"] for s in node.listactivesidechains()], [1, 2])
        node.createsidechaindeposit(1, "dest", 10)
        node.createsidechaindeposit(2, "dest", 10)
        self.generate(node, 1)
        self.blind = CTransaction()
        self.blind.vout = [CTxOut(0, CScript([OP_RETURN, (COIN // 10).to_bytes(8, "big")])), CTxOut(2 * COIN, self.wallet.get_output_script())]
        self.bundle = node.receivewithdrawalbundle(1, self.blind.serialize().hex())["hash"]
        self.generate(node, 1)
        bundles = node.listwithdrawalbundles()
        assert_equal([(b["slot"], b["hash"], b["score"]) for b in bundles], [(1, self.bundle, 1)])
        self.setup_height = node.getblockcount()

        self.test_blocks()
        self.test_mempool()
        self.test_bmm_package()
        self.test_restart_reindex_reorg()

    def escrow(self, slot):
        escrow = self.node.getsidechain(slot)["escrow"]
        return COutPoint(int(escrow["txid"], 16), escrow["n"]), int(Decimal(str(escrow["amount"])) * COIN)

    def spend(self, outputs, *, escrow_inputs=(), coin=True, which=0, fee=FEE):
        """A transaction spending a coin of the MiniWallet (the `which`th; none if coin is False) and the escrow outputs of `escrow_inputs`."""
        tx = CTransaction()
        value = 0
        if coin:
            utxos = sorted(self.wallet.get_utxos(mark_as_spent=False, confirmed_only=True), key=lambda u: (u["txid"], u["vout"]))
            utxo = utxos[which]
            tx.vin.append(CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"])))
            value += int(utxo["value"] * COIN)
        for slot in escrow_inputs:
            outpoint, amount = self.escrow(slot)
            tx.vin.append(CTxIn(outpoint))
            value += amount
        tx.vout = list(outputs)
        spent = sum(out.nValue for out in tx.vout)
        if coin and spent + fee < value:
            # The rest goes back to the MiniWallet.
            tx.vout.append(CTxOut(value - spent - fee, self.wallet.get_output_script()))
        if coin:
            self.wallet.sign_tx(tx)
            witness = tx.wit.vtxinwit[0].scriptWitness.stack
            tx.wit.vtxinwit = [CTxInWitness() for _ in tx.vin]
            tx.wit.vtxinwit[0].scriptWitness.stack = witness
        return tx

    def deposit(self, slot, amount, destination="dest"):
        """A deposit that brings the escrow of `slot` from what it holds to that plus `amount`."""
        escrow = [slot] if "escrow" in self.node.getsidechain(slot) else []
        total = (self.escrow(slot)[1] if escrow else 0) + amount
        return self.spend([CTxOut(total, escrow_script(slot)), CTxOut(0, destination_script(destination))], escrow_inputs=escrow)

    def withdrawal(self, blind, slot=1):
        """The withdrawal paying the blind bundle `blind` out of the escrow of `slot`."""
        outpoint, amount = self.escrow(slot)
        fee = int.from_bytes(blind.vout[0].scriptPubKey[2:10], "big")
        paid = sum(out.nValue for out in blind.vout)
        tx = CTransaction()
        tx.vin = [CTxIn(outpoint)]
        tx.vout = [CTxOut(amount - paid - fee, escrow_script(slot))] + blind.vout[1:]
        return tx

    def block(self, coinbase_scripts=(), txs=()):
        tmpl = self.node.getblocktemplate({"rules": ["segwit"]})
        block = create_block(tmpl=tmpl, txlist=list(txs))
        block.vtx[0].vout[0].nValue = tmpl["coinbasevalue"] + FEE * len(txs)
        for script in coinbase_scripts:
            block.vtx[0].vout.append(CTxOut(0, script))
        add_witness_commitment(block)
        block.solve()
        return block

    def test_blocks(self):
        self.log.info("submitblock refuses a block breaking each drivechain rule, for that rule")
        node = self.node
        tip = node.getbestblockhash()
        state = node.getdrivechaininfo()["statehash"]
        side = "5b" * 32
        abstain_two = vote_script([VOTE_ABSTAIN, VOTE_ABSTAIN])

        def bmm(slot, prev, which=0):
            return self.spend([CTxOut(0, bmm_request_script(slot, side, prev))], which=which)

        other_blind = CTransaction()
        other_blind.vout = [CTxOut(0, CScript([OP_RETURN, (COIN // 10).to_bytes(8, "big")])), CTxOut(COIN, CScript([OP_TRUE, OP_TRUE]))]

        cases = [
            ("bad-dc-coinbase-escrow", [escrow_script(1)], []),
            ("bad-dc-duplicate-proposal", [proposal_script(3, "three")] * 2, []),
            ("bad-dc-multiple-acks", [ack_script(3, "01" * 32), ack_script(3, "02" * 32)], []),
            ("bad-dc-multiple-bundles", [bundle_script(2, "01" * 32), bundle_script(2, "02" * 32)], []),
            ("bad-dc-multiple-votes", [abstain_two, abstain_two], []),
            ("bad-dc-bmm-inactive-sidechain", [bmm_accept_script(3, side)], []),
            ("bad-dc-multiple-bmm-accepts", [bmm_accept_script(1, side), bmm_accept_script(1, "01" * 32)], []),
            ("bad-dc-bmm-prev-block", [bmm_accept_script(1, side)], [bmm(1, "77" * 32)]),
            ("bad-dc-bmm-not-accepted", [], [bmm(1, tip)]),
            ("bad-dc-bmm-not-accepted", [bmm_accept_script(1, "01" * 32)], [bmm(1, tip)]),
            ("bad-dc-multiple-bmm-requests", [bmm_accept_script(1, side)], [bmm(1, tip, 0), bmm(1, tip, 1)]),
            ("bad-dc-multiple-escrow-outputs", [], [self.spend([CTxOut(COIN, escrow_script(3)), CTxOut(COIN, escrow_script(4))])]),
            ("bad-dc-multiple-escrow-inputs", [], [self.spend([CTxOut(21 * COIN, escrow_script(1)), CTxOut(0, destination_script("dest"))], escrow_inputs=[1, 2])]),
            ("bad-dc-escrow-spend", [], [self.spend([CTxOut(10 * COIN - FEE, CScript([OP_TRUE]))], escrow_inputs=[1], coin=False)]),
            ("bad-dc-inactive-sidechain", [], [self.spend([CTxOut(COIN, escrow_script(3)), CTxOut(0, destination_script("dest"))])]),
            ("bad-dc-escrow-mismatch", [], [self.spend([CTxOut(20 * COIN, escrow_script(2)), CTxOut(0, destination_script("dest"))], escrow_inputs=[1])]),
            ("bad-dc-escrow-unspent", [], [self.spend([CTxOut(20 * COIN, escrow_script(1)), CTxOut(0, destination_script("dest"))])]),
            ("bad-dc-escrow-amount", [], [self.deposit(1, 0)]),
            ("bad-dc-deposit-destination", [], [self.spend([CTxOut(20 * COIN, escrow_script(1))], escrow_inputs=[1])]),
            ("bad-dc-deposit-destination", [], [self.deposit(1, COIN, "")]),
            ("bad-dc-deposit-destination", [], [self.deposit(1, COIN, "x" * 101)]),
            ("bad-dc-deposit-destination", [], [self.deposit(1, COIN, "D")]),
            ("bad-dc-withdrawal-inputs", [], [self.spend([CTxOut(7 * COIN, escrow_script(1)), CTxOut(2 * COIN, CScript([OP_TRUE]))], escrow_inputs=[1])]),
            ("bad-dc-withdrawal-outputs", [], [self.spend([CTxOut(9 * COIN, escrow_script(1))], escrow_inputs=[1], coin=False)]),
            ("bad-dc-withdrawal-outputs", [], [self.spend([CTxOut(2 * COIN, CScript([OP_TRUE])), CTxOut(7 * COIN, escrow_script(1))], escrow_inputs=[1], coin=False)]),
            ("bad-dc-withdrawal-unknown", [], [self.withdrawal(other_blind)]),
            ("bad-dc-withdrawal-score", [], [self.withdrawal(self.blind)]),
            ("bad-dc-votes-size", [vote_script([0])], []),
            ("bad-dc-votes-size", [vote_script([0, VOTE_ABSTAIN, VOTE_ABSTAIN])], []),
            ("bad-dc-votes-form", [vote_script([0, 0], form=VOTE_TWO_BYTES)], []),
            ("bad-dc-votes-index", [vote_script([1, VOTE_ABSTAIN])], []),
            ("bad-dc-votes-index", [vote_script([VOTE_ABSTAIN, 0])], []),
            ("bad-dc-bundle-known", [bundle_script(1, self.bundle)], []),
        ]
        covered = set()
        for reason, scripts, txs in cases:
            block = self.block(scripts, txs)
            assert_equal(node.submitblock(block.serialize().hex()), reason)
            assert_equal(node.getbestblockhash(), tip)
            assert_equal(node.getdrivechaininfo()["statehash"], state)
            covered.add(reason)

        self.log.info("A bundle that failed cannot be proposed again; a full queue takes no bundle that is voted for")
        # The bundle downvoted to 0 fails once it can no longer reach the score (here, with the
        # blocks of this node, which abstain).
        assert_equal(node.submitblock(self.block([vote_script([VOTE_DOWNVOTE, VOTE_ABSTAIN])]).serialize().hex()), None)
        for _ in range(60):
            if not node.listwithdrawalbundles():
                break
            self.generate(node, 1)
        assert_equal(node.listwithdrawalbundles(), [])
        assert_equal(node.submitblock(self.block([bundle_script(1, self.bundle)]).serialize().hex()), "bad-dc-bundle-closed")
        covered.add("bad-dc-bundle-closed")
        # The default queue holds 64 bundles: with -testdrivechainparam it is restarted with a queue of one.
        self.restart_node(0, extra_args=self.extra_args[0] + ["-testdrivechainparam=max_pending_bundles@1"])
        assert_equal(node.submitblock(self.block([bundle_script(2, "0a" * 32)]).serialize().hex()), None)
        # An upvote takes it above a new bundle's score: it is not pushed out for another.
        assert_equal(node.submitblock(self.block([vote_script([VOTE_ABSTAIN, 0])]).serialize().hex()), None)
        assert_equal(node.submitblock(self.block([bundle_script(2, "0b" * 32)]).serialize().hex()), "bad-dc-too-many-bundles")
        covered.add("bad-dc-too-many-bundles")
        # Downvoted to a new bundle's score, it is.
        assert_equal(node.submitblock(self.block([vote_script([VOTE_ABSTAIN, VOTE_DOWNVOTE])]).serialize().hex()), None)
        assert_equal(node.submitblock(self.block([bundle_script(2, "0b" * 32)]).serialize().hex()), None)
        assert_equal([b["hash"] for b in node.listwithdrawalbundles(2)], ["0b" * 32])
        # Back to the parameters of the setup: the blocks are judged again (which forgets what
        # invalidateblock did, too), and the chain is taken back to the setup.
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex-chainstate"])
        self.invalidate_back_to_setup()

        # bad-dc-no-coinbase: refused before (bad-cb-missing). bad-dc-withdrawal-amount: a withdrawal
        # paying more than the escrow spends more than its inputs (bad-txns-in-belowout). An escrow
        # amount out of range: bad-txns-vout-toolarge.
        assert_equal(len(covered), 28)
        assert_equal(node.submitblock(self.block([], [self.spend([CTxOut(9 * COIN, escrow_script(1)), CTxOut(2 * COIN, CScript([OP_TRUE]))], escrow_inputs=[1], coin=False)]).serialize().hex()), "bad-txns-in-belowout")

    def invalidate_back_to_setup(self):
        node = self.node
        if node.getblockcount() > self.setup_height:
            node.invalidateblock(node.getblockhash(self.setup_height + 1))
        assert_equal(node.getblockcount(), self.setup_height)

    def test_mempool(self):
        self.log.info("The mempool refuses the transactions breaking a drivechain rule, for that rule")
        node = self.node
        assert_equal(node.getblockcount(), self.setup_height)
        cases = [
            ("bad-dc-multiple-escrow-outputs", self.spend([CTxOut(COIN, escrow_script(1)), CTxOut(COIN, escrow_script(2))])),
            ("bad-dc-multiple-escrow-inputs", self.spend([CTxOut(21 * COIN, escrow_script(1)), CTxOut(0, destination_script("dest"))], escrow_inputs=[1, 2])),
            ("bad-dc-escrow-spend", self.spend([CTxOut(10 * COIN - FEE, self.wallet.get_output_script())], escrow_inputs=[1])),
            ("bad-dc-inactive-sidechain", self.spend([CTxOut(COIN, escrow_script(3)), CTxOut(0, destination_script("dest"))])),
            ("bad-dc-escrow-mismatch", self.spend([CTxOut(20 * COIN, escrow_script(2)), CTxOut(0, destination_script("dest"))], escrow_inputs=[1])),
            ("bad-dc-escrow-unspent", self.spend([CTxOut(20 * COIN, escrow_script(1)), CTxOut(0, destination_script("dest"))])),
            ("bad-dc-escrow-amount", self.deposit(1, 0)),
            ("bad-dc-withdrawal-inputs", self.spend([CTxOut(7 * COIN, escrow_script(1)), CTxOut(2 * COIN, self.wallet.get_output_script())], escrow_inputs=[1])),
            ("bad-dc-withdrawal-score", self.withdrawal(self.blind)),
        ]
        other_blind = CTransaction()
        other_blind.vout = [CTxOut(0, CScript([OP_RETURN, (COIN // 10).to_bytes(8, "big")])), CTxOut(COIN, self.wallet.get_output_script())]
        cases.append(("bad-dc-withdrawal-unknown", self.withdrawal(other_blind)))
        for destination in ["", "x" * 101, "D"]:
            cases.append(("bad-dc-deposit-destination", self.deposit(1, COIN, destination)))
        for reason, tx in cases:
            result = node.testmempoolaccept([tx.serialize().hex()])[0]
            assert_equal(result["allowed"], False)
            assert_equal(result["reject-reason"], reason)
            assert_raises_rpc_error(-26, reason, node.sendrawtransaction, tx.serialize().hex())
        assert_equal(node.getrawmempool(), [])

        self.log.info("A treasury input carries nothing; a BMM request is for an active sidechain and the next block")
        padded = self.deposit(1, COIN)
        padded.vin[1].scriptSig = CScript([OP_TRUE])
        assert_equal(node.testmempoolaccept([padded.serialize().hex()])[0]["reject-reason"], "dc-escrow-input-not-empty")
        tip = node.getbestblockhash()
        for reason, tx in [("dc-bmm-inactive-sidechain", self.spend([CTxOut(0, bmm_request_script(3, "11" * 32, tip))])),
                           ("dc-bmm-prev-block", self.spend([CTxOut(0, bmm_request_script(1, "11" * 32, "22" * 32))]))]:
            assert_equal(node.testmempoolaccept([tx.serialize().hex()])[0]["reject-reason"], reason)
        # A valid deposit, for comparison.
        good = self.deposit(1, COIN)
        assert_equal(node.testmempoolaccept([good.serialize().hex()])[0]["allowed"], True)

    def test_bmm_package(self):
        self.log.info("BMM requests compete one at a time: not in packages, and a better one replaces the other")
        node = self.node
        tip = node.getbestblockhash()
        first = self.spend([CTxOut(0, bmm_request_script(1, "11" * 32, tip))], which=2, fee=2_000)
        node.sendrawtransaction(first.serialize().hex())
        rival = self.spend([CTxOut(0, bmm_request_script(1, "22" * 32, tip))], which=0, fee=10_000)
        other = self.spend([], which=1)
        # (So the replacement rules of package validation, which refuse replacements, never see them.)
        results = node.testmempoolaccept([other.serialize().hex(), rival.serialize().hex()])
        assert_equal([r["package-error"] for r in results], ["package-drivechain-tx"] * 2)
        # On its own it replaces the first (it pays more); one that pays less replaces neither.
        assert_equal(node.testmempoolaccept([rival.serialize().hex()])[0]["allowed"], True)
        node.sendrawtransaction(rival.serialize().hex())
        assert_equal(node.getrawmempool(), [rival.txid_hex])
        low = self.spend([CTxOut(0, bmm_request_script(1, "33" * 32, tip))], which=1, fee=5_000)
        assert_raises_rpc_error(-26, "insufficient fee", node.sendrawtransaction, low.serialize().hex())
        assert_equal(node.getrawmempool(), [rival.txid_hex])
        self.generate(node, 1)
        self.wallet.rescan_utxos()

    def test_restart_reindex_reorg(self):
        self.log.info("The sidechain database survives a restart, a reindex, and a reorg back and forth")
        node = self.node
        # (Longer than the branches invalidateblock left behind, which -reindex forgets.)
        self.generate(node, 40)
        state = node.getdrivechaininfo()["statehash"]
        tip = node.getbestblockhash()
        height = node.getblockcount()
        for args in ([], ["-reindex-chainstate"], ["-reindex"]):
            self.restart_node(0, extra_args=self.extra_args[0] + args)
            self.wait_until(lambda: node.getblockcount() == height)
            assert_equal(node.getbestblockhash(), tip)
            assert_equal(node.getdrivechaininfo()["statehash"], state)
        # Back before the sidechains activated: nothing is active; and forward again.
        first = node.getblockhash(self.setup_height - ACTIVATION_PERIOD - 5)
        node.invalidateblock(first)
        assert_equal(node.listactivesidechains(), [])
        node.reconsiderblock(first)
        assert_equal(node.getblockcount(), height)
        assert_equal(node.getdrivechaininfo()["statehash"], state)


if __name__ == "__main__":
    DrivechainRejectsTest(__file__).main()
