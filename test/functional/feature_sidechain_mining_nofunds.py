#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test merged mining with a mainchain wallet that cannot pay for commitments.

The only merged miner of the sidechain asks every round for a commitment, which its mainchain
wallet, empty, cannot pay. The failure is reported; the blocks built for requests that were never
made are not kept round after round; and once the wallet has the funds, blocks come again with the
next mainchain block.

The mainchain node is a node of the Chains mainchain, looked for in the directory named by the
environment variable MAINCHAIN_BIN_DIR, or else in ../chains/build/bin next to this source tree.
The test is skipped if there is none.
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
    p2p_port,
    rpc_port,
)

# The node program of the mainchain.
MAINCHAIN_DAEMON = "chains" + "d"

SLOT = 3
# Regtest parameters of the mainchain, see CRegTestParams there.
ACTIVATION_PERIOD = 20
# The blocks the miner may keep while no request goes through: the one of the round, and the one
# before it, whose request may have gone through unanswered.
MAX_UNREQUESTED = 2
# Rounds of mining watched (the follower mines once a second).
ROUNDS = 6


def fresh_connections(proxy):
    """A connection per call: a node closes a connection left idle for a while."""
    proxy.reuse_http_connections = False
    return proxy


class SidechainMiningNoFundsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.extra_args = [[
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
        ]]
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
        args = [
            binary, "-regtest", f"-datadir={datadir}", f"-rpcport={self.main_rpc_port}", f"-port={p2p_port(self.num_nodes)}",
            "-rpcuser=main", "-rpcpassword=secret", "-fallbackfee=0.0002", "-listen=0", "-server", "-printtoconsole=0",
        ]
        self.main_process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
        # The wallet that mines the mainchain, and the empty one the sidechain node asks for commitments.
        node.createwallet("funder")
        node.createwallet("miner")
        self.funder = fresh_connections(AuthServiceProxy(url + "/wallet/funder", timeout=120))
        self.main_miner = fresh_connections(AuthServiceProxy(url + "/wallet/miner", timeout=120))

    @staticmethod
    def kill_mainchain(process):
        if process.poll() is None:
            process.kill()

    def stop_mainchain(self):
        if self.main_process is None:
            return
        try:
            self.funder.stop()
        except (OSError, HTTPException, JSONRPCException):
            self.main_process.terminate()
        self.main_process.wait(timeout=120)
        self.main_process = None

    def setup_network(self):
        # The node of the sidechain follows the mainchain from the moment it starts.
        self.start_mainchain()
        super().setup_network()

    def mine_main(self, count=1):
        hashes = self.funder.generatetoaddress(count, self.main_address)
        for node in self.nodes:
            node.syncmainchain()
        return hashes

    def watch_candidates(self, side):
        """The most blocks the miner kept over some rounds of mining."""
        most = 0
        deadline = time.time() + ROUNDS
        while time.time() < deadline:
            most = max(most, side.getbmminfo()["candidates"])
            time.sleep(0.25)
        return most

    def run_test(self):
        try:
            self.run_nofunds_test()
        finally:
            self.stop_mainchain()

    def run_nofunds_test(self):
        side = self.nodes[0]
        self.main_address = self.funder.getnewaddress()
        self.mine_main(110)
        self.funder.createsidechainproposal(SLOT, "Testchain", "The sidechain of this test", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        assert_equal([s["slot"] for s in self.funder.listactivesidechains()], [SLOT])
        assert_equal(self.main_miner.getbalance(), 0)

        self.log.info("The only merged miner has no funds on the mainchain: the failure is reported")
        side.setbmm(True, side.getnewaddress(), True, Decimal("0.0005"))
        self.wait_until(lambda: "error" in side.getbmminfo())
        mining = side.getbmminfo()
        self.log.info(f"Reported: {mining['error']}")
        assert_equal(mining["requests"], 0)
        assert_equal(mining["blocks"], 0)

        self.log.info("Round after round, the blocks never asked for are not kept")
        assert self.watch_candidates(side) <= MAX_UNREQUESTED
        # Nor across mainchain blocks.
        self.mine_main()
        assert self.watch_candidates(side) <= MAX_UNREQUESTED
        assert_equal(side.getblockcount(), 0)
        assert "error" in side.getbmminfo()
        assert_equal(side.getbmminfo()["requests"], 0)

        self.log.info("Funded, the miner gets a block with the next mainchain block")
        self.funder.sendtoaddress(self.main_miner.getnewaddress(), 1)
        self.mine_main()
        self.wait_until(lambda: side.getbmminfo()["requests"] >= 1)
        assert "error" not in side.getbmminfo()
        self.mine_main()
        self.wait_until(lambda: side.getblockcount() == 1)
        assert_equal(side.getbmminfo()["blocks"], 1)
        # And with each one after.
        self.wait_until(lambda: side.getbmminfo()["requests"] >= 2)
        self.mine_main()
        self.wait_until(lambda: side.getblockcount() == 2)
        assert side.getbmminfo()["candidates"] <= MAX_UNREQUESTED
        side.setbmm(False)


if __name__ == '__main__':
    SidechainMiningNoFundsTest(__file__).main()
