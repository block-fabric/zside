#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A new sidechain node follows a mainchain node that pruned blocks before it kept the index of BMM accepts.

Such a mainchain node (a Chains node that pruned before the index existed, and was upgraded) cannot give
the blocks it pruned then: getsidechainevents answers "Block N is not available (pruned?)" for them. A new
sidechain node used to start its record of the mainchain at height 0, and so could never follow it, though
it needs nothing of the mainchain below the block that activated the sidechain in its slot (audit 7 B4). The
record of a new node starts at that block now (SidechainParams::main_activation_height).

- With a stand-in mainchain node (test_framework/sidechain_standin.py) that cannot give the blocks below a
  height: below the activation height, the new node follows it, and mines; above it, the node says why it
  cannot follow, and what to do.
- With a real Chains node (looked for in the directory named by the environment variable
  MAINCHAIN_BIN_DIR, or else in ../chains/build/bin next to this source tree, as feature_sidechain.py
  does; this part is skipped without one that has the index of BMM accepts): pruned, its index dropped
  (-test=drivechain_bmm_unindexed) so that the blocks pruned before are not available, then a sidechain
  activated in the slot. A new sidechain node follows it, mines a block it commits to, and gets a deposit.
"""

import os
import subprocess
import time
from decimal import Decimal
from pathlib import Path

from test_framework.authproxy import AuthServiceProxy, JSONRPCException
from http.client import HTTPException
from test_framework.sidechain_standin import StandInMainchain
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, p2p_port, rpc_port

MAINCHAIN_DAEMON = "chains" + "d"
SLOT = 3
ACTIVATION = 20
# The Chains regtest's activation period of a sidechain proposal (DrivechainParams::activation_period).
CHAINS_ACTIVATION_PERIOD = 20
# A block file of -fastprune holds a few hundred blocks of a regtest chain: pruning keeps that margin.
PRUNE_MARGIN = 300


class SidechainMainchainPrunedTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.main_ports = [rpc_port(self.num_nodes + i) for i in range(2)]
        self.chains_ports = (p2p_port(self.num_nodes + 2), rpc_port(self.num_nodes + 2))
        common = [f"-sidechainslot={SLOT}", "-mainchainrpcuser=main", "-mainchainrpcpassword=secret", "-mainchainrpcwallet=my wallet"]
        self.extra_args = [
            common + [f"-mainchainrpcport={self.main_ports[0]}", f"-sidechainactivationheight={ACTIVATION}"],
            common + [f"-mainchainrpcport={self.main_ports[1]}", f"-sidechainactivationheight={ACTIVATION}"],
            # Its activation height is known once the Chains node activated the slot.
            common + [f"-mainchainrpcport={self.chains_ports[1]}"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.mains = [StandInMainchain(SLOT) for _ in range(2)]
        # (Before the nodes start, which follow their mainchain node from then on.) Node 0's cannot give
        # the blocks below the activation height, node 1's those just above it.
        self.mains[0].unavailable_below = ACTIVATION - 5
        self.mains[1].unavailable_below = ACTIVATION + 5
        for main, port in zip(self.mains, self.main_ports):
            main.sidechain["activationheight"] = ACTIVATION
            for _ in range(ACTIVATION + 10):
                main.add_block()
            main.serve(port)
        # Node 2 is started once the Chains node is ready.
        self.add_nodes(self.num_nodes, self.extra_args)
        for i in range(2):
            self.start_node(i)
            self.nodes[i].createwallet(wallet_name=self.default_wallet_name, load_on_startup=True)

    def run_test(self):
        self.chainsd = None
        try:
            self.test_standin()
            self.test_chains()
        finally:
            for main in self.mains:
                main.shutdown()
            self.stop_chainsd()

    def test_standin(self):
        self.log.info("A mainchain node that cannot give the blocks below the activation height: followed from there")
        node, main = self.nodes[0], self.mains[0]
        node.syncmainchain()
        info = node.getmainchaininfo()
        assert_equal(info["height"], main.height())
        assert_equal(info["bestblockhash"], main.blocks[-1]["hash"])
        # It mines on it, and takes a deposit made after the activation.
        address = node.getnewaddress()
        main.add_block(deposits=[{"destination": address, "amount": "3", "txid": "cd" * 32, "burnindex": 0}])
        node.syncmainchain()
        candidate = node.createbmmblock(node.getnewaddress())
        main.add_block(bmm=candidate["hash"])
        node.syncmainchain()
        assert_equal(node.getbestblockhash(), candidate["hash"])
        assert_equal(node.getbalance(), 3)
        # Restarted, its record starts where it did.
        with node.assert_debug_log([f"Loaded the record of {main.height() - ACTIVATION + 1} mainchain blocks from height {ACTIVATION}"]):
            self.restart_node(0)
        node.syncmainchain()
        assert_equal(node.getbestblockhash(), candidate["hash"])

        self.log.info("A mainchain node that cannot give blocks above the activation height: the node says why, and what to do")
        node, main = self.nodes[1], self.mains[1]
        assert_raises_rpc_error(-1, f"The mainchain node cannot give its blocks from height {ACTIVATION} on (Block {ACTIVATION} is not available (pruned?)). "
                                    f"This node needs every mainchain block from the one that activated this sidechain in its slot (height {ACTIVATION}) on",
                                node.syncmainchain)
        assert_equal(node.getmainchaininfo()["height"], -1)
        # Once it can, the node follows it.
        main.unavailable_below = None
        node.syncmainchain()
        assert_equal(node.getmainchaininfo()["height"], main.height())

    #
    # A real Chains node
    #

    def chains_rpc(self, wallet=None):
        url = f"http://main:secret@127.0.0.1:{self.chains_ports[1]}"
        if wallet is not None:
            url += f"/wallet/{wallet.replace(' ', '%20')}"
        proxy = AuthServiceProxy(url, timeout=120)
        # A connection per call: a node closes a connection left idle for a while.
        proxy.reuse_http_connections = False
        return proxy

    def start_chainsd(self, *extra):
        args = [self.chainsd_path, "-regtest", f"-datadir={self.chains_dir}", f"-port={self.chains_ports[0]}", f"-rpcport={self.chains_ports[1]}",
                "-rpcuser=main", "-rpcpassword=secret", "-listen=0", "-dnsseed=0", "-fixedseeds=0", "-discover=0", "-server=1",
                "-prune=1", "-fastprune", "-fallbackfee=0.0002", "-printtoconsole=0", "-debug=0", *extra]
        self.chainsd = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 120
        while True:
            try:
                self.chains_rpc().getblockcount()
                return
            except (JSONRPCException, OSError, HTTPException) as e:
                if isinstance(e, JSONRPCException) and e.error.get("code") != -28:
                    raise
                if self.chainsd.poll() is not None or time.time() > deadline:
                    raise AssertionError(f"chainsd did not start: {e}")
                time.sleep(0.25)

    def stop_chainsd(self):
        if self.chainsd is None:
            return
        try:
            self.chains_rpc().stop()
            self.chainsd.wait(timeout=120)
        except Exception:
            self.chainsd.kill()
            self.chainsd.wait()
        self.chainsd = None

    def test_chains(self):
        bin_dir = os.environ.get("MAINCHAIN_BIN_DIR", os.path.join(self.config["environment"]["SRCDIR"], "..", "chains", "build", "bin"))
        self.chainsd_path = os.path.join(bin_dir, MAINCHAIN_DAEMON)
        if not os.path.isfile(self.chainsd_path):
            self.log.info("With a real Chains node: skipped (no mainchain node at %s; set MAINCHAIN_BIN_DIR)", self.chainsd_path)
            return
        # (One without the index of BMM accepts serves no pruned block, and cannot drop its index.)
        if "drivechain_bmm_unindexed" not in subprocess.run([self.chainsd_path, "-help-debug"], capture_output=True, text=True).stdout:
            self.log.info("With a real Chains node: skipped (%s has no index of BMM accepts)", self.chainsd_path)
            return
        self.log.info("With a real Chains node (%s), pruned before it kept the index of BMM accepts", self.chainsd_path)
        self.chains_dir = Path(self.options.tmpdir) / "chainsd"
        self.chains_dir.mkdir()
        self.start_chainsd()
        chains = self.chains_rpc()
        chains.createwallet(wallet_name="my wallet", load_on_startup=True)
        wallet = self.chains_rpc("my wallet")
        mine_to = wallet.getnewaddress()

        def mine(count):
            return chains.generatetoaddress(count, mine_to)
        mine(101 + 2 * PRUNE_MARGIN)
        chains.pruneblockchain(PRUNE_MARGIN)
        # As a node that pruned before it kept the index: the blocks pruned are no longer available.
        self.stop_chainsd()
        self.start_chainsd("-test=drivechain_bmm_unindexed")
        chains, wallet = self.chains_rpc(), self.chains_rpc("my wallet")
        pruned = chains.getblockchaininfo()["pruneheight"]
        assert pruned > 1
        assert_raises_rpc_error(-1, "is not available (pruned?)", chains.getsidechainevents, SLOT, 1)

        self.log.info("A sidechain activated in the slot after that")
        wallet.createsidechainproposal(SLOT, "Testchain", "A sidechain for testing", "11" * 32, "22" * 20)
        mine(CHAINS_ACTIVATION_PERIOD + 1)
        activation = chains.getsidechain(SLOT)["activationheight"]
        assert activation > pruned
        self.log.info("Its activation height %d, the blocks below %d pruned: a new sidechain node follows the Chains node", activation, pruned)
        node = self.nodes[2]
        self.extra_args[2].append(f"-sidechainactivationheight={activation}")
        self.start_node(2, self.extra_args[2])
        node.createwallet(wallet_name=self.default_wallet_name, load_on_startup=True)
        node.syncmainchain()
        assert_equal(node.getmainchaininfo()["height"], chains.getblockcount())
        assert_equal(node.getmainchaininfo()["bestblockhash"], chains.getbestblockhash())

        self.log.info("It mines a block the Chains node commits to, and gets a deposit")
        deposit_address = node.getdepositaddress()["depositaddress"]
        candidate = node.createbmmblock(node.getnewaddress())
        wallet.createbmmrequest(SLOT, candidate["hash"], Decimal("0.001"))
        mine(1)
        node.syncmainchain()
        assert_equal(node.getbestblockhash(), candidate["hash"])
        wallet.createsidechaindeposit(SLOT, deposit_address, Decimal("2"))
        mine(1)
        node.syncmainchain()
        candidate = node.createbmmblock(node.getnewaddress())
        wallet.createbmmrequest(SLOT, candidate["hash"], Decimal("0.001"))
        mine(1)
        node.syncmainchain()
        assert_equal(node.getbestblockhash(), candidate["hash"])
        assert_equal(node.getbalance(), 2)
        self.stop_node(2)
        self.stop_chainsd()


if __name__ == '__main__':
    SidechainMainchainPrunedTest(__file__).main()
