#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the sidechain rules against a mainchain node.

The nodes of the test are nodes of this sidechain. The mainchain node is a
node of the Chains mainchain, looked for in the directory named by the
environment variable MAINCHAIN_BIN_DIR, or else in ../chains/build/bin next
to this source tree. The test is skipped if there is none.
"""

import atexit
import os
import subprocess
import time
from decimal import Decimal
from http.client import HTTPException

from test_framework.authproxy import AuthServiceProxy, JSONRPCException
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
    p2p_port,
    rpc_port,
)

# The node program of the mainchain.
MAINCHAIN_DAEMON = "chains" + "d"

SLOT = 3
# Regtest parameters of the mainchain, see CRegTestParams there.
ACTIVATION_PERIOD = 20
WITHDRAWAL_PERIOD = 60
WITHDRAWAL_MIN_SCORE = 30
# Regtest parameters of this chain as a sidechain.
# What a block pays can be spent in the next.
COINBASE_MATURITY = 0
BUNDLE_RETRY_DELAY = 5



def fresh_connections(proxy):
    """A connection per call: a node closes a connection left idle for a while, which a busy test can do."""
    proxy.reuse_http_connections = False
    return proxy

class SidechainTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
        ]
        self.extra_args = [side_args, side_args]
        self.main_process = None

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def start_mainchain(self):
        bin_dir = os.environ.get("MAINCHAIN_BIN_DIR", os.path.join(self.config["environment"]["SRCDIR"], "..", "chains", "build", "bin"))
        binary = os.path.join(bin_dir, MAINCHAIN_DAEMON)
        if not os.path.isfile(binary):
            raise SkipTest(f"no mainchain node at {binary}; set MAINCHAIN_BIN_DIR")
        datadir = os.path.join(self.options.tmpdir, "mainchain")
        os.makedirs(datadir, exist_ok=True)
        self.main_args = [
            binary, "-regtest", f"-datadir={datadir}", f"-rpcport={self.main_rpc_port}", f"-port={p2p_port(self.num_nodes)}",
            "-rpcuser=main", "-rpcpassword=secret", "-fallbackfee=0.0002", "-listen=0", "-server", "-printtoconsole=0",
        ]
        self.main_process = subprocess.Popen(self.main_args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Whatever happens to the test, the mainchain node must not outlive it.
        atexit.register(self.kill_mainchain, self.main_process)
        url = f"http://main:secret@127.0.0.1:{self.main_rpc_port}"
        for _ in range(240):
            try:
                node = fresh_connections(AuthServiceProxy(url, timeout=120))
                node.getblockcount()
                break
            except (OSError, HTTPException, JSONRPCException):
                time.sleep(0.25)
        else:
            raise AssertionError("the mainchain node did not start")
        if "miner" not in node.listwallets():
            if "miner" in [w["name"] for w in node.listwalletdir()["wallets"]]:
                node.loadwallet("miner")
            else:
                node.createwallet("miner")
        self.main = fresh_connections(AuthServiceProxy(url + "/wallet/miner", timeout=120))

    @staticmethod
    def kill_mainchain(process):
        if process.poll() is None:
            process.kill()

    def stop_mainchain(self):
        if self.main_process is None:
            return
        try:
            self.main.stop()
        except (OSError, HTTPException, JSONRPCException):
            self.main_process.terminate()
        self.main_process.wait(timeout=120)
        self.main_process = None

    def setup_network(self):
        # The nodes of the sidechain follow the mainchain from the moment they start.
        self.start_mainchain()
        super().setup_network()

    def mine_main(self, count=1):
        hashes = self.main.generatetoaddress(count, self.main_address)
        for node in self.nodes:
            node.syncmainchain()
        return hashes

    def bmm(self, node=None, expect=True):
        """Mine one block of the sidechain: build it, have the mainchain commit to it."""
        node = node or self.nodes[0]
        block = node.createbmmblock(self.side_address)
        assert_equal(block["mainchainblockhash"], self.main.getbestblockhash())
        request = self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        assert_equal(request["prevblockhash"], block["mainchainblockhash"])
        self.mine_main()
        if expect:
            assert_equal(node.getbestblockhash(), block["hash"])
            self.sync_blocks()
        return block["hash"]

    def check_in_sync(self):
        self.sync_blocks()
        # The whole state of the sidechain, entry by entry.
        states = [n.getsidechainstate() for n in self.nodes]
        assert_equal(len({(st["bestblock"], st["hash"]) for st in states}), 1)
        assert_equal(self.nodes[0].listwithdrawals(), self.nodes[1].listwithdrawals())
        assert_equal(self.nodes[0].getwithdrawalbundle(), self.nodes[1].getwithdrawalbundle())
        assert_equal(self.nodes[0].gettxoutsetinfo()["total_amount"], self.nodes[1].gettxoutsetinfo()["total_amount"])

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("The nodes follow the mainchain")
        self.mine_main(110)
        info = side.getmainchaininfo()
        assert_equal(info["slot"], SLOT)
        assert_equal(info["connected"], True)
        assert_equal(info["height"], 110)
        assert_equal(info["bestblockhash"], main.getbestblockhash())
        assert_equal(info["tipheight"], -1)

        self.log.info("Blocks need a commitment of the mainchain")
        assert_raises_rpc_error(-32603, "block not accepted", self.generateblock, side, self.side_address, [], sync_fun=self.no_op)
        candidate = side.createbmmblock(self.side_address)
        self.mine_main()
        assert_equal(side.getblockcount(), 0)
        # The slot is not a sidechain of the mainchain yet, so the mainchain refuses to commit to anything for it.
        assert_raises_rpc_error(-4, "not accepted into the mempool", main.createbmmrequest, SLOT, candidate["hash"], Decimal("0.001"))

        self.log.info("Activate the sidechain on the mainchain")
        main.createsidechainproposal(SLOT, "Testchain", "The sidechain of this test", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        assert_equal([s["slot"] for s in main.listactivesidechains()], [SLOT])

        self.log.info("Mine blocks by blind merged mining")
        first = self.bmm()
        assert_equal(side.getblockcount(), 1)
        assert_equal(side.getmainchaininfo()["tipheight"], main.getblockcount() - 1)
        assert_equal(other.getbestblockhash(), first)
        # A block the mainchain did not commit to stays out, and so does one it committed to too late.
        stale = side.createbmmblock(self.side_address)
        self.mine_main()
        assert_equal(side.getblockcount(), 1)
        self.mine_main()
        assert_equal(side.getblockcount(), 1)
        del stale
        # No coins exist yet: there is no block subsidy on a sidechain.
        assert_equal(side.gettxoutsetinfo()["total_amount"], 0)

        self.log.info("Deposit from the mainchain")
        # The wallet of the sidechain gives a deposit address, which names the sidechain and has a checksum.
        deposit = side.getdepositaddress()
        deposit_address = deposit["address"]
        assert_equal(deposit["slot"], SLOT)
        assert deposit["depositaddress"].startswith(f"s{SLOT}_{deposit_address}_")
        assert_equal(side.getaddressinfo(deposit_address)["ismine"], True)
        assert_raises_rpc_error(-5, "Not an address of this chain", side.getdepositaddress, "nonsense")
        # The mainchain wallet refuses it for another sidechain, or with a typing error.
        assert_raises_rpc_error(-5, f"is for the sidechain in slot {SLOT}", main.createsidechaindeposit, SLOT + 1, deposit["depositaddress"], 50)
        assert_raises_rpc_error(-5, "check it for typing errors", main.createsidechaindeposit, SLOT, deposit["depositaddress"][:-1] + ("0" if deposit["depositaddress"][-1] != "0" else "1"), 50)
        main.createsidechaindeposit(SLOT, deposit["depositaddress"], 50)
        main.createsidechaindeposit(SLOT, "not an address of the sidechain", 1)
        # An address as it is works too, unchecked.
        main.createsidechaindeposit(SLOT, other.getnewaddress(), 7)
        self.mine_main()
        assert_equal(side.getbalances()["mine"]["immature"], 0)
        block = side.getblock(self.bmm(), 2)
        coinbase = block["tx"][0]
        assert_equal([out["value"] for out in coinbase["vout"][:4]], [0, 50, 1, 7])
        assert_equal(coinbase["vout"][1]["scriptPubKey"]["address"], deposit_address)
        # The deposit that names no address of this chain is burned.
        assert_equal(coinbase["vout"][2]["scriptPubKey"]["type"], "nulldata")
        # What the block pays can be spent in the next.
        assert_equal(side.getbalances()["mine"]["immature"], 0)
        assert_equal(side.getbalance(), 50)
        assert_equal(other.getbalance(), 7)
        assert_equal(side.gettxoutsetinfo()["total_amount"], 57)
        self.check_in_sync()

        self.log.info("Transactions pay their fees to the miner of the block")
        txid = side.sendtoaddress(other.getnewaddress(), 5)
        self.sync_mempools()
        fee = -side.gettransaction(txid)["fee"]
        block = side.getblock(self.bmm(), 2)
        assert_equal(len(block["tx"]), 2)
        assert_equal(block["tx"][0]["vout"][0]["value"], fee)
        assert_equal(other.getbalance(), 12)
        assert_equal(side.gettxoutsetinfo()["total_amount"], 57)

        self.log.info("Withdraw to the mainchain")
        payout_address = main.getnewaddress()
        assert_raises_rpc_error(-5, "Not an address of the mainchain", side.createwithdrawal, "nonsense", 1)
        assert_raises_rpc_error(-4, "below the minimum", side.createwithdrawal, payout_address, Decimal("0.00000100"))
        assert_equal(side.getwithdrawalbundle(), {"status": "none", "waiting": 0})
        withdrawal = side.createwithdrawal(payout_address, 3, Decimal("0.01"))
        small = other.createwithdrawal(payout_address, 1, Decimal("0.002"))
        self.sync_mempools()
        assert_equal(side.listwithdrawals(), [])
        self.bmm()
        assert_equal(side.gettxoutsetinfo()["total_amount"] < Decimal("52.99"), True)
        withdrawals = {w["txid"]: w for w in side.listwithdrawals()}
        assert_equal(len(withdrawals), 2)
        assert_equal(withdrawals[withdrawal["txid"]]["amount"], 3)
        assert_equal(withdrawals[withdrawal["txid"]]["mainchainfee"], Decimal("0.01"))
        assert_equal(withdrawals[withdrawal["txid"]]["refundaddress"], withdrawal["refundaddress"])
        assert_equal(withdrawals[withdrawal["txid"]]["status"], "waiting")
        bundle = side.getwithdrawalbundle()
        assert_equal(bundle["status"], "next")
        assert_equal(bundle["withdrawals"], 2)
        assert_equal(bundle["amount"], 4)
        assert_equal(bundle["mainchainfee"], Decimal("0.012"))

        self.log.info("Take a withdrawal back")
        assert_raises_rpc_error(-5, "no such transaction output", other.refundwithdrawal, withdrawal["txid"], withdrawal["vout"])
        assert_raises_rpc_error(-5, "not a withdrawal", side.refundwithdrawal, txid, 0)
        balance = other.getbalance()
        refund = other.refundwithdrawal(small["txid"], small["vout"])
        assert_equal(refund["amount"], Decimal("1.002"))
        assert_equal(refund["refundaddress"], small["refundaddress"])
        assert_raises_rpc_error(-4, "not accepted into the mempool", other.refundwithdrawal, small["txid"], small["vout"])
        self.sync_mempools()
        # The block with the refund does not start a bundle that would take the withdrawal.
        block = side.getblock(self.bmm(), 2)
        assert_equal(block["tx"][0]["vout"][1]["value"], Decimal("1.002"))
        assert_equal(block["tx"][0]["vout"][1]["scriptPubKey"]["address"], small["refundaddress"])
        assert_equal(len(side.listwithdrawals()), 1)
        assert_equal(side.getwithdrawalbundle()["status"], "next")
        assert_greater_than(other.getbalance(), balance + 1)
        # The next block starts the bundle.
        self.bmm()

        self.log.info("The withdrawal goes in a bundle, which the mainchain votes through and pays")
        bundle = side.getwithdrawalbundle()
        assert_equal(bundle["status"], "pending")
        assert_equal(bundle["withdrawals"], 1)
        assert_equal(side.listwithdrawals()[0]["status"], "bundled")
        assert_raises_rpc_error(-4, "not accepted into the mempool", side.refundwithdrawal, withdrawal["txid"], withdrawal["vout"])
        self.check_in_sync()
        # The node gave the bundle to its mainchain node, which proposes it and votes on it.
        self.mine_main()
        pending = main.listwithdrawalbundles(SLOT)
        assert_equal([b["hash"] for b in pending], [bundle["hash"]])
        assert_equal(pending[0]["known"], True)
        # A mainchain node upvotes the bundle its sidechain node gave it, unless it is told otherwise.
        assert_equal(pending[0]["vote"], "upvote")
        # The block that proposed the bundle gave it its first point, the votes of the blocks since the others.
        self.mine_main(WITHDRAWAL_MIN_SCORE - pending[0]["score"])
        # With the work score, the sidechain node has its mainchain node broadcast the withdrawal (BIP300 M6),
        # so any miner can mine it, one that does not run the sidechain too.
        self.wait_until(lambda: main.getreceivedbyaddress(payout_address, 0) == 3)
        assert_equal(main.getreceivedbyaddress(payout_address, 1), 0)
        self.mine_main()
        assert_equal(main.getreceivedbyaddress(payout_address), 3)
        assert_equal(main.getwithdrawalbundle(SLOT, bundle["hash"]), {"status": "paid"})
        assert_equal(main.getsidechain(SLOT)["escrow"]["amount"], Decimal("58") - Decimal("3.01"))
        # The sidechain learns of it with its next block.
        assert_equal(side.getwithdrawalbundle()["status"], "pending")
        self.bmm()
        assert_equal(side.listwithdrawals(), [])
        assert_equal(side.getwithdrawalbundle(), {"status": "none", "waiting": 0})
        self.check_in_sync()

        self.log.info("A bundle the mainchain does not vote for fails, and its withdrawals go in a later one")
        main.setwithdrawalvote(SLOT, "abstain")
        again = side.createwithdrawal(payout_address, 2)
        self.bmm()
        self.bmm()
        failed = side.getwithdrawalbundle()
        assert_equal(failed["status"], "pending")
        self.mine_main(WITHDRAWAL_PERIOD + 2)
        assert_equal(main.getwithdrawalbundle(SLOT, failed["hash"]), {"status": "failed"})
        self.bmm()
        height = side.getblockcount()
        assert_equal(side.getwithdrawalbundle(), {"status": "none", "waiting": 1, "lastfailureheight": height})
        assert_equal(side.listwithdrawals()[0]["status"], "waiting")
        for _ in range(BUNDLE_RETRY_DELAY - 1):
            self.bmm()
        retry = side.getwithdrawalbundle()
        assert_equal(retry["status"], "next")
        assert retry["hash"] != failed["hash"]
        self.bmm()
        assert_equal(side.getwithdrawalbundle()["status"], "pending")
        assert_equal(side.getwithdrawalbundle()["hash"], retry["hash"])
        self.mine_main()
        assert_equal([b["hash"] for b in main.listwithdrawalbundles(SLOT)], [retry["hash"]])
        self.check_in_sync()
        del again

        self.log.info("A reorganisation of the mainchain takes the blocks it committed to with it")
        tip = side.getbestblockhash()
        count = side.getblockcount()
        last = self.bmm()
        main_tip = main.getbestblockhash()
        main.invalidateblock(main_tip)
        for node in self.nodes:
            node.syncmainchain()
            assert_equal(node.getbestblockhash(), tip)
            assert_equal(node.getmainchaininfo()["height"], main.getblockcount())
        main.reconsiderblock(main_tip)
        for node in self.nodes:
            node.syncmainchain()
            assert_equal(node.getbestblockhash(), last)
        assert_equal(side.getblockcount(), count + 1)
        self.check_in_sync()

        self.log.info("The state survives restarts and is rebuilt by a reindex")
        expected = (side.listwithdrawals(), side.getwithdrawalbundle(), side.getbestblockhash(), side.gettxoutsetinfo()["total_amount"])
        self.restart_node(0)
        assert_equal((side.listwithdrawals(), side.getwithdrawalbundle(), side.getbestblockhash(), side.gettxoutsetinfo()["total_amount"]), expected)
        state = side.getsidechainstate()
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex"])
        self.wait_until(lambda: side.getbestblockhash() == expected[2])
        assert_equal((side.listwithdrawals(), side.getwithdrawalbundle(), side.gettxoutsetinfo()["total_amount"]), (expected[0], expected[1], expected[3]))
        assert_equal(side.getsidechainstate(), state)
        # Only the coins rebuilt, from genesis: the sidechain state with them, not read from the old tip.
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex-chainstate"])
        self.wait_until(lambda: side.getbestblockhash() == expected[2])
        assert_equal((side.listwithdrawals(), side.getwithdrawalbundle(), side.gettxoutsetinfo()["total_amount"]), (expected[0], expected[1], expected[3]))
        assert_equal(side.getsidechainstate(), state)

        self.log.info("A node that was away catches up, with the mainchain and with this chain")
        self.stop_node(1)
        block = side.createbmmblock(self.side_address)
        main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        main.generatetoaddress(1, self.main_address)
        side.syncmainchain()
        assert_equal(side.getbestblockhash(), block["hash"])
        self.start_node(1)
        self.connect_nodes(0, 1)
        self.sync_blocks()
        self.check_in_sync()

        self.log.info("Mine continuously")
        assert_equal(side.getbmminfo()["mining"], False)
        assert_raises_rpc_error(-8, "An address is required", side.setbmm, True)
        # For tests: a block for every block of the mainchain, with or without fees, at a set price.
        side.setbmm(True, self.side_address, True, Decimal("0.0005"))
        count = side.getblockcount()
        for i in range(3):
            self.wait_until(lambda: len(main.getrawmempool()) > 0)
            main.generatetoaddress(1, self.main_address)
            self.wait_until(lambda: side.getblockcount() == count + i + 1)
        mining = side.getbmminfo()
        assert_equal(mining["mining"], True)
        assert_equal(mining["address"], self.side_address)
        assert_greater_than(mining["requests"], 2)
        assert_equal(mining["blocks"], 3)
        assert_equal(mining["always"], True)
        assert_equal(mining["amount"], Decimal("0.0005"))
        # A restart (after a crash, say) does not stop it: it is kept in the settings.
        self.restart_node(0)
        self.connect_nodes(0, 1)
        mining = side.getbmminfo()
        assert_equal((mining["mining"], mining["address"], mining["always"], mining["amount"]), (True, self.side_address, True, Decimal("0.0005")))
        side.setbmm(False)
        self.restart_node(0)
        self.connect_nodes(0, 1)
        assert_equal(side.getbmminfo()["mining"], False)

        self.log.info("Mining by itself, the node asks for a block when fees pay for it, and offers 99% of them")
        # Start from nothing to mine. The last request of the mining before may still be taken.
        self.mine_main()
        if side.getrawmempool():
            self.bmm()
        assert_equal(side.getrawmempool(), [])
        side.setbmm(True, self.side_address)
        self.wait_until(lambda: side.getbmminfo()["idle"])
        count = side.getblockcount()
        requests = side.getbmminfo()["requests"]
        # Neither blocks of the mainchain nor a deposit make it ask for one: there are no fees in that.
        deposit = side.getdepositaddress()
        main.createsidechaindeposit(SLOT, deposit["depositaddress"], 4)
        main.generatetoaddress(3, self.main_address)
        time.sleep(3)
        assert_equal(main.getrawmempool(), [])
        assert_equal(side.getblockcount(), count)
        assert_equal(side.getbmminfo()["requests"], requests)
        assert_equal(side.getbmminfo()["always"], False)
        assert_equal(side.getreceivedbyaddress(deposit["address"]), 0)
        # A transaction with a fee does.
        txid = side.sendtoaddress(side.getnewaddress(), 1)
        fee = -side.gettransaction(txid)["fee"]
        self.wait_until(lambda: len(main.getrawmempool()) == 1)
        mining = side.getbmminfo()
        assert_equal(mining["idle"], False)
        assert_equal(mining["lastfees"], fee)
        offer = fee - (fee * 100000000 // 100) / Decimal(100000000)
        assert_equal(mining["lastoffer"], offer)
        # The mainchain wallet pays about that for the request.
        paid = main.getmempoolentry(main.getrawmempool()[0])["fees"]["base"]
        assert_greater_than(paid, offer * Decimal("0.9"))
        assert_greater_than(offer * Decimal("1.1"), paid)
        main.generatetoaddress(1, self.main_address)
        self.wait_until(lambda: side.getblockcount() == count + 1)
        assert_equal(side.gettransaction(txid)["confirmations"], 1)
        # The block paid the deposit as well, and what it paid can be spent at once.
        assert_equal(side.getreceivedbyaddress(deposit["address"]), 4)
        self.wait_until(lambda: side.getbmminfo()["idle"])
        side.setbmm(False)

        self.log.info("A block is mined by hand for a fee, empty or not")
        deposit = side.getdepositaddress()
        main.createsidechaindeposit(SLOT, deposit["depositaddress"], 6)
        main.generatetoaddress(1, self.main_address)
        assert_raises_rpc_error(-8, "The amount must be positive", side.requestbmmblock, self.side_address, 0)
        asked = side.requestbmmblock(self.side_address, Decimal("0.0003"))
        assert_equal(asked["transactions"], 1)
        assert_equal(asked["fees"], 0)
        assert_equal(asked["amount"], Decimal("0.0003"))
        assert asked["mainchaintxid"] in main.getrawmempool()
        main.generatetoaddress(1, self.main_address)
        self.wait_until(lambda: side.getbestblockhash() == asked["hash"])
        assert_equal(side.getreceivedbyaddress(deposit["address"]), 6)
        # Spendable in the next block.
        assert_equal(side.getbalances()["mine"]["immature"], 0)
        self.sync_blocks()
        self.check_in_sync()

        self.run_double_payout_test()

        self.log.info("Without the mainchain node, the node keeps running and says so")
        self.stop_mainchain()
        self.wait_until(lambda: not side.getmainchaininfo()["connected"])
        assert "Cannot reach the mainchain node" in side.getmainchaininfo()["error"]
        assert_raises_rpc_error(-1, "Cannot reach the mainchain node", side.syncmainchain)
        self.start_mainchain()
        self.wait_until(lambda: side.getmainchaininfo()["connected"])

        self.log.info("A mainchain node that takes connections and never answers does not stall the node")
        import socket
        import threading
        silent = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        silent.bind(("127.0.0.1", 0))
        silent.listen(16)
        held = []
        stop = threading.Event()

        def accept():
            silent.settimeout(0.5)
            while not stop.is_set():
                try:
                    held.append(silent.accept()[0])
                except OSError:
                    pass
        thread = threading.Thread(target=accept, daemon=True)
        thread.start()
        try:
            self.restart_node(1, extra_args=[a for a in self.extra_args[1] if not a.startswith("-mainchainrpcport")] + [f"-mainchainrpcport={silent.getsockname()[1]}"])
            self.connect_nodes(0, 1)
            # Blocks come from node 0: node 1 cannot check their commitments, and must stay responsive
            # meanwhile. (Built by hand: the helpers ask every node to sync with the mainchain.)
            block = side.createbmmblock(self.side_address)
            self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
            self.main.generatetoaddress(1, self.main_address)
            side.syncmainchain()
            assert_equal(side.getbestblockhash(), block["hash"])
            start = time.time()
            for _ in range(20):
                other.getblockcount()
                other.getpeerinfo()
            assert_greater_than(5, time.time() - start)
        finally:
            stop.set()
            thread.join()
            for conn in held:
                conn.close()
            silent.close()
        self.restart_node(1)
        self.connect_nodes(0, 1)
        self.sync_blocks()


    def run_double_payout_test(self):
        """A bundle left pending on the mainchain by a reorg is paid once, and its withdrawals are not paid again."""
        main = self.main
        side, other = self.nodes
        self.log.info("Double payout: a reorg leaves a bundle pending on the mainchain that the sidechain no longer has")
        # What is pending pays first, and the votes are back to their default.
        main.setwithdrawalvote(SLOT, "default")
        while main.listwithdrawalbundles(SLOT):
            self.mine_main()
        self.bmm()
        assert_equal(side.getwithdrawalbundle()["status"], "none")

        payout_address = main.getnewaddress()
        withdrawal = side.createwithdrawal(payout_address, 2)
        self.bmm()
        fee = side.listwithdrawals()[0]["mainchainfee"]
        # The block that commits to bundle X, and the mainchain block that commits to that block.
        committed = self.bmm()
        x = side.getwithdrawalbundle()
        assert_equal(x["status"], "pending")
        commitment = main.getbestblockhash()
        # The mainchain node got X from its sidechain node, and proposes it.
        self.mine_main()
        assert_equal([b["hash"] for b in main.listwithdrawalbundles(SLOT)], [x["hash"]])

        # The mainchain drops the block that committed to the sidechain block with X: the sidechain
        # block goes, and with it X, on this sidechain.
        main.invalidateblock(commitment)
        # The nodes of this chain learn it, and tell the mainchain node that their chain has no bundle
        # any more. (Their followers may have done so already, on their own: they poll every second.)
        for node in self.nodes:
            node.syncmainchain()
            assert committed != node.getbestblockhash()
        # The mainchain node still has X, and a block built before it heard that proposes X again on
        # the new branch. That order is not left to the timing of the followers: the word for X is
        # given back for that one block, then taken back as the sidechain nodes gave it.
        main.vouchwithdrawalbundle(SLOT, x["hash"])
        # The request for the sidechain block went back to the mempool, good for the new tip: an empty
        # block first, after which it is stale, so that nothing commits to that sidechain block again.
        main.generateblock(main.getnewaddress(), [])
        main.vouchwithdrawalbundle(SLOT)
        for node in self.nodes:
            node.syncmainchain()
            assert committed != node.getbestblockhash()
        self.mine_main()
        assert_equal([b["hash"] for b in main.listwithdrawalbundles(SLOT)], [x["hash"]])
        assert_equal(side.listwithdrawals()[0]["status"], "waiting")

        # X may hold the withdrawal: it cannot be refunded, nor put in another bundle, while X is pending.
        assert_raises_rpc_error(-4, "not accepted into the mempool", side.refundwithdrawal, withdrawal["txid"], withdrawal["vout"])
        self.bmm()
        assert_equal(side.getwithdrawalbundle()["status"], "none")
        # This chain has no bundle: its node says so, and the mainchain node downvotes X, which fails.
        assert_equal(main.listwithdrawalbundles(SLOT)[0]["vote"], "downvote")
        escrow = main.getsidechain(SLOT)["escrow"]["amount"]
        while main.getwithdrawalbundle(SLOT, x["hash"])["status"] == "pending":
            self.mine_main()
        assert_equal(main.getwithdrawalbundle(SLOT, x["hash"])["status"], "failed")
        # Then the withdrawal goes in a bundle of this chain, which is paid: once.
        for _ in range(BUNDLE_RETRY_DELAY + 2):
            self.bmm()
        y = side.getwithdrawalbundle()
        assert_equal(y["status"], "pending")
        assert y["hash"] != x["hash"]
        while main.getwithdrawalbundle(SLOT, y["hash"])["status"] == "pending":
            self.mine_main()
        assert_equal(main.getwithdrawalbundle(SLOT, y["hash"])["status"], "paid")
        self.mine_main()
        self.bmm()
        assert_equal(side.listwithdrawals(), [])
        for _ in range(5):
            self.mine_main()
        assert_equal(main.getreceivedbyaddress(payout_address), 2)
        assert_equal(main.getsidechain(SLOT)["escrow"]["amount"], escrow - 2 - fee)
        main.reconsiderblock(commitment)
        self.bmm()
        self.check_in_sync()


if __name__ == '__main__':
    SidechainTest(__file__).main()
