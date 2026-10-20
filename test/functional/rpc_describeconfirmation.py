#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The host of the pages over RPC (src/webui/remotehost.h), as the Chains app uses it to show this chain's pages.

- describeconfirmation describes a confirmed call with this chain's own describers and runs nothing;
- webuihost runs a request of the pages: what needs the user's word answers {confirm} with a one-time token, and
  runs only when sent again exactly, with that token: never twice (replay), never with other parameters (tampered),
  never for another wallet under the same name, never past its time;
- the passphrase of a locked wallet comes with the answer, and the wallet is locked again after;
- the pages' RPC policy holds (a spend is not an RPC method the pages call);
- webuievents gives the events of the node in order.
"""

import os
import shutil

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

REFUSED = -32802
NOT_FOUND = -32601


class DescribeConfirmationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        node.createwallet("main")
        w = node.get_wallet_rpc("main")
        address = w.getnewaddress()
        self.generatetoaddress(node, 101, address)

        self.log.info("describeconfirmation describes, runs nothing")
        d = node.describeconfirmation("signmessage", [address, "hello"], "main")
        assert_equal(d["needed"], True)
        assert_equal(d["title"], "Sign a message")
        assert_equal(len(d["token"]), 64)
        assert_equal(d["expiresIn"], 300)
        rows = {r["label"]: r for r in d["rows"]}
        assert_equal(rows["Address"]["value"], address)
        assert_equal(rows["Address"]["kind"], "address")
        assert_equal(rows["Message"]["value"], "hello")
        assert_equal(rows["Wallet"]["value"], "main")
        assert_equal(d["request"], {"kind": "host", "method": "rpc.confirmed", "params": {"method": "signmessage", "params": [address, "hello"], "wallet": "main"}})
        # The wallet of the URI, when not given.
        assert_equal(w.describeconfirmation("signmessage", [address, "hello"])["request"]["params"]["wallet"], "main")
        # A method that reads is not one to confirm.
        assert_raises_rpc_error(-8, "not a method that is confirmed", node.describeconfirmation, "getblockcount", [])

        self.log.info("the token runs exactly that call, once")
        signature = node.webuihost(d["request"], d["token"])["result"]
        assert node.verifymessage(address, signature, "hello")
        assert_raises_rpc_error(REFUSED, "not known", node.webuihost, d["request"], d["token"])
        # Without a token: the question again, never the call.
        again = node.webuihost(d["request"])
        assert "result" not in again and again["confirm"]["title"] == "Sign a message"

        self.log.info("a tampered call is refused, and its token is used up")
        d = node.describeconfirmation("signmessage", [address, "hello"], "main")
        tampered = {"kind": "host", "method": "rpc.confirmed", "params": {"method": "signmessage", "params": [address, "pay me"], "wallet": "main"}}
        assert_raises_rpc_error(REFUSED, "not the one that was confirmed", node.webuihost, tampered, d["token"])
        assert_raises_rpc_error(REFUSED, "not known", node.webuihost, d["request"], d["token"])
        # Another method with the token of a signature.
        d = node.describeconfirmation("signmessage", [address, "hello"], "main")
        other = {"kind": "host", "method": "rpc.confirmed", "params": {"method": "signmessage", "params": [address, "hello"], "wallet": "spare"}}
        node.createwallet("spare")
        assert_raises_rpc_error(REFUSED, "not the one that was confirmed", node.webuihost, other, d["token"])

        self.log.info("another wallet loaded under the same name is not the one confirmed")
        node.createwallet("swap")
        swap = node.get_wallet_rpc("swap")
        swap_address = swap.getnewaddress()
        d = node.describeconfirmation("signmessage", [swap_address, "hello"], "swap")
        node.unloadwallet("swap")
        shutil.rmtree(node.wallets_path / "swap")
        node.createwallet("swap")
        assert_raises_rpc_error(REFUSED, "not the one the call was confirmed for", node.webuihost, d["request"], d["token"])

        self.log.info("a send of the pages: prepared, asked, sent once")
        to = node.get_wallet_rpc("spare").getnewaddress()
        prepared = node.webuihost({"kind": "host", "method": "wallet.send.prepare", "params": {"wallet": "main", "recipients": [{"address": to, "amount": 150000000}]}})["result"]
        commit = {"kind": "host", "method": "wallet.send.commit", "params": {"wallet": "main", "token": prepared["token"]}}
        ask = node.webuihost(commit)["confirm"]
        assert_equal(ask["title"], "Send coins")
        assert_equal(ask["passphrase"], False)
        values = [r["value"] for r in ask["rows"]]
        assert to in values
        assert ask["rows"][values.index(to) + 1]["value"].startswith("1.50000000 ")
        # The question was carried out, not answered: the send is still prepared, asked again the same.
        ask2 = node.webuihost(commit)["confirm"]
        assert_equal(ask2["digest"], ask["digest"])
        sent = node.webuihost(commit, ask2["token"])["result"]
        assert sent["complete"]
        assert sent["txid"] in node.getrawmempool()
        # Used up: the old token, and the prepared send.
        assert_raises_rpc_error(-1, "no longer prepared", node.webuihost, commit, ask["token"])

        self.log.info("a locked wallet: the passphrase comes with the answer")
        w.encryptwallet("secret phrase")
        prepared = node.webuihost({"kind": "host", "method": "wallet.send.prepare", "params": {"wallet": "main", "recipients": [{"address": to, "amount": 20000000}]}})["result"]
        commit = {"kind": "host", "method": "wallet.send.commit", "params": {"wallet": "main", "token": prepared["token"]}}
        ask = node.webuihost(commit)["confirm"]
        assert_equal(ask["passphrase"], True)
        assert_raises_rpc_error(-14, "incorrect", node.webuihost, commit, ask["token"], "wrong")
        prepared = node.webuihost({"kind": "host", "method": "wallet.send.prepare", "params": {"wallet": "main", "recipients": [{"address": to, "amount": 20000000}]}})["result"]
        commit = {"kind": "host", "method": "wallet.send.commit", "params": {"wallet": "main", "token": prepared["token"]}}
        ask = node.webuihost(commit)["confirm"]
        sent = node.webuihost(commit, ask["token"], "secret phrase")["result"]
        assert sent["txid"] in node.getrawmempool()
        assert_equal(w.getwalletinfo()["unlocked_until"], 0)

        self.log.info("the pages' RPC policy")
        assert_equal(node.webuihost({"kind": "rpc", "method": "getblockcount", "params": []})["result"], 101)
        assert_raises_rpc_error(NOT_FOUND, "does not call sendtoaddress", node.webuihost, {"kind": "rpc", "method": "sendtoaddress", "params": [to, 1], "wallet": "main"})
        assert_raises_rpc_error(-8, "no field", node.webuihost, {"kind": "rpc", "method": "getblockcount", "extra": 1})
        assert_raises_rpc_error(NOT_FOUND, "No host method", node.webuihost, {"kind": "host", "method": "window.close", "params": {}})
        hello = node.webuihost({"kind": "host", "method": "app.hello", "params": {}})["result"]
        assert_equal(hello["phase"], "ready")
        assert_equal(hello["info"]["embedded"], True)
        assert_equal(hello["info"]["network"], "regtest")

        self.log.info("files: only those chosen in the app's chooser, by token, never a path of the pages")
        backup = os.path.realpath(os.path.join(self.options.tmpdir, "backup-main.dat"))
        request = {"kind": "host", "method": "wallet.backup", "params": {"wallet": "main", "file": backup}}
        # A path the pages name: refused, nothing written.
        assert_raises_rpc_error(-32600, "Only a file chosen by the user can be written", node.webuihost, request)
        # A path given where a token goes: refused.
        assert_raises_rpc_error(-8, "a token of a file chosen in the app is needed", node.webuihost, request, "", "", [backup])
        assert not os.path.exists(backup)
        # A link or ".." in the path: not taken.
        assert_raises_rpc_error(-8, "not a canonical path", node.webuichosenfile, os.path.join(os.path.dirname(backup), "x", "..", "backup-main.dat"), True)
        assert_raises_rpc_error(-8, "not an absolute path", node.webuichosenfile, "backup-main.dat", True)
        # Chosen to be read, then written: refused.
        read_only = node.webuichosenfile(backup, False)
        assert_raises_rpc_error(-32600, "Only a file chosen by the user can be written", node.webuihost, request, "", "", [read_only])
        # Chosen to be written: written, once.
        token = node.webuichosenfile(backup, True)
        assert_equal(node.webuihost(request, "", "", [token])["result"], backup)
        assert os.path.exists(backup)
        assert_raises_rpc_error(-8, "unknown, used or expired", node.webuihost, request, "", "", [token])

        self.log.info("the events of the node, in order")
        first = node.webuievents(0, 0)
        self.generatetoaddress(node, 1, address)
        later = node.webuievents(first["seq"], 10000)
        assert later["seq"] > first["seq"]
        assert any('"tip"' in batch for batch in later["batches"])
        assert_equal(node.webuievents(10**9, 0)["reset"], True)


if __name__ == "__main__":
    DescribeConfirmationTest(__file__).main()
