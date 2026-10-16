#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A new sidechain node catches up on a long chain from a single peer, its record of the mainchain behind.

A node just started has no record of the mainchain yet: the headers of its peer come before the
commitments to them are on record, so they wait (bmm-unknown). Of a headers message, the node keeps
16 that wait; it used to take those once their commitments were on record and never ask the peer for
the rest: with one peer (-connect), and no new block announced, it stopped at height 16 for good. This
is what happened to fresh Thunder and Hivemind nodes syncing from the testnet hub.

Two stand-in mainchain nodes (test_framework/sidechain_standin.py), one per sidechain node, so that the
record of the new node can be held behind while its peer sends its headers:

- node 0 mines a chain of more than 16 blocks;
- node 1 starts with a record of the mainchain without the commitments to them, and connects to node 0
  only: node 0's headers wait;
- node 1's mainchain catches up: node 1 takes the headers kept, fetches their blocks, asks node 0 for
  the rest of its headers, and ends on node 0's tip, node 0 announcing nothing new meanwhile;
- the same with a chain longer than a headers message (2000 headers): every message cut short.
"""

import copy

from test_framework.sidechain_standin import StandInMainchain
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, rpc_port

SLOT = 3
# The headers of a message a node keeps while they wait for their commitment (MAX_WAITING_PER_MESSAGE).
WAITING_PER_MESSAGE = 16
# Bitcoin's MAX_HEADERS_RESULTS.
MAX_HEADERS_RESULTS = 2000


class SidechainCatchupTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.main_ports = [rpc_port(self.num_nodes + i) for i in range(self.num_nodes)]
        self.extra_args = [[
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=my wallet",
        ] for port in self.main_ports]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.mains = [StandInMainchain(SLOT) for _ in range(self.num_nodes)]
        for main, port in zip(self.mains, self.main_ports):
            main.serve(port)
        self.setup_nodes()

    def run_test(self):
        try:
            self.run_catchup_test()
        finally:
            for main in self.mains:
                main.shutdown()

    def mine(self, count):
        """Blocks on node 0, each committed to by the next block of its mainchain."""
        node = self.nodes[0]
        for _ in range(count):
            candidate = node.createbmmblock(self.address)
            self.mains[0].add_block(bmm=candidate["hash"])
            node.syncmainchain()
            assert_equal(node.getbestblockhash(), candidate["hash"])

    def catch_up(self, behind):
        """Node 1, its record of the mainchain `behind` blocks short, catches up from node 0 alone."""
        new, old = self.nodes[1], self.nodes[0]
        with self.mains[0].lock, self.mains[1].lock:
            self.mains[1].blocks = copy.deepcopy(self.mains[0].blocks[:len(self.mains[0].blocks) - behind])
        new.syncmainchain()
        start = new.getblockcount()
        assert old.getblockcount() - start > WAITING_PER_MESSAGE
        # Node 0 has nothing new to announce while node 1 catches up.
        tip = old.getbestblockhash()
        # Node 0's headers come before their commitments are on record: they wait.
        with new.assert_debug_log(["bmm-unknown"], timeout=30):
            self.connect_nodes(1, 0)
        assert_equal(new.getblockcount(), start)
        assert_equal(new.getblockchaininfo()["headers"], start)
        # The mainchain of node 1 catches up.
        with self.mains[0].lock, self.mains[1].lock:
            self.mains[1].blocks = copy.deepcopy(self.mains[0].blocks)
        new.syncmainchain()
        self.wait_until(lambda: new.getbestblockhash() == tip, timeout=300)
        assert_equal(old.getbestblockhash(), tip)
        assert_equal(new.getblockchaininfo()["headers"], old.getblockcount())
        self.disconnect_nodes(1, 0)

    def run_catchup_test(self):
        node = self.nodes[0]
        self.address = node.getnewaddress()
        for main in self.mains:
            for _ in range(2):
                main.add_block()
        for n in self.nodes:
            n.syncmainchain()

        self.log.info("A new node whose record of the mainchain is behind catches up on 40 blocks from one peer")
        self.mine(40)
        self.catch_up(behind=40)
        assert_equal(self.nodes[1].getblockcount(), 40)

        self.log.info("The same on more than a headers message: every message cut short, every rest asked for")
        self.mine(MAX_HEADERS_RESULTS + 60)
        self.catch_up(behind=MAX_HEADERS_RESULTS + 60)
        assert_equal(self.nodes[1].getblockcount(), 40 + MAX_HEADERS_RESULTS + 60)


if __name__ == '__main__':
    SidechainCatchupTest(__file__).main()
