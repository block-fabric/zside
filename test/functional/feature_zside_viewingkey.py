#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the viewing keys of the shielded account: exportshieldedviewingkey gives the key of the wallet's
account; importshieldedviewingkey watches it in a wallet without private keys, which then sees what the
account received and paid (the memos too), watch-only, after scanning the blocks again; and keeps it when
it is loaded again. A wallet with private keys, a key of another network or a malformed one are refused."""

from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.util import assert_equal, assert_raises_rpc_error


class ZSideViewingKeyTest(SidechainTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain, fund a wallet, shield, and pay another wallet with a memo")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "zSide", "Private payments")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(), 10)
        self.mine_main()
        self.bmm()
        side.shieldcoins(6, None, "to myself")
        self.sync_mempools()
        self.bmm()
        # (3 deep, as every note a payment spends.)
        self.bmm()
        self.bmm()
        to = other.getshieldedaddress()
        side.sendshielded({to: Decimal("1.5")}, "for the coffee")
        self.sync_mempools()
        self.bmm()
        self.sync_blocks()
        balance = side.getshieldedbalance()["available"]
        assert_equal(other.getshieldedbalance()["available"], Decimal("1.5"))

        self.log.info("Export the viewing key of the wallet's account")
        key = side.exportshieldedviewingkey()
        assert key.startswith("rzview1"), key
        assert_equal(side.exportshieldedviewingkey(), key)

        self.log.info("Refused: a wallet with private keys, a key of another network, a malformed key")
        assert_raises_rpc_error(-4, "without private keys", other.importshieldedviewingkey, key)
        other.createwallet(wallet_name="watch", disable_private_keys=True, blank=True)
        watch = other.get_wallet_rpc("watch")
        assert_raises_rpc_error(-5, "Not a viewing key", watch.importshieldedviewingkey, "zview1" + key[len("rzview1"):])
        assert_raises_rpc_error(-5, "Not a viewing key", watch.importshieldedviewingkey, key[:-1] + ("q" if key[-1] != "q" else "p"))
        assert_raises_rpc_error(-5, "Not a viewing key", watch.importshieldedviewingkey, to)
        assert_raises_rpc_error(-8, "negative", watch.importshieldedviewingkey, key, -1)

        self.log.info("Watch it: the notes, the memos and the payments, watch-only")
        assert_equal(watch.importshieldedviewingkey(key), {"scanned_from": 0})
        seen = watch.getshieldedbalance()
        assert_equal(seen["available"], 0)
        assert_equal(seen["watchonly"], balance)
        notes = watch.listshieldednotes()
        assert len(notes) > 0
        assert all(n["viewonly"] and n["watchonly"] for n in notes)
        assert_equal(sum(n["amount"] for n in notes), balance)
        txs = watch.listshieldedtransactions()
        assert "to myself" in [m for tx in txs for m in tx["memos"]]
        payments = [p for tx in txs for p in tx["payments"]]
        assert {"address": to, "amount": Decimal("1.5"), "memo": "for the coffee", "to_self": False} in payments
        # Nothing to spend: the wallet has no keys.
        assert_raises_rpc_error(None, None, watch.sendshielded, {to: Decimal("0.1")})
        # Again: nothing changes.
        watch.importshieldedviewingkey(key, 5)
        assert_equal(watch.getshieldedbalance()["watchonly"], balance)

        self.log.info("Kept when the wallet is loaded again")
        other.unloadwallet("watch")
        other.loadwallet("watch")
        assert_equal(watch.getshieldedbalance()["watchonly"], balance)
        assert_equal(len(watch.listshieldednotes()), len(notes))

        self.log.info("What the account receives later shows too")
        self.bmm()
        self.bmm()
        self.sync_blocks()
        other.get_wallet_rpc(self.default_wallet_name).sendshielded({side.getshieldedaddress(): Decimal("0.25")}, "back to you")
        self.sync_mempools()
        self.bmm()
        self.sync_blocks()
        assert_equal(watch.getshieldedbalance()["watchonly"], balance + Decimal("0.25"))
        assert "back to you" in [m for tx in watch.listshieldedtransactions() for m in tx["memos"]]


if __name__ == '__main__':
    ZSideViewingKeyTest(__file__).main()
