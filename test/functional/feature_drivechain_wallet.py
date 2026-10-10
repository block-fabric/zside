#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The drivechain commands of the wallet and of the miner where things do not go to plan.

- createbmmrequest: a request already in the mempool for the same sidechain block is returned as
  it is; one outbid only once the mempool refused it says so; a wallet whose only coins are the
  change of a request pays the next one with it.
- createsidechaindeposit: a wallet that cannot sign, a node whose mempool refuses the deposit, and
  a deposit after an unconfirmed one.
- vouchwithdrawalbundle: what the sidechain node says its chain has decides the vote.
- A miner state file that cannot be read is reset at startup, with a warning.
"""
from decimal import Decimal

from test_framework.messages import COIN, CTransaction, CTxOut
from test_framework.script import CScript, OP_RETURN
from test_framework.address import address_to_scriptpubkey
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

# Regtest drivechain parameters, see CRegTestParams.
ACTIVATION_PERIOD = 20


class DrivechainWalletTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            ["-fallbackfee=0.0002"],
            # Relays no data: deposits and BMM requests carry some.
            ["-fallbackfee=0.0002", "-datacarrier=0"],
            # A low fee rate, at which the wallet spends many small coins rather than one large one.
            ["-fallbackfee=0.00001"],
        ]

    def setup_network(self):
        # Node 1 relays no deposits: node 2 hears of them from node 0.
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.sync_all()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        node.createsidechainproposal(1, "one")
        self.generate(node, 1)
        node.createsidechainproposal(2, "two")
        self.generate(node, ACTIVATION_PERIOD)
        assert_equal([s["slot"] for s in node.listactivesidechains()], [1, 2])
        for other in self.nodes[1:]:
            node.sendtoaddress(other.getnewaddress(), 10)
        self.generate(node, 1)

        self.test_bmm_request_already_there()
        self.test_bmm_request_outbid()
        self.test_bmm_request_from_change()
        self.test_deposit_cannot_sign()
        self.test_deposit_refused()
        self.test_deposit_after_unconfirmed()
        self.test_vouch()
        self.test_unreadable_miner_state()

    def test_bmm_request_already_there(self):
        self.log.info("A request for the same sidechain block as one in the mempool is that one")
        node = self.nodes[0]
        first = node.createbmmrequest(1, "aa" * 32, Decimal("0.001"))
        assert "existing" not in first
        # The fee is the one offered, but for the rounding of the fee rate.
        assert abs(first["fee"] - Decimal("0.001")) <= Decimal("0.00000100"), first["fee"]
        again = node.createbmmrequest(1, "aa" * 32, Decimal("0.002"))
        assert_equal(again, {**first, "existing": True})
        assert_equal(node.getrawmempool(), [first["txid"]])
        # For the next block it is another request.
        self.generate(node, 1)
        later = node.createbmmrequest(1, "aa" * 32, Decimal("0.001"))
        assert "existing" not in later
        assert later["txid"] != first["txid"]
        self.generate(node, 1)

    def test_bmm_request_outbid(self):
        self.log.info("A request that pays more than the one in the mempool, but not enough to replace it, is outbid")
        node = self.nodes[0]
        first = node.createbmmrequest(1, "aa" * 32, Decimal("0.001"))
        assert_raises_rpc_error(-26, "Outbid: the mempool holds a request for another block of this sidechain paying",
                                node.createbmmrequest, 1, "bb" * 32, first["fee"])
        assert_raises_rpc_error(-26, "; a replacement has to pay more than that and the relay fee",
                                node.createbmmrequest, 1, "bb" * 32, first["fee"] + Decimal("0.00000001"))
        assert_equal(node.getrawmempool(), [first["txid"]])
        # Neither left anything behind in the wallet.
        assert_equal([tx["txid"] for tx in node.listtransactions("*", 1000) if tx["confirmations"] == 0 and not tx.get("abandoned")], [first["txid"]])
        better = node.createbmmrequest(1, "bb" * 32, Decimal("0.002"))
        assert_equal(node.getrawmempool(), [better["txid"]])
        self.generate(node, 1)

    def test_bmm_request_from_change(self):
        self.log.info("A wallet whose only coins are the unconfirmed change of a request pays the next one with it")
        node = self.nodes[0]
        node.createwallet("requests")
        wallet = node.get_wallet_rpc("requests")
        node.get_wallet_rpc(self.default_wallet_name).sendtoaddress(wallet.getnewaddress(), 1)
        self.generate(node, 1)
        first = wallet.createbmmrequest(1, "cc" * 32, Decimal("0.0001"))
        second = wallet.createbmmrequest(2, "dd" * 32, Decimal("0.0001"))
        spent = [vin["txid"] for vin in node.getrawtransaction(second["txid"], True)["vin"]]
        assert_equal(spent, [first["txid"]])
        assert_equal(set(node.getrawmempool()), {first["txid"], second["txid"]})

        self.log.info("...but not to outbid the request whose change it is")
        # Its only coin is the change of the request for slot 2: a request for slot 2 that spends
        # it would replace its own parent.
        assert_raises_rpc_error(-6, "The total exceeds your balance", wallet.createbmmrequest, 2, "ee" * 32, Decimal("0.001"))
        assert_equal(set(node.getrawmempool()), {first["txid"], second["txid"]})
        self.generate(node, 1)
        assert_equal(node.getrawmempool(), [])

    def test_deposit_cannot_sign(self):
        self.log.info("A wallet without the keys of its coins cannot deposit")
        node = self.nodes[0]
        default = node.get_wallet_rpc(self.default_wallet_name)
        node.createwallet("watch", disable_private_keys=True)
        watch = node.get_wallet_rpc("watch")
        # The public descriptors of a wallet with coins (and change addresses).
        node.createwallet("keys")
        keys = node.get_wallet_rpc("keys")
        imports = [{"desc": d["desc"], "timestamp": "now", "active": d["active"], "internal": d.get("internal", False), "range": d.get("range", 0)}
                   for d in keys.listdescriptors()["descriptors"]]
        assert all(r["success"] for r in watch.importdescriptors(imports))
        default.sendtoaddress(keys.getnewaddress(), 5)
        self.generate(node, 1)
        assert_equal(watch.getbalance(), 5)
        assert_raises_rpc_error(-4, "Signing failed", watch.createsidechaindeposit, 1, "dest", 1)
        assert_equal(node.getrawmempool(), [])

    def test_deposit_refused(self):
        self.log.info("A deposit the mempool of the node refuses is not kept")
        node = self.nodes[1]
        balance = node.getbalance()
        assert_raises_rpc_error(-4, "The transaction was not accepted into the mempool", node.createsidechaindeposit, 1, "dest", 1)
        assert_raises_rpc_error(-4, "The request was not accepted into the mempool (see the debug log for why)", node.createbmmrequest, 1, "aa" * 32, Decimal("0.001"))
        assert_equal(node.getrawmempool(), [])
        # The coins are back: abandoned, the transactions spend nothing.
        assert_equal(node.getbalance(), balance)

    def test_deposit_after_unconfirmed(self):
        self.log.info("A deposit after an unconfirmed one is small, whatever coins the wallet has")
        miner = self.nodes[0]
        node = self.nodes[2]
        default = miner.get_wallet_rpc(self.default_wallet_name)
        node.createwallet("small")
        small = node.get_wallet_rpc("small")
        large = node.get_wallet_rpc(self.default_wallet_name)
        # Forty small coins in each wallet; the default one also has a large coin.
        default.sendmany("", {small.getnewaddress(): Decimal("0.001") for _ in range(40)})
        default.sendmany("", {large.getnewaddress(): Decimal("0.001") for _ in range(40)})
        self.generate(miner, 1)
        default.createsidechaindeposit(1, "first", 1)
        self.sync_mempools([miner, node])
        deposit = large.createsidechaindeposit(1, "second", Decimal("0.03"))
        self.sync_mempools([miner, node])
        assert deposit["txid"] in miner.getrawmempool()
        assert node.getrawtransaction(deposit["txid"], True)["vsize"] <= 1000
        self.generate(miner, 1)

        # With only small coins, no deposit after an unconfirmed one can be small enough: said so,
        # and nothing is sent.
        default.createsidechaindeposit(1, "third", 1)
        self.sync_mempools([miner, node])
        assert_raises_rpc_error(-6, "(The deposit before this one is not confirmed yet: until it is, a deposit can have no more than 1000 vbytes.)", small.createsidechaindeposit, 1, "fourth", Decimal("0.03"))
        assert_equal(len(miner.getrawmempool()), 1)
        self.generate(miner, 1)
        # After a confirmed one, it can be larger.
        deposit = small.createsidechaindeposit(1, "fourth", Decimal("0.03"))
        self.sync_mempools([miner, node])
        assert_greater_than(node.getrawtransaction(deposit["txid"], True)["vsize"], 1000)
        self.generate(miner, 1)

    def test_vouch(self):
        self.log.info("vouchwithdrawalbundle: the bundle the sidechain has, or none, decides the vote")
        node = self.nodes[0]
        blind = CTransaction()
        address = node.get_wallet_rpc(self.default_wallet_name).getnewaddress()
        blind.vout = [CTxOut(0, CScript([OP_RETURN, (1000).to_bytes(8, "big")])), CTxOut(COIN // 10, address_to_scriptpubkey(address))]
        bundle = node.receivewithdrawalbundle(1, blind.serialize().hex())["hash"]
        assert_raises_rpc_error(-8, "This node was not handed that bundle (receivewithdrawalbundle)", node.vouchwithdrawalbundle, 1, "ee" * 32)
        assert_raises_rpc_error(-8, "Sidechain slot out of range", node.vouchwithdrawalbundle, 256)
        assert_equal(node.vouchwithdrawalbundle(1, bundle), None)
        self.generate(node, 1)
        [pending] = node.listwithdrawalbundles(1)
        assert_equal((pending["hash"], pending["vote"]), (bundle, "upvote"))
        self.generate(node, 1)
        assert_equal(node.listwithdrawalbundles(1)[0]["score"], pending["score"] + 1)
        # The sidechain says its chain has none: the bundle is downvoted.
        assert_equal(node.vouchwithdrawalbundle(1), None)
        assert_equal(node.listwithdrawalbundles(1)[0]["vote"], "downvote")
        self.generate(node, 1)
        assert_equal(node.listwithdrawalbundles(1)[0]["score"], pending["score"])
        # And back.
        node.vouchwithdrawalbundle(1, bundle)
        assert_equal(node.listwithdrawalbundles(1)[0]["vote"], "upvote")

    def test_unreadable_miner_state(self):
        self.log.info("A miner state that cannot be read is reset at startup, with a warning")
        node = self.nodes[0]
        node.setdefaultwithdrawalvote("downvote")
        assert_equal(node.listwithdrawalbundles(1)[0]["vote"], "downvote")
        self.stop_node(0)
        (node.chain_path / "drivechain_miner.dat").write_bytes(b"\x05\x00\x00\x00garbage")
        with node.assert_debug_log(["The saved drivechain settings of this node (proposals, acks, votes and withdrawal bundles) could not be read and were reset."]):
            self.start_node(0, extra_args=self.extra_args[0])
        # Reset: no bundle handed, no default vote of the operator's.
        assert_equal(node.listwithdrawalbundles(1)[0]["known"], False)
        assert_equal(node.listwithdrawalbundles(1)[0]["vote"], "abstain")
        # Saved again as the node goes on.
        node.setdefaultwithdrawalvote("downvote")
        self.stop_node(0, expected_stderr="Warning: The saved drivechain settings of this node (proposals, acks, votes and withdrawal bundles) could not be read and were reset.")
        self.start_node(0, extra_args=self.extra_args[0])
        assert_equal(node.listwithdrawalbundles(1)[0]["vote"], "downvote")
        assert_greater_than(len((node.chain_path / "drivechain_miner.dat").read_bytes()), 8)


if __name__ == "__main__":
    DrivechainWalletTest(__file__).main()
