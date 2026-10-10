#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The sidechain rules as a node applies them to what it is sent: blocks and transactions.

The mainchain node is a stand-in (test_framework/sidechain_standin.py), so that the test can have the
mainchain commit to any block, record any deposit, close or leave pending any bundle. Every block is
built by the node (generateblock, without submitting it), then changed by the test where it breaks a
rule, committed to by the stand-in, and submitted: submitblock answers with the reason it was refused
for. Transactions are tried with testmempoolaccept.

Also the RPCs a sidechain node has or changes: those of mining (getblocktemplate, submitheader,
setgenerate, getgenerate, getmininginfo), proofs of transactions, and what a node that is no sidechain
answers to the sidechain RPCs.
"""

import base64
import copy
from decimal import Decimal

from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import COIN, CBlock, CTransaction, CTxOut, from_hex, tx_from_hex
from test_framework.script import CScript, OP_RETURN
from test_framework.sidechain_standin import StandInMainchain
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, rpc_port

SLOT = 3
WITHDRAWAL_TAG = bytes.fromhex("d8574452")
REFUND_TAG = bytes.fromhex("d952464e")
BUNDLE_COMMIT_TAG = bytes.fromhex("da424e44")
# Regtest parameters of this chain as a sidechain.
MIN_WITHDRAWAL = 10000
PENDING_MIN_SCORE = 3
MAIN_SCRIPT = bytes.fromhex("0014" + "11" * 20)


def withdrawal_data(fee, keyhash=b"\x22" * 20, script=MAIN_SCRIPT):
    return (WITHDRAWAL_TAG + fee.to_bytes(8, "little") + keyhash + script).hex()


def bundle_commit(bundle_hash):
    return CScript([OP_RETURN, BUNDLE_COMMIT_TAG + bytes.fromhex(bundle_hash)[::-1]])


class SidechainRulesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        # Node 1 is no sidechain.
        self.extra_args = [[
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=my wallet",
        ], []]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.main = StandInMainchain(SLOT)
        self.main.serve(self.main_rpc_port)
        # Not connected: they are not the same chain.
        self.setup_nodes()

    def run_test(self):
        try:
            self.run_rules_test()
        finally:
            self.main.shutdown()

    #
    # Helpers
    #

    def commit(self, block_hash):
        """The next mainchain block commits to the block, and the node learns of it."""
        self.main.add_block(bmm=block_hash)
        self.node.syncmainchain()

    def mine(self):
        """A block the node builds itself, from its mempool, committed to."""
        candidate = self.node.createbmmblock(self.address)
        self.commit(candidate["hash"])
        assert_equal(self.node.getbestblockhash(), candidate["hash"])
        return candidate["hash"]

    def deposit(self, amount, destination=None):
        self.main.add_block(deposits=[{"destination": destination or self.address, "amount": str(amount), "txid": "ab" * 32, "burnindex": 0}])
        self.node.syncmainchain()

    def template(self):
        """The block the node would make now, for the next mainchain block to commit to."""
        return from_hex(CBlock(), self.node.generateblock(self.address, [], False, called_by_framework=True)["hex"])

    @staticmethod
    def finish(block, txs=()):
        block.vtx.extend(txs)
        if txs:
            add_witness_commitment(block)
        block.hashMerkleRoot = block.calc_merkle_root()
        block.solve()
        return block

    def submit(self, block, commit=True):
        if commit:
            self.commit(block.hash_hex)
        return self.node.submitblock(block.serialize().hex())

    def refused(self, block, reason, txs=()):
        """Submit the block, with the transactions, and check that it is refused for the reason."""
        self.finish(block, txs)
        assert_equal(self.submit(block), reason)
        assert block.hash_hex != self.node.getbestblockhash()
        return block

    def data_tx(self, data, value=0):
        """A transaction of the wallet with an OP_RETURN output pushing `data`, of `value`."""
        tx = CTransaction()
        tx.vout = [CTxOut(value, CScript([OP_RETURN, bytes.fromhex(data)]))]
        funded = self.node.fundrawtransaction(tx.serialize_without_witness().hex(), {"minconf": 1})["hex"]
        signed = self.node.signrawtransactionwithwallet(funded)
        assert signed["complete"]
        return tx_from_hex(signed["hex"])

    def refund_tx(self, txid, vout, signer):
        signature = base64.b64decode(self.node.signmessage(signer, f"Refund withdrawal {txid}:{vout}"))
        return self.data_tx((REFUND_TAG + bytes.fromhex(txid)[::-1] + vout.to_bytes(4, "little") + signature).hex())

    def reject_reason(self, tx):
        result = self.node.testmempoolaccept([tx.serialize().hex()])[0]
        assert not result["allowed"]
        return result["reject-reason"]

    def accepted(self, tx):
        return self.node.testmempoolaccept([tx.serialize().hex()])[0]["allowed"]

    #
    # The test
    #

    def run_rules_test(self):
        self.node = node = self.nodes[0]
        plain = self.nodes[1]
        main = self.main
        self.address = node.getnewaddress()

        self.log.info("Without mainchain blocks on record, no block can be built")
        assert_equal(node.syncmainchain(), -1)
        info = node.getmainchaininfo()
        assert_equal((info["height"], "bestblockhash" in info), (-1, False))
        assert_raises_rpc_error(-1, "The mainchain has no blocks on record yet", node.createbmmblock, self.address)
        for _ in range(3):
            main.add_block()
        assert_equal(node.syncmainchain(), 2)

        self.log.info("A node that is no sidechain has no mainchain")
        assert_raises_rpc_error(-1, "This chain is not a sidechain", plain.getmainchaininfo)
        assert_raises_rpc_error(-1, "This chain is not running as a sidechain", plain.getdepositaddress)
        assert_raises_rpc_error(-5, "This chain is not a sidechain", plain.createwithdrawal, "mainaddress", 1)

        self.log.info("The CPU miner is no way to mine a sidechain")
        assert_raises_rpc_error(-1, "use setbmm instead", node.setgenerate, True, self.address)
        assert_equal(node.getgenerate()["generate"], False)
        assert_equal(node.getmininginfo()["blocks"], 0)

        self.log.info("A deposit: the template's coinbase pays it, right after its first output")
        self.deposit(50)
        gbt = node.getblocktemplate({"rules": ["segwit"], "capabilities": ["coinbasetxn"]})
        assert_equal(gbt["height"], 1)
        coinbase = tx_from_hex(gbt["coinbasetxn"]["data"])
        assert_equal(coinbase.vout[1].nValue, 50 * COIN)
        assert_equal(coinbase.vout[1].scriptPubKey.hex(), node.getaddressinfo(self.address)["scriptPubKey"])

        self.log.info("A header, and a block, need a commitment of the mainchain")
        block = self.finish(self.template())
        header = block.serialize()[:80].hex()
        assert_raises_rpc_error(-25, "bmm-unknown", node.submitheader, header)
        assert_equal(node.submitblock(block.serialize().hex()), "bmm-unknown")
        assert_equal(node.getblockcount(), 0)
        self.commit(block.hash_hex)
        assert_equal(node.submitheader(header), None)
        assert_equal(node.submitblock(block.serialize().hex()), None)
        assert_equal(node.getbestblockhash(), block.hash_hex)
        assert_equal(node.getbalance(), 50)

        self.log.info("Deposits the coinbase does not pay as it must")
        self.deposit(3)
        base = self.template()
        assert_equal(base.vtx[0].vout[1].nValue, 3 * COIN)
        # Another output (the witness commitment) where the payout goes.
        misplaced = copy.deepcopy(base)
        del misplaced.vtx[0].vout[1]
        self.refused(misplaced, "bad-sc-payout")
        # No payout at all.
        unpaid = copy.deepcopy(base)
        unpaid.vtx[0].vout = unpaid.vtx[0].vout[:1]
        unpaid.vtx[0].wit.vtxinwit = []
        self.refused(unpaid, "bad-sc-payouts-missing")
        # The payouts are what the coinbase creates on top of the fees, and no more.
        greedy = copy.deepcopy(base)
        greedy.vtx[0].vout[0].nValue += 1
        self.refused(greedy, "bad-cb-amount")
        # A failure against the record is remembered as such.
        tips = {tip["hash"]: tip["status"] for tip in node.getchaintips()}
        assert_equal([tips[b.hash_hex] for b in (misplaced, unpaid, greedy)], ["invalid"] * 3)
        self.mine()
        assert_equal(node.getbalance(), 53)

        self.log.info("Commitments in the order of the blocks")
        first = self.finish(self.template())
        second = create_block(hashprev=first.hash_int, coinbase=create_coinbase(node.getblockcount() + 2, script_pubkey=CScript([OP_RETURN]), nValue=0), ntime=first.nTime + 1)
        second.solve()
        main.add_block(bmm=second.hash_hex)
        main.add_block(bmm=first.hash_hex)
        node.syncmainchain()
        assert_equal(node.submitblock(first.serialize().hex()), None)
        assert_equal(node.getbestblockhash(), first.hash_hex)
        assert_equal(node.submitblock(second.serialize().hex()), "bmm-order")

        self.log.info("Withdrawals: the form of the output")
        assert_raises_rpc_error(-5, "Not an address of the mainchain", node.createwithdrawal, "nonsense", 1)
        assert_raises_rpc_error(-5, "The mainchain node sent a malformed script", node.createwithdrawal, "malformed", 1)
        main.errors["validateaddress"] = "no such wallet"
        assert_raises_rpc_error(-5, "The mainchain node answered validateaddress with an error: no such wallet", node.createwithdrawal, "mainaddress", 1)
        del main.errors["validateaddress"]
        # Its fee is all of its value: no withdrawal.
        malformed = self.data_tx(withdrawal_data(COIN), COIN)
        assert_equal(self.reject_reason(malformed), "bad-sc-withdrawal")
        assert_raises_rpc_error(-25, "TestBlockValidity failed: bad-sc-withdrawal", node.generateblock, self.address, [malformed.serialize().hex()], called_by_framework=True)
        self.refused(self.template(), "bad-sc-withdrawal", [malformed])
        # Below the least amount that can be withdrawn.
        small = self.data_tx(withdrawal_data(1000), MIN_WITHDRAWAL - 1 + 1000)
        assert_equal(self.reject_reason(small), "bad-sc-withdrawal-amount")
        self.refused(self.template(), "bad-sc-withdrawal-amount", [small])
        # At the least amount, it goes.
        assert self.accepted(self.data_tx(withdrawal_data(1000), MIN_WITHDRAWAL + 1000))

        self.log.info("Bundles: one, of the withdrawals the rules pick, not closed, not while one is pending")
        withdrawal = node.createwithdrawal("mainaddress", 1)
        self.mine()
        bundle = node.getwithdrawalbundle()
        assert_equal(bundle["status"], "next")
        base = self.template()
        commitment = [i for i, out in enumerate(base.vtx[0].vout) if out.scriptPubKey == bundle_commit(bundle["hash"])]
        assert_equal(len(commitment), 1)
        other_hash = copy.deepcopy(base)
        other_hash.vtx[0].vout[commitment[0]] = CTxOut(0, bundle_commit("77" * 32))
        self.refused(other_hash, "bad-sc-bundle-hash")
        twice = copy.deepcopy(base)
        twice.vtx[0].vout.insert(commitment[0], CTxOut(0, bundle_commit(bundle["hash"])))
        self.refused(twice, "bad-sc-bundle-multiple")
        # The mainchain closed it before the commitment (someone worked its hash out ahead): the node
        # makes no block that commits to it, and takes none.
        main.add_block(bundles=[{"hash": bundle["hash"], "paid": False}])
        node.syncmainchain()
        closed = self.template()
        assert not any(out.scriptPubKey == bundle_commit(bundle["hash"]) for out in closed.vtx[0].vout)
        closed.vtx[0].vout.insert(1, CTxOut(0, bundle_commit(bundle["hash"])))
        self.refused(closed, "bad-sc-bundle-closed")
        self.mine()
        self.mine()
        pending = node.getwithdrawalbundle()
        assert_equal(pending["status"], "pending")
        assert pending["hash"] != bundle["hash"]
        another = self.template()
        another.vtx[0].vout.insert(1, CTxOut(0, bundle_commit(pending["hash"])))
        self.refused(another, "bad-sc-bundle-not-allowed")

        self.log.info("Refunds: of a withdrawal there is, signed by its key, not in the bundle, not while another is pending")
        waiting = node.createwithdrawal("mainaddress", 2)
        self.mine()
        statuses = {w["txid"]: w["status"] for w in node.listwithdrawals()}
        assert_equal((statuses[withdrawal["txid"]], statuses[waiting["txid"]]), ("bundled", "waiting"))
        # Of a transaction output that is no withdrawal.
        unknown = self.refund_tx(waiting["txid"], 1 - waiting["vout"], waiting["refundaddress"])
        assert_equal(self.reject_reason(unknown), "bad-sc-refund-unknown")
        self.refused(self.template(), "bad-sc-refund-unknown", [unknown])
        # Signed by another key.
        forged = self.refund_tx(waiting["txid"], waiting["vout"], node.getnewaddress(address_type="bech32"))
        assert_equal(self.reject_reason(forged), "bad-sc-refund-signature")
        self.refused(self.template(), "bad-sc-refund-signature", [forged])
        # Of a withdrawal in the pending bundle.
        bundled = self.refund_tx(withdrawal["txid"], withdrawal["vout"], withdrawal["refundaddress"])
        assert_equal(self.reject_reason(bundled), "bad-sc-refund-in-bundle")
        self.refused(self.template(), "bad-sc-refund-in-bundle", [bundled])
        # A good one, in a block that does not pay it: the witness commitment where the payout goes.
        refund = self.refund_tx(waiting["txid"], waiting["vout"], waiting["refundaddress"])
        assert self.accepted(refund)
        self.refused(self.template(), "bad-sc-payout", [refund])
        # One refund of a withdrawal in the mempool at a time.
        node.sendrawtransaction(refund.serialize().hex())
        again = self.refund_tx(waiting["txid"], waiting["vout"], waiting["refundaddress"])
        assert again.vin[0].prevout != refund.vin[0].prevout
        assert_equal(self.reject_reason(again), "sc-refund-in-mempool")
        assert_raises_rpc_error(-4, "The refund was not accepted into the mempool", node.refundwithdrawal, waiting["txid"], waiting["vout"])

        self.log.info("A bundle of another branch, pending on the mainchain with support, holds refunds back")
        foreign = "f0" * 32
        main.add_block(proposed=[foreign], pending=[{"hash": foreign, "score": PENDING_MIN_SCORE}])
        node.syncmainchain()
        assert_equal(self.reject_reason(again), "bad-sc-refund-bundle-pending")
        self.refused(self.template(), "bad-sc-refund-bundle-pending", [again])
        # The refund in the mempool stays out of the next block, and out of the mempool after it.
        assert refund.txid_hex in node.getrawmempool()
        tip = self.mine()
        assert refund.txid_hex not in node.getblock(tip)["tx"]
        assert refund.txid_hex not in node.getrawmempool()
        # Downvoted below the score, it no longer holds them back once a block has acted on that: the
        # tip acted on the mainchain as it was before, which the next block's refunds also go by.
        main.add_block(pending=[{"hash": foreign, "score": 0}])
        node.syncmainchain()
        assert_equal(self.reject_reason(again), "bad-sc-refund-bundle-pending")
        self.mine()
        assert self.accepted(again)
        balance = node.getbalance()
        taken = node.refundwithdrawal(waiting["txid"], waiting["vout"])
        assert_equal(taken["amount"], Decimal("2.0001"))
        tip = self.mine()
        assert taken["txid"] in node.getblock(tip)["tx"]
        assert_equal(node.getblock(tip, 2)["tx"][0]["vout"][1]["value"], Decimal("2.0001"))
        assert node.getbalance() > balance + 2
        assert_equal([w["txid"] for w in node.listwithdrawals()], [withdrawal["txid"]])

        self.log.info("Proofs of transactions of the sidechain")
        proof = node.gettxoutproof([taken["txid"]], tip)
        assert_equal(node.verifytxoutproof(proof), [taken["txid"]])

        self.log.info("Payouts add up to what can exist: more is refused before the sidechain rules, by the coinbase's own")
        self.deposit(Decimal("20999999"))
        self.deposit(Decimal("20999999"))
        assert_raises_rpc_error(-1, "bad-txns-txouttotal-toolarge", node.generateblock, self.address, [], called_by_framework=True)


if __name__ == '__main__':
    SidechainRulesTest(__file__).main()
