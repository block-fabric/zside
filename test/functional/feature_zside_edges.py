#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the shielded pool where feature_zside.py and feature_zside_rules.py do not: shielding to another
wallet's address, getblockstats of shielded transactions, a fee bump of an unshielding, shielded
transactions in a package, a wallet whose active descriptor changed or that was encrypted after its
shielded account was set up, what happens to a fee bump while it is proved, and the orchard index after an
unclean shutdown that followed the removal of a block; an account kept from another chain."""

from decimal import Decimal
import threading
import time

from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.authproxy import AuthServiceProxy, JSONRPCException
from test_framework.messages import COIN
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error, get_auth_cookie, rpc_port


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

    def wallet_proxy(self, node, wallet):
        """A connection of its own to a wallet of the node, for a call made in another thread."""
        user, password = get_auth_cookie(node.datadir_path, node.chain)
        return AuthServiceProxy(f"http://{user}:{password}@127.0.0.1:{rpc_port(node.index)}/wallet/{wallet}", timeout=600)

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

        self.run_shield_to_test()
        self.run_blockstats_test()
        self.run_bump_unshield_test()
        self.run_package_test()
        self.run_descriptor_test()
        self.run_encrypt_test()
        self.run_bump_race_test()
        self.run_index_crash_test()
        self.run_other_chain_test()

    def run_shield_to_test(self):
        side, other = self.nodes
        self.log.info("Shield transparent coins to another wallet's shielded address")
        to = other.getshieldedaddress(True)
        before = other.getshieldedbalance()["available"]
        mine_before = side.getshieldedbalance()["available"]
        txid = side.shieldcoins(Decimal("0.3"), to, "shielded for you")["txid"]
        assert_greater_than(len(side.getrawtransaction(txid, True)["vin"]), 0)
        self.sync_mempools()
        assert_equal(other.getshieldedbalance()["pending"], Decimal("0.3"))
        self.mine_txs()
        assert_equal(other.getshieldedbalance()["available"], before + Decimal("0.3"))
        note = [n for n in other.listshieldednotes() if n["txid"] == txid]
        assert_equal(len(note), 1)
        assert_equal(note[0]["amount"], Decimal("0.3"))
        assert_equal(note[0]["memo"], "shielded for you")
        assert_equal(note[0]["change"], False)
        # Nothing of it is the sender's.
        assert all(n["txid"] != txid for n in side.listshieldednotes(True))
        assert_equal(side.getshieldedbalance()["available"], mine_before)
        self.check_in_sync()

    def run_blockstats_test(self):
        side, other = self.nodes
        self.log.info("getblockstats: what a transaction takes out of the shielded pool counts as its input")
        shield = side.shieldcoins(Decimal("0.2"))["txid"]
        pay = side.sendshielded({other.getshieldedaddress(): Decimal("0.1")})["txid"]
        unshield = other.sendshielded({side.getnewaddress(): Decimal("0.1")})["txid"]
        self.sync_mempools()
        fees = [int(side.getmempoolentry(t)["fees"]["base"] * COIN) for t in (shield, pay, unshield)]
        assert_equal(side.getrawtransaction(pay, True)["vin"], [])
        assert_equal(side.getrawtransaction(unshield, True)["vin"], [])
        block = self.mine_txs()
        assert_equal(set(side.getblock(block)["tx"][1:]), {shield, pay, unshield})
        stats = side.getblockstats(block, ["txs", "totalfee", "minfee", "maxfee", "medianfee"])
        assert_equal(stats["txs"], 4)
        assert_equal(stats["totalfee"], sum(fees))
        assert_equal(stats["minfee"], min(fees))
        assert_equal(stats["maxfee"], max(fees))
        assert_equal(stats["medianfee"], sorted(fees)[1])
        self.check_in_sync()

    def run_bump_unshield_test(self):
        side, other = self.nodes
        self.log.info("A fee bump of an unshielding pays the same transparent output")
        to = other.getnewaddress()
        txid = side.sendshielded({to: Decimal("0.25")})["txid"]
        self.sync_mempools()
        res = side.bumpshieldedfee(txid)
        assert_greater_than(res["fee"], res["origfee"])
        bumped = side.getrawtransaction(res["txid"], True)
        assert_equal(bumped["vin"], [])
        paid = [o for o in bumped["vout"] if o["value"] > 0]
        assert_equal(len(paid), 1)
        assert_equal(paid[0]["scriptPubKey"]["address"], to)
        assert_equal(paid[0]["value"], Decimal("0.25"))
        self.sync_mempools()
        assert txid not in other.getrawmempool()
        self.mine_txs()
        assert_equal(side.gettransaction(res["txid"])["confirmations"], 1)
        assert_equal(other.getreceivedbyaddress(to), Decimal("0.25"))
        self.check_in_sync()

    def run_package_test(self):
        side, other = self.nodes
        self.log.info("A shielded transaction in a package is refused before the mempool checks it (they would not see the others)")
        txid = side.sendshielded({other.getshieldedaddress(): Decimal("0.05")})["txid"]
        old = side.gettransaction(txid)["hex"]
        side.bumpshieldedfee(txid)
        # The old one reveals the nullifiers of the one in the mempool: alone, a replacement that pays too little.
        alone = side.testmempoolaccept([old])[0]
        assert_equal(alone["allowed"], False)
        assert alone["reject-reason"].startswith("insufficient fee"), alone
        # In a package, with an unrelated transaction: the package is refused before the mempool's
        # replacement rules (which refuse replacements) see it. Without transparent inputs, a transaction
        # is consistent with no package.
        # (A shielding, with transparent inputs, for below.)
        shield = side.gettransaction(side.shieldcoins(Decimal("0.01"))["txid"])["hex"]
        coin = max(side.listunspent(), key=lambda u: u["amount"])
        unrelated = side.signrawtransactionwithwallet(side.createrawtransaction(
            [{"txid": coin["txid"], "vout": coin["vout"]}], {side.getnewaddress(): coin["amount"] - Decimal("0.001")}))["hex"]
        assert_equal(side.testmempoolaccept([unrelated])[0]["allowed"], True)
        for package in ([old, unrelated], [unrelated, old]):
            results = side.testmempoolaccept(package)
            assert_equal([r["package-error"] for r in results], ["conflict-in-package"] * 2)
        # With transparent inputs, a shielded transaction is refused in a package all the same: the
        # nullifiers of a package are not checked against each other.
        for package in ([shield, unrelated], [unrelated, shield]):
            results = side.testmempoolaccept(package)
            assert_equal([r["package-error"] for r in results], ["package-drivechain-tx"] * 2)
        self.mine_txs()
        self.check_in_sync()

    def run_descriptor_test(self):
        side, other = self.nodes
        self.log.info("A wallet whose active descriptor is no longer the one its shielded account was made from still spends its notes")
        other.createwallet("switched")
        switched = other.get_wallet_rpc("switched")
        address = switched.getshieldedaddress()
        side.sendshielded({address: Decimal("0.3")})
        self.mine_txs()
        assert_equal(switched.getshieldedbalance()["available"], Decimal("0.3"))

        def receiving(wallet):
            return [d for d in wallet.listdescriptors(True)["descriptors"] if d["active"] and not d["internal"] and d["desc"].startswith("wpkh(")][0]

        def make_active(wallet, d):
            result = wallet.importdescriptors([{"desc": d["desc"], "active": True, "timestamp": "now", "range": d["range"], "next_index": d["next_index"]}])
            assert result[0]["success"], result

        original = receiving(switched)
        other.createwallet("donor")
        replacement = receiving(other.get_wallet_rpc("donor"))
        other.unloadwallet("donor")
        make_active(switched, replacement)
        assert receiving(switched)["desc"] != original["desc"]
        # The same account: its addresses, its notes; and it spends them, with the keys of the
        # descriptor it was made from (no longer the active one).
        assert_equal(switched.getshieldedaddress(), address)
        assert_equal(switched.getshieldedbalance()["available"], Decimal("0.3"))
        to = side.getshieldedaddress()
        txid = switched.sendshielded({to: Decimal("0.1")})["txid"]
        self.mine_txs()
        assert_equal(switched.gettransaction(txid)["confirmations"], 1)
        # The original active again: the same.
        make_active(switched, original)
        txid = switched.sendshielded({to: Decimal("0.1")})["txid"]
        self.mine_txs()
        assert_equal(switched.gettransaction(txid)["confirmations"], 1)
        other.unloadwallet("switched")
        self.check_in_sync()

    def run_encrypt_test(self):
        side, other = self.nodes
        self.log.info("A wallet encrypted after its shielded account was set up (which makes it new descriptors) still spends its notes")
        other.createwallet("later")
        later = other.get_wallet_rpc("later")
        address = later.getshieldedaddress()
        side.sendshielded({address: Decimal("0.3")})
        self.mine_txs()
        before = later.listdescriptors()["descriptors"]
        later.encryptwallet("secret")
        assert later.listdescriptors()["descriptors"] != before
        # The same account: its addresses, its notes.
        assert_equal(later.getshieldedaddress(), address)
        assert_equal(later.getshieldedbalance()["available"], Decimal("0.3"))
        assert_raises_rpc_error(-13, "walletpassphrase", later.sendshielded, {side.getshieldedaddress(): Decimal("0.1")})
        later.walletpassphrase("secret", 600)
        txid = later.sendshielded({side.getshieldedaddress(): Decimal("0.1")})["txid"]
        self.mine_txs()
        assert_equal(later.gettransaction(txid)["confirmations"], 1)
        unshield = later.unshieldcoins(Decimal("0.1"))
        self.mine_txs()
        assert_equal(later.getreceivedbyaddress(unshield["address"]), Decimal("0.1"))
        later.walletlock()
        other.unloadwallet("later")
        self.check_in_sync()

    def run_bump_race_test(self):
        side, other = self.nodes
        self.log.info("While a fee bump is proved: its notes are held from other payments, and if the transaction it replaces is mined meanwhile, it is not sent")
        other.createwallet("racer")
        racer = other.get_wallet_rpc("racer")
        # Twenty notes, deep enough to be spent first; one more, newer. A bump of a payment of the twenty
        # proves more than twenty actions, for seconds, and takes the newer note for its higher fee.
        for _ in range(2):
            side.sendshielded({racer.getshieldedaddress(True): Decimal("0.01") for _ in range(10)})
            self.mine_txs()
        self.mine_txs()
        self.mine_txs()
        side.sendshielded({racer.getshieldedaddress(True): Decimal("0.05")})
        self.mine_txs()
        assert_equal(racer.getshieldedbalance(), {"available": Decimal("0.25"), "pending": 0, "notes": 21})
        # The twenty, no change.
        fee = Decimal("0.001")
        to = side.getshieldedaddress()
        txid = racer.sendshielded({to: Decimal("0.2") - fee}, None, fee)["txid"]
        assert_equal(racer.getshieldedbalance(), {"available": Decimal("0.05"), "pending": 0, "notes": 1})
        self.sync_mempools()
        unheld = "Not enough shielded funds: 0.05 available, 1.00"
        assert_raises_rpc_error(-4, unheld, racer.sendshielded, {to: 1})

        results = []

        def bump():
            try:
                results.append(self.wallet_proxy(other, "racer").bumpshieldedfee(txid))
            except JSONRPCException as e:
                results.append(e.error)

        thread = threading.Thread(target=bump)
        started = time.time()
        thread.start()
        # The bump holds the notes while it is proved: a payment that needs them says so.
        held = "Not enough shielded funds: 0.05 available, part of it held by a payment being made; 1.00"
        while True:
            try:
                racer.sendshielded({to: 1})
                raise AssertionError("paid with no funds")
            except JSONRPCException as e:
                assert_equal(e.error["code"], -4)
                if e.error["message"].startswith(held):
                    break
                assert e.error["message"].startswith(unheld), e.error
            assert thread.is_alive(), results
            time.sleep(0.05)
        # Meanwhile the transaction it replaces is mined, and the wallet hears of it.
        self.bmm(node=other)
        other.syncwithvalidationinterfacequeue()
        assert_equal(racer.gettransaction(txid)["confirmations"], 1)
        assert thread.is_alive(), "the bump was proved before the block came: too fast for this test"
        thread.join()
        self.log.info(f"The bump took {time.time() - started:.1f}s")
        assert_equal(results, [{"code": -4, "message": "The transaction it replaces is no longer waiting for a block"}])
        assert_equal(racer.gettransaction(txid)["confirmations"], 1)
        assert "replaced_by_txid" not in racer.gettransaction(txid)
        assert_equal(racer.getshieldedbalance(), {"available": Decimal("0.05"), "pending": 0, "notes": 1})
        # Nothing held any more.
        assert_raises_rpc_error(-4, unheld, racer.sendshielded, {to: 1})
        other.unloadwallet("racer")
        self.check_in_sync()

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

    def run_other_chain_test(self):
        side, other = self.nodes
        self.log.info("An account kept from another chain (a reset of the network, another slot) is retired: its notes are spent, nothing is paid into it again")
        # A wallet set up on another chain (the option stands in for it: keys made for another slot).
        self.restart_node(1, extra_args=self.extra_args[1] + [f"-zsidekeyslot={SLOT + 1}"])
        self.connect_nodes(0, 1)
        other.createwallet("moved")
        moved = other.get_wallet_rpc("moved")
        old_address = moved.getshieldedaddress()
        side.sendshielded({old_address: Decimal("0.3")}, "to the account of the other chain")
        self.mine_txs()
        assert_equal(moved.getshieldedbalance()["available"], Decimal("0.3"))
        # This chain: the account is retired on load, and a new one set up.
        self.restart_node(1)
        self.connect_nodes(0, 1)
        with other.assert_debug_log(expected_msgs=["zSide: the shielded account was set up for another chain"]):
            other.loadwallet("moved")
        moved = other.get_wallet_rpc("moved")
        new_address = moved.getshieldedaddress()
        assert new_address != old_address
        assert_equal(moved.getshieldedbalance()["available"], Decimal("0.3"))
        notes = moved.listshieldednotes()
        assert_equal(len(notes), 1)
        assert_equal(notes[0]["retired"], True)
        assert "watchonly" not in notes[0]
        # Nothing is paid into it again: not shielded, not paid from the account.
        assert_raises_rpc_error(-4, "set up for another chain", moved.shieldcoins, 1, old_address)
        assert_raises_rpc_error(-4, "set up for another chain", moved.sendshielded, {old_address: Decimal("0.1")})
        # The default shielding goes to the new account.
        # (Paid out of the first wallet's shielded account: its transparent coins are left as they are.)
        side.unshieldcoins(1, moved.getnewaddress())
        self.mine_txs()
        moved.shieldcoins(Decimal("0.5"))
        self.mine_txs()
        assert_equal(moved.getshieldedbalance()["available"], Decimal("0.8"))
        # Its note is spent (its keys made again for its chain): the coins are recovered.
        res = moved.unshieldcoins("all")
        self.mine_txs()
        assert_equal(moved.gettransaction(res["txid"])["confirmations"], 1)
        assert_equal(moved.getshieldedbalance()["available"], 0)
        # The retired account is kept across a restart, as it is.
        self.restart_node(1)
        self.connect_nodes(0, 1)
        with other.assert_debug_log(expected_msgs=[], unexpected_msgs=["zSide: the shielded account was set up for another chain"]):
            other.loadwallet("moved")
        moved = other.get_wallet_rpc("moved")
        assert_equal(moved.getshieldedaddress(), new_address)
        assert_raises_rpc_error(-4, "set up for another chain", moved.shieldcoins, 1, old_address)
        other.unloadwallet("moved")
        self.check_in_sync()

if __name__ == "__main__":
    ZSideEdgesTest(__file__).main()
