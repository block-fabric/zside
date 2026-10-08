#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the shielded pool: shielding, private payments with memos, unshielding, double spends, undo, fee bumps,
conflicts, the account of a restored or locked wallet, melt/cast."""

from decimal import Decimal
import os
import time

from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.messages import tx_from_hex
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

# Melt/cast on regtest: a bill's window opens every CAST_PERIOD blocks, at (bill * 7) % CAST_PERIOD, for WINDOW blocks.
CAST_PERIOD = 12
WINDOW = 3
BILL_UNIT = Decimal("0.01")


class ZSideTest(SidechainTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def mine_txs(self):
        self.sync_mempools()
        return self.nodes[0].getblock(self.bmm(), 2)

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain and fund the first wallet")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "zSide", "Private payments")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(), 20)
        self.mine_main()
        self.bmm()
        assert_equal(side.getshieldedinfo()["pool"], 0)

        self.log.info("Addresses")
        address = side.getshieldedaddress()
        assert address.startswith("rzo1")
        assert_equal(side.getshieldedaddress(), address)
        assert side.getshieldedaddress(True) != address
        other_address = other.getshieldedaddress()

        self.log.info("Shield transparent coins")
        balance_before = side.getbalance()
        txid = side.shieldcoins(5, None, "into the pool")["txid"]
        tx = side.getrawtransaction(txid, True)
        assert_equal(len(tx["vin"]) > 0, True)
        assert_equal(side.getshieldedbalance()["pending"], Decimal(5))
        self.mine_txs()
        assert_equal(side.getshieldedbalance(), {"available": Decimal(5), "pending": Decimal(0), "notes": 1})
        assert_equal(side.getshieldedinfo()["pool"], Decimal(5))
        assert_greater_than(balance_before - 5, side.getbalance() - Decimal("0.01"))
        notes = side.listshieldednotes()
        assert_equal(notes[0]["memo"], "into the pool")
        assert_equal(other.getshieldedbalance()["notes"], 0)

        self.log.info("Pay another wallet privately, with a memo")
        txid = side.sendshielded({other_address: 2}, "thanks for the coffee")["txid"]
        tx = side.getrawtransaction(txid, True)
        assert_equal(tx["vin"], [])
        self.sync_mempools()
        assert_equal(other.getshieldedbalance()["pending"], Decimal(2))
        self.mine_txs()
        assert_equal(other.getshieldedbalance()["available"], Decimal(2))
        assert_equal(other.listshieldednotes()[0]["memo"], "thanks for the coffee")
        mine = side.getshieldedbalance()
        assert_greater_than(mine["available"], Decimal("2.99"))
        assert_greater_than(Decimal(3), mine["available"])
        history = side.listshieldedtransactions()
        assert_equal(history[0]["payments"][0]["address"], other_address)
        assert_equal(history[0]["payments"][0]["memo"], "thanks for the coffee")
        # The fee came out of the pool: it holds less than went in.
        info = side.getshieldedinfo()
        assert_equal(info["pool"], mine["available"] + 2)
        # Every bundle reveals a nullifier per action, the padding ones included.
        assert_equal(info["spent"], 4)
        # The index holds the same tree as consensus.
        assert_equal(info["index"]["root"], info["root"])

        self.log.info("Unshield to a transparent address")
        taddress = other.getnewaddress()
        txid = other.sendshielded({taddress: Decimal("1.5")})["txid"]
        self.mine_txs()
        assert_equal(other.getreceivedbyaddress(taddress), Decimal("1.5"))
        assert_greater_than(Decimal("0.5"), other.getshieldedbalance()["available"])

        self.log.info("Spending more than there is, or a note twice, fails")
        assert_raises_rpc_error(-4, "Not enough shielded funds", other.sendshielded, {address: 5})
        spend = side.sendshielded({other_address: 1})["txid"]
        raw = side.getrawtransaction(spend)
        self.mine_txs()
        # Again, in a later block: its nullifiers are known.
        res = side.testmempoolaccept([raw])[0]
        assert_equal(res["allowed"], False)
        assert res["reject-reason"] in ("bad-zs-double-spend", "txn-already-known", "txn-already-in-mempool"), res

        self.log.info("A changed byte of the bundle is refused")
        tx = tx_from_hex(raw)
        marker = [o for o in tx.vout if o.scriptPubKey[:1] == b"\x6a"][0]
        script = bytearray(marker.scriptPubKey)
        script[-70] ^= 1
        marker.scriptPubKey = bytes(script)
        res = side.testmempoolaccept([tx.serialize().hex()])[0]
        assert_equal(res["allowed"], False)

        self.log.info("Undoing blocks undoes the pool")
        info = side.getshieldedinfo()
        balance = side.getshieldedbalance()
        tip = side.getbestblockhash()
        block = side.getblockhash(side.getblockcount() - 3)
        side.invalidateblock(block)
        assert info != side.getshieldedinfo()
        side.reconsiderblock(block)
        assert_equal(side.getbestblockhash(), tip)
        assert_equal(side.getshieldedinfo(), info)
        assert_equal(side.getshieldedbalance(), balance)
        self.check_in_sync()

        # Before melt and unshield, which leave the wallet without coins on either side.
        self.run_anchor_reorg_test()
        self.run_bump_test()
        self.run_conflict_test()
        self.run_rescan_test()
        self.run_meltcast_test()
        self.run_unshield_test()

    def run_anchor_reorg_test(self):
        side, other = self.nodes
        self.log.info("A spend is anchored a few blocks back: a reorg of the last blocks leaves it in the mempool")
        side.shieldcoins(1)
        self.mine_txs()
        anchored = side.getbestblockhash()
        # Its note, and every other, three blocks deep: the spend's anchor is the tree the block of the
        # newest of them left.
        self.mine_txs()
        self.mine_txs()
        spend = side.sendshielded({other.getshieldedaddress(): Decimal("0.1")})["txid"]
        assert spend in side.getrawmempool()
        tip = side.getbestblockhash()
        side.invalidateblock(tip)
        assert spend in side.getrawmempool()
        side.reconsiderblock(tip)
        assert_equal(side.getbestblockhash(), tip)

        self.log.info("A spend whose anchor a reorg takes away leaves the mempool, and blocks can still be made")
        side.invalidateblock(anchored)
        assert spend not in side.getrawmempool()
        # The wallet gives it up (abandons it), rather than hold its notes as spent forever.
        self.wait_until(lambda: all(n.get("spent_by") != spend for n in side.listshieldednotes(True)))
        # The next block is built (the template would fail on the stale spend). The other node keeps
        # the anchored block and those after it, its branch the longer.
        replacement = self.bmm(expect=False)
        assert_equal(side.getbestblockhash(), replacement)
        # Back, the node may go back to the anchored branch. Either way the next block makes one
        # branch longer, and both nodes take it.
        side.reconsiderblock(anchored)
        self.bmm()
        self.sync_blocks()
        # The spend is still the wallet's: sent again if its anchor is on the chain again (mined, it is
        # the wallet's again), left abandoned if not.
        try:
            side.sendrawtransaction(side.gettransaction(spend)["hex"])
            self.mine_txs()
            assert_equal(side.gettransaction(spend)["confirmations"], 1)
        except JSONRPCException:
            assert_raises_rpc_error(-5, "not eligible for abandonment", side.abandontransaction, spend)
        self.check_in_sync()

    def run_bump_test(self):
        side, other = self.nodes
        self.log.info("A shielded transaction is fee-bumped: the new one spends the same notes, and replaces it")
        self.mine_txs()
        before = side.getshieldedbalance()
        assert_equal(before["pending"], 0)
        to = other.getshieldedaddress(True)
        txid = side.sendshielded({to: Decimal("0.2")}, "bumped")["txid"]
        self.sync_mempools()
        assert_raises_rpc_error(-4, "transparent inputs", side.bumpshieldedfee, side.shieldcoins(Decimal("0.01"))["txid"])
        assert_raises_rpc_error(-4, "The new fee must be at least", side.bumpshieldedfee, txid, Decimal("0.00000001"))
        res = side.bumpshieldedfee(txid)
        assert_greater_than(res["fee"], res["origfee"])
        bumped = res["txid"]
        self.sync_mempools()
        for node in self.nodes:
            assert bumped in node.getrawmempool()
            assert txid not in node.getrawmempool()
        assert_equal(side.gettransaction(txid)["replaced_by_txid"], bumped)
        assert_raises_rpc_error(-4, "replaced already", side.bumpshieldedfee, txid)
        # The old one does not come back: it reveals the same nullifiers, and pays less.
        accept = side.testmempoolaccept([side.gettransaction(txid)["hex"]])[0]
        assert_equal(accept["allowed"], False)
        assert accept["reject-reason"].startswith("insufficient fee"), accept
        # Bumped again, with a fee given.
        fee = res["fee"] * 2
        res = side.bumpshieldedfee(bumped, fee)
        assert_equal(res["fee"], fee)
        bumped = res["txid"]
        self.mine_txs()
        # The old ones conflict with the one in the block; the change of none of them counts but its.
        assert_greater_than(0, side.gettransaction(txid)["confirmations"])
        assert_equal(side.gettransaction(bumped)["confirmations"], 1)
        after = side.getshieldedbalance()
        assert_equal(after["pending"], 0)
        # The 0.01 shielded on the way came in.
        assert_equal(after["available"], before["available"] - Decimal("0.2") - fee + Decimal("0.01"))
        assert {"amount": Decimal("0.2"), "memo": "bumped"}.items() <= [n for n in other.listshieldednotes() if n["txid"] == bumped][0].items()
        self.check_in_sync()

    def run_conflict_test(self):
        side, other = self.nodes
        self.log.info("A spend of the wallet whose note another spends in a block: conflicted, and gone from the mempool")
        backup = os.path.join(self.options.tmpdir, "side-wallet.dat")
        side.backupwallet(backup)
        other.restorewallet("copy", backup)
        copy = other.get_wallet_rpc("copy")
        other_wallet = other.get_wallet_rpc(self.default_wallet_name)
        txid = side.sendshielded({other_wallet.getshieldedaddress(): Decimal("0.1")})["txid"]
        self.sync_mempools()
        assert_equal(copy.gettransaction(txid)["txid"], txid)
        # Apart, the copy of the wallet spends the same notes again, with a higher fee, and its node mines it.
        self.disconnect_nodes(0, 1)
        bumped = copy.bumpshieldedfee(txid)["txid"]
        assert txid in side.getrawmempool()
        assert bumped in other.getrawmempool()
        self.bmm(node=other, expect=False)
        self.connect_nodes(0, 1)
        self.sync_blocks()
        # The node finds the conflict by the nullifiers of the block, the wallet by the note.
        assert txid not in side.getrawmempool()
        assert_greater_than(0, side.gettransaction(txid)["confirmations"])
        assert_equal(side.gettransaction(bumped)["confirmations"], 1)
        assert_equal(side.getshieldedbalance()["pending"], 0)
        assert_equal(side.getshieldedbalance(), copy.getshieldedbalance())
        other.unloadwallet("copy")
        self.check_in_sync()

    def run_rescan_test(self):
        side, other = self.nodes
        self.log.info("A blank wallet that imports the keys of another, from the start, finds its shielded notes")
        received = other.getshieldedbalance()
        assert_greater_than(received["available"], 0)
        descriptors = []
        for d in other.listdescriptors(True)["descriptors"]:
            request = {"desc": d["desc"], "timestamp": 0, "active": d["active"], "internal": d.get("internal", False)}
            if "range" in d:
                request["range"] = d["range"]
                request["next_index"] = d["next_index"]
            descriptors.append(request)
        other.createwallet("imported", blank=True)
        imported = other.get_wallet_rpc("imported")
        assert all(r["success"] for r in imported.importdescriptors(descriptors))
        other_wallet = other.get_wallet_rpc(self.default_wallet_name)
        assert_equal(imported.getshieldedbalance(), received)
        assert_equal(imported.listshieldednotes(True), other_wallet.listshieldednotes(True))
        other.unloadwallet("imported")

        self.log.info("A wallet that scanned blocks locked, its shielded account not set up, scans them again once unlocked")
        other.createwallet("locked", passphrase="secret")
        locked = other.get_wallet_rpc("locked")
        assert_raises_rpc_error(-13, "unlock the wallet once", locked.getshieldedbalance)
        backup = os.path.join(self.options.tmpdir, "locked-wallet.dat")
        locked.backupwallet(backup)
        other.restorewallet("unlocked", backup)
        unlocked = other.get_wallet_rpc("unlocked")
        unlocked.walletpassphrase("secret", 600)
        side.sendshielded({unlocked.getshieldedaddress(): Decimal("0.3")}, "to a locked wallet")
        self.mine_txs()
        assert_equal(unlocked.getshieldedbalance()["available"], Decimal("0.3"))
        # The same keys: the locked wallet saw the block, and could not read it.
        locked.walletpassphrase("secret", 600)
        assert_equal(locked.getshieldedbalance()["available"], Decimal("0.3"))
        assert_equal(locked.listshieldednotes()[0]["memo"], "to a locked wallet")
        # It does not scan again after a restart: it is noted done.
        self.restart_node(1)
        self.connect_nodes(0, 1)
        with other.assert_debug_log(expected_msgs=[], unexpected_msgs=["zSide: scanning again"]):
            other.loadwallet("locked")
            assert_equal(other.get_wallet_rpc("locked").getshieldedbalance()["available"], Decimal("0.3"))
        other.unloadwallet("locked")
        self.check_in_sync()

    def run_unshield_test(self):
        side, other = self.nodes
        self.log.info("Unshield: to a new address of the wallet, or all of it, the fee out of it")
        res = side.unshieldcoins(Decimal("0.5"))
        assert side.getaddressinfo(res["address"])["ismine"]
        assert_equal(res["amount"], Decimal("0.5"))
        assert_raises_rpc_error(-5, "A shielded address", side.unshieldcoins, 1, other.getshieldedaddress())
        self.mine_txs()
        assert_equal(side.getreceivedbyaddress(res["address"]), Decimal("0.5"))
        everything = other.getshieldedbalance()["available"]
        assert_greater_than(everything, 0)
        to = other.getnewaddress()
        res = other.unshieldcoins("all", to)
        assert_equal(res["address"], to)
        assert_greater_than(everything, res["amount"])
        assert_greater_than(res["amount"], everything - Decimal("0.01"))
        self.mine_txs()
        assert_equal(other.getshieldedbalance()["available"], 0)
        assert_equal(other.getreceivedbyaddress(to), res["amount"])
        assert_raises_rpc_error(-6, "Nothing in the shielded account", other.unshieldcoins, "all")
        self.check_in_sync()

    def run_meltcast_test(self):
        side, other = self.nodes

        self.log.info("Melt: each transparent coin is shielded alone, whole, and locked until then")
        # Three coins of the wallet, in blocks.
        side.sendmany("", {side.getnewaddress(): 1, side.getnewaddress(): 2, side.getnewaddress(): 3})
        self.mine_txs()
        unspent = {f"{u['txid']}:{u['vout']}": u["amount"] for u in side.listunspent()}
        planned = side.meltcoins(0)
        coins = {j["coin"]: unspent[j["coin"]] for j in planned}
        # The three coins among them; a coin too small for its fee is left out.
        assert_equal(sorted(a for a in coins.values() if a in (1, 2, 3)), [1, 2, 3])
        assert all(a >= Decimal("0.01") for a in coins.values())
        assert all(a < Decimal("0.001") or c in coins for c, a in unspent.items() if a >= Decimal("0.05"))
        assert all(j["kind"] == "melt" and j["status"] == "planned" for j in planned)
        assert_equal(len(side.listlockunspent()), len(coins))
        # Locked: nothing else spends them meanwhile, and a second melt plans nothing.
        assert_raises_rpc_error(-4, "No transparent coin to melt", side.meltcoins, 0)
        before = side.getshieldedbalance()["available"]
        self.mine_txs()
        self.wait_until(lambda: all(j["status"] == "sent" for j in side.listmeltcast()), timeout=120)
        for job in side.listmeltcast():
            tx = side.getrawtransaction(job["txid"], True)
            # One input, the coin; one output, the marker: no change.
            assert_equal(len(tx["vin"]), 1)
            assert_equal(f"{tx['vin'][0]['txid']}:{tx['vin'][0]['vout']}", job["coin"])
            assert_equal(len(tx["vout"]), 1)
        assert_equal(side.listlockunspent(), [])
        self.mine_txs()
        melted = side.getshieldedbalance()["available"] - before
        assert_greater_than(sum(coins.values()), melted)
        assert_greater_than(melted, sum(coins.values()) - Decimal("0.01"))

        self.log.info("Cast: bills, each to a new address, in its window")
        assert_raises_rpc_error(-4, "Not enough in the shielded account", side.castcoins, 10000)
        assert_raises_rpc_error(-4, "The smallest bill is 0.01", side.castcoins, Decimal("0.009"))
        planned = side.castcoins(Decimal("0.0599"), 0)
        # 0.05 is bills 0.04 and 0.01; the rest stays in the pool.
        assert_equal(sorted(j["amount"] for j in planned), [Decimal("0.01"), Decimal("0.04")])
        for job in planned:
            bill = int((job["amount"] / BILL_UNIT)).bit_length() - 1
            assert_equal((job["height"] - bill * 7) % CAST_PERIOD, 0)

        self.log.info("The plan survives a restart")
        ids = [j["id"] for j in side.listmeltcast()]
        self.restart_node(0)
        self.connect_nodes(0, 1)
        assert_equal([j["id"] for j in side.listmeltcast()], ids)

        cast_ids = {j["id"] for j in planned}
        for _ in range(3 * CAST_PERIOD):
            jobs = {j["id"]: j for j in side.listmeltcast() if j["id"] in cast_ids}
            if all(j["status"] == "sent" for j in jobs.values()):
                break
            tip = side.getblockcount()
            # A bill whose window is open goes, at this height.
            if any(j["status"] != "sent" and j["height"] <= tip for j in jobs.values()):
                self.wait_until(lambda: all(j["status"] == "sent" or j["height"] > tip for j in side.listmeltcast() if j["id"] in cast_ids), timeout=120)
            self.mine_txs()
        self.mine_txs()
        for job in side.listmeltcast():
            if job["id"] not in cast_ids:
                continue
            assert_equal(job["status"], "sent")
            wtx = side.gettransaction(job["txid"], True, True)
            tx = wtx["decoded"]
            assert_equal(tx["vin"], [])
            paid = [o for o in tx["vout"] if o["value"] > 0]
            assert_equal(len(paid), 1)
            assert_equal(paid[0]["value"], job["amount"])
            info = side.getaddressinfo(paid[0]["scriptPubKey"]["address"])
            assert info["ismine"]
            assert_equal(side.getreceivedbyaddress(paid[0]["scriptPubKey"]["address"]), job["amount"])
            # Sent with the tip in the bill's window.
            bill = int((job["amount"] / BILL_UNIT)).bit_length() - 1
            sent_at = wtx["blockheight"] - 1
            assert_greater_than(WINDOW, (sent_at - bill * 7) % CAST_PERIOD)
        # No two bills to one address.
        outs = [o["scriptPubKey"]["address"] for j in side.listmeltcast() if j["id"] in cast_ids for o in side.gettransaction(j["txid"], True, True)["decoded"]["vout"] if o["value"] > 0]
        assert_equal(len(set(outs)), len(outs))

        self.log.info("Melt and cast while blocks keep coming: the node keeps taking them")
        # Two transparent coins, out of the shielded account (the blocks pay the wallet little).
        fresh = [side.unshieldcoins(Decimal("0.1"))["address"] for _ in range(2)]
        self.mine_txs()
        # Two coins melted, the others (the blocks' rewards) held back meanwhile.
        others = [{"txid": u["txid"], "vout": u["vout"]} for u in side.listunspent() if u["address"] not in fresh]
        side.lockunspent(False, others)
        melts = side.meltcoins(0)
        assert_equal(len(melts), 2)
        side.lockunspent(True, others)
        busy = {j["id"] for j in melts} | {j["id"] for j in side.castcoins(Decimal("0.03"), 0)}
        for _ in range(4 * CAST_PERIOD):
            if all(j["status"] == "sent" for j in side.listmeltcast() if j["id"] in busy):
                break
            # Each block taken by both nodes, while the wallet proves (a melt/cast that waited for the
            # index on the scheduler's thread would stop the node here).
            self.mine_txs()
            time.sleep(3)
        assert all(j["status"] == "sent" for j in side.listmeltcast() if j["id"] in busy)
        self.mine_txs()
        for job in side.listmeltcast():
            if job["id"] in busy:
                assert_equal(side.gettransaction(job["txid"])["confirmations"] > 0, True)

        self.log.info("Cancelling")
        side.castcoins(Decimal("0.03"), 5)
        assert_raises_rpc_error(-8, "No such step", side.cancelmeltcast, "999")
        assert_equal(side.cancelmeltcast("all"), 2)
        assert all(j["status"] == "sent" for j in side.listmeltcast())
        self.check_in_sync()


if __name__ == "__main__":
    ZSideTest(__file__).main()
