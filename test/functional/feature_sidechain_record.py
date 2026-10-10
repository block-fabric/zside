#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""How a sidechain node follows the record of the mainchain as it changes, and talks to its mainchain node.

The mainchain node is a stand-in (test_framework/sidechain_standin.py), which the test makes reorganise,
move or drop commitments, refuse calls, and stop answering at a chosen moment: the record of a node can
then disagree with its chain when it starts, as when a node stops between learning of a change of the
mainchain and acting on it.

- A commitment dropped, then back: the block is taken off the chain, then checked again.
- A candidate block whose parent lost its commitment is not accepted.
- At startup, with a record that disagrees: VerifyDB stops at the block; blocks at the top without a
  commitment go, a block whose commitment moved is checked again, blocks failed against the record that
  have their commitment again are checked again; rebuilt under other parameters, the sidechain database
  stops at the block (the chain goes back), and a block the operator invalidated stays invalid.
- A mainchain node that is syncing, too old, without the sidechain, refusing the credentials, answering
  what is no JSON-RPC or blocks that do not follow each other, refusing a bundle or the word of this chain.
- Blind merged mining: the wallet named with characters to encode, requests that fail, an outbid node,
  a block whose fees do not pay for the request, a saved setting that is no longer valid.
- Connecting with a cookie file.
"""

import json
import os
import time
from decimal import Decimal

from test_framework.sidechain_standin import OUTBID, StandInMainchain
from test_framework.test_framework import BitcoinTestFramework
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, assert_raises_rpc_error, rpc_port

SLOT = 3
BUNDLE_RETRY_DELAY = 5


class SidechainRecordTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.connection_args = [f"-sidechainslot={SLOT}", f"-mainchainrpcport={self.main_rpc_port}", "-mainchainrpcwallet=my wallet"]
        self.extra_args = [self.connection_args + ["-mainchainrpcuser=main", "-mainchainrpcpassword=secret"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.main = StandInMainchain(SLOT)
        self.main.serve(self.main_rpc_port)
        self.setup_nodes()

    def run_test(self):
        try:
            self.run_record_test()
        finally:
            self.main.shutdown()

    #
    # Helpers
    #

    def commit(self, block_hash):
        self.main.add_block(bmm=block_hash)
        self.node.syncmainchain()

    def mine(self):
        candidate = self.node.createbmmblock(self.address)
        self.commit(candidate["hash"])
        assert_equal(self.node.getbestblockhash(), candidate["hash"])
        return candidate["hash"]

    def deposit(self, amount):
        return {"destination": self.address, "amount": str(amount), "txid": "cd" * 32, "burnindex": 0}

    def sync_error(self, message):
        assert_raises_rpc_error(-1, message, self.node.syncmainchain)

    def status(self, block_hash):
        return {tip["hash"]: tip["status"] for tip in self.node.getchaintips()}.get(block_hash)

    def change_unacted(self, change):
        """Have the mainchain change, and the node learn of it without acting on it: the stand-in stops
        answering after the node has the new blocks on record (the first call of an update asks for the
        block at the height of the record, so the new chain is made longer than that)."""
        recorded = self.node.getmainchaininfo()["height"]
        self.pad_branch += 1
        with self.main.lock:
            change()
            while self.main.height() <= recorded:
                self.main.add_block(branch=self.pad_branch)
            self.main.fail_from = self.main.height()
        self.sync_error("the stand-in is not answering this now")
        assert_equal(self.node.getmainchaininfo()["height"], self.main.height())

    def reorg(self, height, count=1, branch=1, **kwargs):
        """The mainchain drops its blocks from `height` on, and has `count` others there instead."""
        def change():
            del self.main.blocks[height:]
            for i in range(count):
                self.main.add_block(branch=branch, **(kwargs if i == 0 else {}))
        return change

    #
    # The test
    #

    def run_record_test(self):
        self.node = node = self.nodes[0]
        main = self.main
        self.address = node.getnewaddress()
        self.pad_branch = 0x40
        for _ in range(3):
            main.add_block()
        main.add_block(deposits=[self.deposit(20)])
        node.syncmainchain()
        blocks = [self.mine() for _ in range(5)]
        assert_equal(node.getbalance(), 20)

        self.log.info("A commitment the mainchain drops takes its block off the chain; back, the block is checked again")
        tip = blocks[-1]
        committed_at = main.height()
        saved = [dict(b) for b in main.blocks]
        with main.lock:
            self.reorg(committed_at, count=2)()
        node.syncmainchain()
        assert_equal(node.getbestblockhash(), blocks[-2])
        assert_equal(self.status(tip), "invalid")
        with main.lock:
            main.blocks = [dict(b) for b in saved]
        with node.assert_debug_log([f"Block {tip} has a commitment on the mainchain again"]):
            node.syncmainchain()
        assert_equal(node.getbestblockhash(), tip)

        self.log.info("A mainchain node that is syncing is not followed back")
        main.add_block()
        node.syncmainchain()
        with main.lock:
            main.ibd = True
            self.reorg(main.height(), branch=2)()
        self.sync_error("The mainchain node is still syncing")
        with main.lock:
            main.ibd = False
        node.syncmainchain()
        assert_equal(node.getmainchaininfo()["bestblockhash"], main.blocks[-1]["hash"])

        self.log.info("Blocks that do not follow each other: those before are taken, then the node asks again later")
        with main.lock:
            main.add_block()
            main.add_block()
            main.stray_at = main.height()
        self.sync_error(f"The mainchain node sent blocks that do not follow each other (at height {main.height()})")
        assert_equal(node.getmainchaininfo()["height"], main.height() - 1)
        main.stray_at = None
        node.syncmainchain()
        assert_equal(node.getmainchaininfo()["height"], main.height())

        self.log.info("A mainchain node too old to say what bundles blocks propose is not followed")
        with main.lock:
            main.no_proposed = True
            main.add_block()
        self.sync_error("it does not report the withdrawal bundles that blocks propose")
        assert_equal(node.getmainchaininfo()["height"], main.height() - 1)
        main.no_proposed = False
        node.syncmainchain()

        self.log.info("A deposit of more coins than can exist is no amount: the record does not take it (no block can have it to pay)")
        with main.lock:
            main.add_block(deposits=[self.deposit(21000001)])
        self.sync_error("the mainchain node sent something that is not an amount (Amount out of range)")
        assert_equal(node.getmainchaininfo()["height"], main.height() - 1)
        with main.lock:
            del main.blocks[-1]
        node.syncmainchain()

        self.log.info("A mainchain without the sidechain in its slot: the node waits")
        with main.lock:
            main.sidechain = None
            main.add_block()
        self.sync_error(f"The mainchain has no sidechain in slot {SLOT}")
        main.sidechain = {"activationheight": 1, "proposalhash": "aa" * 32}
        node.syncmainchain()

        self.log.info("Answers that are no JSON-RPC, and refused credentials")
        main.statuses["getsidechainevents"] = 500
        self.sync_error("unexpected reply with HTTP status 500")
        main.statuses["getsidechainevents"] = 403
        self.sync_error("does not allow connections from this address")
        main.statuses.clear()
        main.credentials = "main:another"
        self.sync_error("The mainchain node refused the credentials")
        assert "refused the credentials" in node.getmainchaininfo()["error"]
        main.credentials = None
        node.syncmainchain()

        self.log.info("Blind merged mining by hand: the wallet's name, encoded; requests that fail")
        asked = node.requestbmmblock(self.address, Decimal("0.001"))
        assert_equal(main.paths["createbmmrequest"], "/wallet/my%20wallet")
        assert_equal(main.requests[-1][:2], [SLOT, asked["hash"]])
        self.commit(asked["hash"])
        assert_equal(node.getbestblockhash(), asked["hash"])
        # The mainchain moved meanwhile: the block is given up, and a commitment to it does nothing.
        main.request_prev = "ff" * 32
        assert_raises_rpc_error(-1, "The mainchain moved while the request was made", node.requestbmmblock, self.address, Decimal("0.001"))
        main.request_prev = None
        given_up = main.requests[-1][1]
        tip = node.getbestblockhash()
        self.commit(given_up)
        assert_equal(node.getbestblockhash(), tip)
        main.errors["createbmmrequest"] = "Insufficient funds"
        assert_raises_rpc_error(-1, "The mainchain node answered createbmmrequest with an error: Insufficient funds", node.requestbmmblock, self.address, Decimal("0.001"))
        # Outbid: the mainchain wallet's own words.
        main.errors["createbmmrequest"] = OUTBID
        try:
            node.requestbmmblock(self.address, Decimal("0.001"))
            raise AssertionError("no error")
        except JSONRPCException as e:
            assert_equal(e.error["message"], OUTBID)

        self.log.info("Mining continuously, outbid: counted, and asked again only with the next mainchain block")
        calls = main.calls.get("createbmmrequest", 0)
        node.setbmm(True, self.address, True, Decimal("0.0005"))
        self.wait_until(lambda: node.getbmminfo()["outbid"] == 1)
        time.sleep(3)
        mining = node.getbmminfo()
        assert_equal((mining["outbid"], mining["requests"], "error" in mining), (1, 0, False))
        assert_equal(main.calls["createbmmrequest"], calls + 1)
        main.add_block()
        self.wait_until(lambda: node.getbmminfo()["outbid"] == 2)
        del main.errors["createbmmrequest"]
        main.add_block()
        self.wait_until(lambda: node.getbmminfo()["requests"] == 1)
        node.setbmm(False)
        self.commit(main.requests[-1][1])
        assert_equal(node.getbestblockhash(), main.requests[-1][1])

        self.log.info("Mining by itself, a block whose fees do not pay for the request is not asked for")
        node.setbmm(True, self.address)
        txid = node.sendtoaddress(node.getnewaddress(), 1, fee_rate=1)
        requests = main.calls["createbmmrequest"]
        self.wait_until(lambda: node.getbmminfo()["idle"])
        time.sleep(2)
        assert_equal(node.getbmminfo()["idle"], True)
        assert_equal(main.calls["createbmmrequest"], requests)
        node.setbmm(False)
        assert txid in node.getblock(self.mine())["tx"]

        self.log.info("The mainchain node refuses the bundle, or the word of this chain on it")
        main.errors.update({"vouchwithdrawalbundle": "unknown slot", "sendwithdrawalbundle": "not yet"})
        withdrawal = node.createwithdrawal("mainaddress", 1)
        self.mine()
        with node.assert_debug_log(["did not take the word of this chain on its bundle", "did not broadcast the withdrawal of bundle"]):
            self.mine()
        bundle = node.getwithdrawalbundle()
        assert_equal(bundle["status"], "pending")
        assert_equal(main.received[-1], bundle["hex"])
        del main.errors["vouchwithdrawalbundle"]
        node.syncmainchain()
        assert_equal(main.vouched[-1], bundle["hash"])
        # The bundle fails; the next one, the mainchain node refuses.
        main.errors["receivewithdrawalbundle"] = "the slot has too many bundles"
        main.add_block(bundles=[{"hash": bundle["hash"], "paid": False}])
        node.syncmainchain()
        for _ in range(BUNDLE_RETRY_DELAY):
            self.mine()
        with node.assert_debug_log(["The mainchain node did not take the withdrawal bundle"]):
            self.mine()
        retry = node.getwithdrawalbundle()
        assert_equal((retry["status"], [w["txid"] for w in node.listwithdrawals()]), ("pending", [withdrawal["txid"]]))
        assert retry["hex"] not in main.received
        main.errors.clear()
        node.syncmainchain()
        assert_equal(main.received[-1], retry["hex"])

        self.log.info("A block built for the mainchain is not accepted once its parent lost its commitment")
        tip = node.getbestblockhash()
        parent = node.getblockheader(tip)["previousblockhash"]
        candidate = node.createbmmblock(self.address)["hash"]
        saved = [dict(b) for b in main.blocks]
        with main.lock:
            self.reorg(main.height(), branch=3)()
            main.add_block(bmm=candidate, branch=3)
        with node.assert_debug_log([f"The block {candidate} that the mainchain committed to was not accepted"]):
            node.syncmainchain()
        assert_equal(node.getbestblockhash(), parent)
        with main.lock:
            main.blocks = saved
            main.add_block()
        node.syncmainchain()
        assert_equal(node.getbestblockhash(), tip)

        self.log.info("At startup, the record no longer has the commitment of the tip: VerifyDB stops there, the follower takes it off")
        tip = node.getbestblockhash()
        parent = node.getblockheader(tip)["previousblockhash"]
        saved = [dict(b) for b in main.blocks]
        [committed_at] = [b["height"] for b in main.blocks if b.get("bmm") == tip]
        self.change_unacted(self.reorg(committed_at, count=2))
        assert_equal(node.getbestblockhash(), tip)
        self.stop_node(0)
        with node.assert_debug_log(["Verification stopped at height", "the record of the mainchain does not agree with it any more"]):
            self.start_node(0, extra_args=self.extra_args[0] + ["-checklevel=4", "-checkblocks=3"])
        assert_equal(node.getbestblockhash(), tip)
        with node.assert_debug_log([f"Block {tip} and the blocks after it have no commitment on the mainchain"]):
            main.fail_from = None
            node.syncmainchain()
        assert_equal(node.getbestblockhash(), parent)
        assert_equal(self.status(tip), "invalid")

        self.log.info("At startup, a block failed against the record has its commitment again: it is checked again")
        self.change_unacted(lambda: main.blocks.__setitem__(slice(None), [dict(b) for b in saved]))
        self.stop_node(0)
        main.fail_from = None
        with node.assert_debug_log(["Checking again 1 blocks that failed against the record and have a commitment on the mainchain"]):
            self.start_node(0)
            node.syncmainchain()
        self.wait_until(lambda: node.getbestblockhash() == tip)

        self.log.info("At startup, the commitment of the tip moved, after a deposit: the tip is checked again and fails")
        [committed_at] = [b["height"] for b in main.blocks if b.get("bmm") == tip]

        def move():
            del main.blocks[committed_at:]
            main.add_block(branch=4, deposits=[self.deposit(1)])
            main.add_block(branch=4, bmm=tip)
        self.change_unacted(move)
        self.stop_node(0)
        main.fail_from = None
        with node.assert_debug_log([f"The commitment of block {tip} moved on the mainchain"]):
            self.start_node(0)
            node.syncmainchain()
        self.wait_until(lambda: node.getbestblockhash() == parent)
        assert_equal(self.status(tip), "invalid")
        # The next block pays the deposit.
        balance = node.getbalance()
        self.mine()
        assert_equal(node.getbalance(), balance + 1)

        self.log.info("Rebuilt under other parameters, with a record that disagrees: the chain goes back; what the operator invalidated stays so")
        manual = self.mine()
        node.invalidateblock(manual)
        # Not the same block again: one with a transaction.
        node.sendtoaddress(node.getnewaddress(), 1)
        tip = self.mine()
        parent = node.getblockheader(tip)["previousblockhash"]
        self.change_unacted(self.reorg(main.height(), count=2, branch=5))
        self.stop_node(0)
        with node.assert_debug_log(["blocks found invalid under the former drivechain parameters are judged again",
                                    f"Block {tip} at height", "does not fit the record of the mainchain as it is; the chain goes back to height"]):
            self.start_node(0, extra_args=self.extra_args[0] + ["-testdrivechainparam=activation_period@21"])
        assert_equal(node.getbestblockhash(), parent)
        assert_equal(self.status(manual), "invalid")
        main.fail_from = None
        node.syncmainchain()
        self.restart_node(0)
        assert_equal(node.getbestblockhash(), parent)
        assert_equal(self.status(manual), "invalid")
        self.mine()

        self.log.info("The state is the same rebuilt from the blocks")
        # A chain longer than the branch of the block the operator invalidated, which -reindex forgets
        # (as it does on any node): then the two branches would tie.
        self.mine()
        state = node.getsidechainstate()
        for args in (["-reindex-chainstate"], ["-reindex"]):
            self.restart_node(0, extra_args=self.extra_args[0] + args)
            self.wait_until(lambda: node.getbestblockhash() == state["bestblock"])
            assert_equal(node.getsidechainstate(), state)

        self.log.info("Merged mining kept in the settings with an address that is no longer valid is not turned on")
        self.stop_node(0)
        settings = os.path.join(node.chain_path, "settings.json")
        with open(settings, "w") as f:
            json.dump({"bmm": {"address": "nonsense", "always": False, "amount": 10000}}, f)
        with node.assert_debug_log(["Merged mining not turned on: the address kept in the settings is not valid here"]):
            self.start_node(0)
        assert_equal(node.getbmminfo()["mining"], False)

        self.log.info("The credentials in a cookie file")
        cookie = os.path.join(self.options.tmpdir, "mainchain.cookie")
        with open(cookie, "w") as f:
            f.write("__cookie__:from-a-file\n")
        main.credentials = "__cookie__:from-a-file"
        self.restart_node(0, extra_args=self.connection_args + [f"-mainchainrpccookiefile={cookie}"])
        node.syncmainchain()
        assert_equal(node.getmainchaininfo()["connected"], True)
        self.restart_node(0, extra_args=self.connection_args + [f"-mainchainrpccookiefile={cookie}.missing"])
        self.sync_error("no credentials: set -mainchainrpcuser and -mainchainrpcpassword, or check that the cookie file")


if __name__ == '__main__':
    SidechainRecordTest(__file__).main()
