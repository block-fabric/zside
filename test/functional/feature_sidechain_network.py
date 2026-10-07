#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that the nodes of a sidechain stay in agreement across a network.

Two mainchain nodes, A and B, and five sidechain nodes: 0 and 1 follow A,
2 and 3 follow B, and 4, which joins late, follows A. The sidechain nodes are
connected in a line, 0-1-2-3, so that blocks are relayed across nodes that
follow different mainchain nodes. On this:

  - blocks are mined through either mainchain node, with deposits, payments
    and a withdrawal bundle that both mainchain nodes vote through;
  - the mainchain splits, each half commits to blocks of its own, and the
    half with more work wins when it joins again: every sidechain node ends
    up on the blocks the winning half committed to;
  - a mainchain node stops while sidechain blocks go on: the nodes that
    follow it hold the blocks they cannot check, and take them when it is back;
  - a node that joins late syncs the whole chain from its peers;
  - a node restarted, and a node reindexed, come back to the same state.

After each step every running sidechain node must have the same best block and
the same state: unspent coins, withdrawals and bundle.
"""

import atexit
import os
import subprocess
import time
from decimal import Decimal
from http.client import HTTPException

from feature_sidechain import ACTIVATION_PERIOD, MAINCHAIN_DAEMON, SLOT, WITHDRAWAL_MIN_SCORE
from test_framework.authproxy import AuthServiceProxy, JSONRPCException
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    p2p_port,
    rpc_port,
)

# Sidechain nodes and the mainchain node each follows.
FOLLOWS = ["A", "A", "B", "B", "A"]
LATE = 4



def fresh_connections(proxy):
    """A connection per call: a node closes a connection left idle for a while, which a busy test can do."""
    proxy.reuse_http_connections = False
    return proxy

class MainNode:
    """A node of the mainchain, run as a process of its own."""

    def __init__(self, test, name, index):
        self.test = test
        self.name = name
        self.rpc = rpc_port(index)
        self.p2p = p2p_port(index)
        self.datadir = os.path.join(test.options.tmpdir, f"mainchain-{name}")
        self.process = None

    def start(self):
        bin_dir = os.environ.get("MAINCHAIN_BIN_DIR", os.path.join(self.test.config["environment"]["SRCDIR"], "..", "chains", "build", "bin"))
        binary = os.path.join(bin_dir, MAINCHAIN_DAEMON)
        if not os.path.isfile(binary):
            raise SkipTest(f"no mainchain node at {binary}; set MAINCHAIN_BIN_DIR")
        os.makedirs(self.datadir, exist_ok=True)
        args = [
            binary, "-regtest", f"-datadir={self.datadir}", f"-rpcport={self.rpc}", f"-port={self.p2p}", "-bind=127.0.0.1",
            "-rpcuser=main", "-rpcpassword=secret", "-fallbackfee=0.0002", "-server", "-printtoconsole=0", "-discover=0", "-dnsseed=0",
        ]
        self.process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        atexit.register(lambda process=self.process: process.poll() is None and process.kill())
        url = f"http://main:secret@127.0.0.1:{self.rpc}"
        for _ in range(240):
            try:
                fresh_connections(AuthServiceProxy(url, timeout=120)).getblockcount()
                break
            except (OSError, HTTPException, JSONRPCException):
                time.sleep(0.25)
        else:
            raise AssertionError(f"mainchain node {self.name} did not start")
        self.node = fresh_connections(AuthServiceProxy(url, timeout=120))
        if "miner" not in self.node.listwallets():
            if "miner" in [w["name"] for w in self.node.listwalletdir()["wallets"]]:
                self.node.loadwallet("miner")
            else:
                self.node.createwallet("miner")
        self.wallet = fresh_connections(AuthServiceProxy(url + "/wallet/miner", timeout=120))
        self.address = self.wallet.getnewaddress()

    def stop(self):
        try:
            self.node.stop()
        except (OSError, HTTPException, JSONRPCException):
            self.process.terminate()
        self.process.wait(timeout=120)
        self.process = None

    def generate(self, count=1):
        return self.wallet.generatetoaddress(count, self.address)


class SidechainNetworkTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = len(FOLLOWS)
        self.setup_clean_chain = True
        self.mains = {"A": None, "B": None}
        self.extra_args = []
        for i, name in enumerate(FOLLOWS):
            index = self.num_nodes + (0 if name == "A" else 1)
            self.extra_args.append([
                f"-sidechainslot={SLOT}",
                f"-mainchainrpcport={rpc_port(index)}",
                "-mainchainrpcuser=main",
                "-mainchainrpcpassword=secret",
                "-mainchainrpcwallet=miner",
            ])

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.mains["A"] = MainNode(self, "A", self.num_nodes)
        self.mains["B"] = MainNode(self, "B", self.num_nodes + 1)
        for main in self.mains.values():
            main.start()
        self.connect_mains()
        self.add_nodes(self.num_nodes, self.extra_args)
        for i in range(self.num_nodes):
            if i != LATE:
                self.start_node(i)
        # A line, so that blocks go through nodes that follow different mainchain nodes.
        for i in range(LATE - 1):
            self.connect_nodes(i, i + 1)

    # -- The mainchain.

    def connect_mains(self):
        a, b = self.mains["A"], self.mains["B"]
        a.node.setnetworkactive(True)
        b.node.setnetworkactive(True)
        a.node.addnode(f"127.0.0.1:{b.p2p}", "onetry")
        self.wait_until(lambda: len(a.node.getpeerinfo()) > 0 and len(b.node.getpeerinfo()) > 0)

    def sync_mains(self):
        a, b = self.mains["A"].node, self.mains["B"].node
        self.wait_until(lambda: a.getbestblockhash() == b.getbestblockhash(), timeout=120)

    def split_mains(self):
        self.mains["A"].node.setnetworkactive(False)
        self.mains["B"].node.setnetworkactive(False)
        self.wait_until(lambda: len(self.mains["A"].node.getpeerinfo()) == 0 and len(self.mains["B"].node.getpeerinfo()) == 0)

    # -- The sidechain.

    def running(self):
        return [node for node in self.nodes if node.running]

    def follow(self, nodes=None):
        """Have the sidechain nodes take in what their mainchain nodes have now."""
        for node in nodes or self.running():
            # A node whose mainchain node is down cannot ask it for news.
            if self.mains[FOLLOWS[node.index]].process is not None:
                node.syncmainchain()

    def bmm(self, i, main_name, sync=True):
        """Have sidechain node i build a block, and mainchain node main_name commit to it."""
        main = self.mains[main_name]
        node = self.nodes[i]
        self.follow([node])
        block = node.createbmmblock(self.address[i])
        assert_equal(block["mainchainblockhash"], main.node.getbestblockhash())
        main.wallet.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        main.generate()
        if sync:
            self.sync_mains()
        self.follow()
        assert_equal(node.getbestblockhash(), block["hash"])
        return block["hash"]

    def check_in_sync(self, nodes=None):
        """Every node has the same best block and the same state."""
        nodes = nodes or self.running()
        self.follow(nodes)
        try:
            self.sync_blocks(nodes, timeout=60)
        except AssertionError:
            for node in nodes:
                info = node.getmainchaininfo()
                tips = [(t["height"], t["hash"][:12], t["status"]) for t in node.getchaintips()]
                self.log.error(f"node {node.index}: tip {node.getblockcount()} {node.getbestblockhash()[:12]} mainchain {info['height']} {info['bestblockhash'][:12]} tips {tips}")
            for name, main in self.mains.items():
                if main.process is not None:
                    self.log.error(f"mainchain {name}: {main.node.getblockcount()} {main.node.getbestblockhash()[:12]}")
            raise
        tip = nodes[0].getbestblockhash()
        state = None
        for node in nodes:
            assert_equal(node.getbestblockhash(), tip)
            mine = (
                node.gettxoutsetinfo("muhash")["muhash"],
                node.listwithdrawals(),
                node.getwithdrawalbundle(),
                node.getmainchaininfo()["tipheight"],
            )
            if state is None:
                state = mine
            else:
                assert_equal(mine, state)
        return tip

    def run_test(self):
        try:
            self.run_network_test()
        finally:
            for main in self.mains.values():
                if main is not None and main.process is not None:
                    main.stop()

    def run_network_test(self):
        a, b = self.mains["A"], self.mains["B"]
        for i in range(self.num_nodes):
            if i != LATE:
                self.nodes[i].createwallet("side")
        self.address = {i: self.nodes[i].getnewaddress() for i in range(self.num_nodes) if i != LATE}

        self.log.info("The mainchain activates the sidechain; both mainchain nodes have coins to pay for blocks")
        a.generate(110)
        self.sync_mains()
        b.generate(110)
        self.sync_mains()
        a.wallet.createsidechainproposal(SLOT, "Sidechain", "The sidechain of the test")
        a.generate(ACTIVATION_PERIOD + 1)
        self.sync_mains()
        assert_equal(b.node.getsidechain(SLOT)["slot"], SLOT)

        self.log.info("Deposits through either mainchain node, and blocks committed by either")
        # A deposit spends the escrow output of the sidechain, so two made at once through different
        # mainchain nodes conflict: the second waits for the first to be mined.
        a.wallet.createsidechaindeposit(SLOT, self.nodes[0].getdepositaddress()["depositaddress"], 50)
        a.generate()
        self.sync_mains()
        b.wallet.createsidechaindeposit(SLOT, self.nodes[3].getdepositaddress()["depositaddress"], 30)
        b.generate()
        self.sync_mains()
        self.bmm(0, "A")
        self.bmm(2, "B")
        self.check_in_sync()
        assert_equal(self.nodes[0].getbalance(), 50)
        assert_equal(self.nodes[3].getbalance(), 30)

        self.log.info("Payments relayed across the line, mined by the far end")
        paid = self.nodes[0].sendtoaddress(self.nodes[3].getnewaddress(), 7)
        self.sync_mempools(self.running())
        self.bmm(3, "B")
        self.check_in_sync()
        assert_equal(self.nodes[3].gettransaction(paid)["confirmations"], 1)
        for i in range(3):
            self.bmm(i, "A" if FOLLOWS[i] == "A" else "B")
        tip = self.check_in_sync()

        self.log.info("A withdrawal bundle reaches both mainchain nodes, which vote it through")
        payout = a.wallet.getnewaddress()
        withdrawal = self.nodes[3].createwithdrawal(payout, 5)
        self.sync_mempools(self.running())
        self.bmm(1, "A")
        self.bmm(2, "B")
        self.check_in_sync()
        bundle = self.nodes[0].getwithdrawalbundle()
        assert_equal(bundle["status"], "pending")
        # Each sidechain node hands the bundle to its own mainchain node, which proposes it and upvotes it.
        self.wait_until(lambda: any(x["hash"] == bundle["hash"] and x["known"] for x in a.node.listwithdrawalbundles(SLOT)) or
                        len(a.node.listwithdrawalbundles(SLOT)) == 0)
        if not a.node.listwithdrawalbundles(SLOT):
            a.generate()
            self.sync_mains()
        for _ in range(WITHDRAWAL_MIN_SCORE + 2):
            (a if a.node.getblockcount() % 2 else b).generate()
            self.sync_mains()
            if a.node.getwithdrawalbundle(SLOT, bundle["hash"])["status"] == "paid":
                break
        assert_equal(a.node.getwithdrawalbundle(SLOT, bundle["hash"])["status"], "paid")
        assert_equal(a.wallet.getreceivedbyaddress(payout), 5)
        self.bmm(0, "A")
        self.check_in_sync()
        assert_equal(self.nodes[0].listwithdrawals(), [])
        del withdrawal

        self.log.info("The mainchain splits; each half commits to blocks of its own")
        before = self.check_in_sync()
        self.split_mains()
        a_blocks = [self.bmm(0, "A", sync=False) for _ in range(2)]
        b_blocks = [self.bmm(2, "B", sync=False) for _ in range(3)]
        b.generate(1)
        # The nodes that follow A are on A's blocks, those that follow B on B's.
        self.follow()
        for i in (0, 1):
            assert_equal(self.nodes[i].getbestblockhash(), a_blocks[-1])
        for i in (2, 3):
            assert_equal(self.nodes[i].getbestblockhash(), b_blocks[-1])
        assert before != a_blocks[-1]

        self.log.info("The halves join; B has more work, and every sidechain node goes over to B's blocks")
        self.connect_mains()
        self.sync_mains()
        assert_equal(a.node.getbestblockhash(), b.node.getbestblockhash())
        tip = self.check_in_sync()
        assert_equal(tip, b_blocks[-1])
        for i in (0, 1):
            for block in a_blocks:
                assert block not in [t["hash"] for t in self.nodes[i].getchaintips() if t["status"] == "active"]
        # The chain goes on from there, through either half.
        self.bmm(1, "A")
        self.bmm(3, "B")
        self.check_in_sync()

        self.log.info("Mainchain node B stops; the nodes that follow it hold what they cannot check")
        held_by = [2, 3]
        b.stop()
        self.bmm(0, "A", sync=False)
        self.bmm(1, "A", sync=False)
        tip = self.nodes[0].getbestblockhash()
        self.check_in_sync([self.nodes[0], self.nodes[1]])
        for i in held_by:
            # The follower learns it at its next poll.
            self.wait_until(lambda: self.nodes[i].getmainchaininfo()["connected"] is False, timeout=60)
            assert tip != self.nodes[i].getbestblockhash()
        b.start()
        self.connect_mains()
        self.sync_mains()
        assert_equal(self.check_in_sync(), tip)

        self.log.info("A node that joins late syncs the whole chain from its peers")
        self.start_node(LATE)
        self.connect_nodes(LATE, 3)
        self.nodes[LATE].createwallet("side")
        self.address[LATE] = self.nodes[LATE].getnewaddress()
        self.check_in_sync()
        self.bmm(LATE, "A")
        self.check_in_sync()

        self.log.info("A node restarted, and a node reindexed, come back to the same state")
        self.restart_node(1)
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.restart_node(2, extra_args=self.extra_args[2] + ["-reindex"])
        self.connect_nodes(1, 2)
        self.connect_nodes(2, 3)
        self.check_in_sync()
        self.bmm(2, "B")
        self.bmm(0, "A")
        self.check_in_sync()
        assert_greater_than(self.nodes[0].getblockcount(), 15)


if __name__ == "__main__":
    SidechainNetworkTest(__file__).main()
