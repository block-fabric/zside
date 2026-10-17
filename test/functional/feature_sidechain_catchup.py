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
- the same with a chain longer than a headers message (2000 headers): every message cut short;
- the record partly caught up (audit 7 B1): the headers of a message whose commitments are on record
  are taken, and the next ones wait. The blocks of those taken are the peer's, and are fetched at once:
  without that, the blocks of the headers that waited, fetched once taken, did not connect, the tip
  never moved, and the node stopped for good with no block at all;
- the waiting set full of headers from many inbound peers that will never be committed to (audit 7
  B2): the headers of a peer this node chose (outbound) are kept; those of an inbound peer may be
  dropped, and that peer is asked again once the record moved on. Without that, the honest peer lost
  its headers to the others and the node stopped for good.
"""

import copy

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CBlockHeader, msg_headers
from test_framework.p2p import P2PInterface
from test_framework.sidechain_standin import StandInMainchain
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, rpc_port

SLOT = 3
# The headers of a message a node keeps while they wait for their commitment (MAX_WAITING_PER_MESSAGE).
WAITING_PER_MESSAGE = 16
# Bitcoin's MAX_HEADERS_RESULTS.
MAX_HEADERS_RESULTS = 2000
# The headers the waiting set keeps (ChainstateManager::MAX_BMM_WAITING).
MAX_BMM_WAITING = 1000


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

    def catch_up(self, behind, on_record=0, flood=None, inbound=False):
        """Node 1, its record of the mainchain `behind` blocks short, catches up from node 0 alone. Of the
        blocks node 1 does not have, the commitments of the first `on_record` are on record. `flood`: so
        many inbound peers each send that many headers that will never be committed to, once node 0's wait.
        `inbound`: node 0 connects to node 1 (an inbound peer of node 1's), not node 1 to node 0."""
        new, old = self.nodes[1], self.nodes[0]
        with self.mains[0].lock, self.mains[1].lock:
            self.mains[1].blocks = copy.deepcopy(self.mains[0].blocks[:len(self.mains[0].blocks) - behind])
        new.syncmainchain()
        start = new.getblockcount()
        assert old.getblockcount() - start - on_record > WAITING_PER_MESSAGE
        # Node 0 has nothing new to announce while node 1 catches up.
        tip = old.getbestblockhash()
        # Node 0's headers come before their commitments are on record: they wait.
        with new.assert_debug_log(["bmm-unknown"], timeout=30):
            if inbound:
                self.connect_nodes(0, 1)
            else:
                self.connect_nodes(1, 0)
        # The headers whose commitments are on record are taken, and their blocks fetched from node 0.
        self.wait_until(lambda: new.getblockcount() == start + on_record, timeout=60)
        assert_equal(new.getblockchaininfo()["headers"], start + on_record)
        flooders = []
        if flood:
            peers, count = flood
            parent = int(new.getbestblockhash(), 16)
            block_time = new.getblock(new.getbestblockhash())["time"] + 1
            height = new.getblockcount() + 1
            for p in range(peers):
                flooder = new.add_p2p_connection(P2PInterface())
                prev, headers = parent, []
                for i in range(count):
                    block = create_block(prev, create_coinbase(height + i), ntime=block_time + p * count + i)
                    block.solve()
                    headers.append(CBlockHeader(block))
                    prev = block.hash_int
                flooder.send_and_ping(msg_headers(headers))
                flooders.append(flooder)
        # The mainchain of node 1 catches up.
        with self.mains[0].lock, self.mains[1].lock:
            self.mains[1].blocks = copy.deepcopy(self.mains[0].blocks)
        new.syncmainchain()
        self.wait_until(lambda: new.getbestblockhash() == tip, timeout=300)
        assert_equal(old.getbestblockhash(), tip)
        assert_equal(new.getblockchaininfo()["headers"], old.getblockcount())
        for flooder in flooders:
            flooder.peer_disconnect()
        if inbound:
            self.disconnect_nodes(0, 1)
        else:
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
        count = 40

        self.log.info("The record partly caught up: the blocks of the headers taken are fetched at once, the rest once on record")
        self.mine(60)
        self.catch_up(behind=40, on_record=20)
        count += 60
        assert_equal(self.nodes[1].getblockcount(), count)

        # Each flooder sends as many headers as node 0's kept (WAITING_PER_MESSAGE): node 0 is no less crowded than any.
        flooders = MAX_BMM_WAITING // WAITING_PER_MESSAGE + 4
        self.log.info("The waiting set full of %d inbound peers' headers that are never committed to: an outbound peer's are kept", flooders)
        self.mine(40)
        self.catch_up(behind=40, flood=(flooders, WAITING_PER_MESSAGE))
        count += 40
        assert_equal(self.nodes[1].getblockcount(), count)

        # One header fewer each, but more of them: node 0, an inbound peer too, is the most crowded one.
        flooders = MAX_BMM_WAITING // (WAITING_PER_MESSAGE - 1) + 4
        self.log.info("The same, %d of them, node 0 an inbound peer: its headers may go, and it is asked again once the record moved on", flooders)
        self.mine(40)
        self.catch_up(behind=40, flood=(flooders, WAITING_PER_MESSAGE - 1), inbound=True)
        count += 40
        assert_equal(self.nodes[1].getblockcount(), count)

        self.log.info("The same on more than a headers message: every message cut short, every rest asked for")
        self.mine(MAX_HEADERS_RESULTS + 60)
        self.catch_up(behind=MAX_HEADERS_RESULTS + 60)
        assert_equal(self.nodes[1].getblockcount(), count + MAX_HEADERS_RESULTS + 60)


if __name__ == '__main__':
    SidechainCatchupTest(__file__).main()
