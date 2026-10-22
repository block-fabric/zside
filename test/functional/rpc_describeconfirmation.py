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
- webuievents gives the events of the node in order;
- one wallet per request: a request bound to one wallet never acts on another;
- a chosen file is the same file when used (a link put in its place after it was chosen is refused);
- what runs by name inside webuihost and describeconfirmation is only what the RPC user may call (-rpcwhitelist).
"""

import hashlib
import hmac
import os
import shutil
import urllib.parse

from test_framework.authproxy import AuthServiceProxy
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

REFUSED = -32802
NOT_FOUND = -32601
NOT_ALLOWED = -32803

# What the Chains app's own user of a sidechain daemon may call (SidechainSupervisor::AppRpcMethods of Chains).
APP_METHODS = [
    "getblockcount", "listwallets", "listwalletdir", "loadwallet", "createwallet", "stop",
    "importdescriptors", "walletpassphrase", "walletlock", "walletpassphrasechange",
    "webuihost", "webuievents", "describeconfirmation", "webuichosenfile",
    "getblockchaininfo", "getbestblockhash", "getblockhash", "getblockheader", "getblock", "getmainchaininfo", "getbmminfo",
    "getnetworkinfo", "getconnectioncount", "getmempoolinfo", "getpeerinfo", "getwalletinfo", "getbalances", "getbalance",
    "listtransactions", "uptime", "getdepositaddress",
]


# What the app's user must also be allowed for the pages' host methods it runs through webuihost (HostPrivileges of
# remotehost.cpp): a spend of the pages, a backup and a restore of a file chosen in its chooser.
APP_NEEDS = ["send", "backupwallet", "restorewallet"]


def rpcauth(user, password):
    salt = os.urandom(16).hex()
    return f"{user}:{salt}${hmac.new(salt.encode(), password.encode(), hashlib.sha256).hexdigest()}"


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
        # A file to be read must be there.
        assert_raises_rpc_error(-8, "no such file", node.webuichosenfile, backup, False)
        # Chosen to be read, then written: refused.
        open(backup, "wb").close()
        read_only = node.webuichosenfile(backup, False)
        assert_raises_rpc_error(-32600, "Only a file chosen by the user can be written", node.webuihost, request, "", "", [read_only])
        # A file there already is written over only when the user agreed (overwrite), and only by a wallet backup.
        assert_raises_rpc_error(-8, "a file is there already", node.webuichosenfile, backup, True)
        token = node.webuichosenfile(backup, True, True)
        export = {"kind": "host", "method": "wallet.exportWatchOnly", "params": {"wallet": "main", "file": backup}}
        assert_raises_rpc_error(-32600, "Only a file chosen by the user can be written", node.webuihost, export, "", "", [token])
        # Chosen to be written: written, once.
        token = node.webuichosenfile(backup, True, True)
        assert_equal(node.webuihost(request, "", "", [token])["result"], backup)
        assert os.path.getsize(backup) > 0
        assert_raises_rpc_error(-8, "unknown, used or expired", node.webuihost, request, "", "", [token])

        self.log.info("the events of the node, in order")
        first = node.webuievents(0, 0)
        self.generatetoaddress(node, 1, address)
        later = node.webuievents(first["seq"], 10000)
        assert later["seq"] > first["seq"]
        assert any('"tip"' in batch for batch in later["batches"])
        assert_equal(node.webuievents(10**9, 0)["reset"], True)

        self.test_one_wallet(node, address)
        self.test_chosen_file_swapped(node)
        self.test_whitelist(address)

    def test_one_wallet(self, node, address):
        self.log.info("one wallet per request: bound to one wallet, never acting on another")
        d = node.describeconfirmation("signmessage", [address, "hello"], "main")
        # The question and token of "main", the request naming "spare" beside it: refused, nothing signed.
        mixed = dict(d["request"], wallet="spare")
        assert_raises_rpc_error(-8, "acts on the wallet of its parameters", node.webuihost, mixed, d["token"])
        mixed = {"kind": "host", "method": "rpc.confirmed", "params": {"method": "signmessage", "params": [address, "hello"], "wallet": "spare"}, "wallet": "main"}
        assert_raises_rpc_error(-8, "acts on the wallet of its parameters", node.webuihost, mixed, d["token"])
        # A host method of a wallet: the wallet beside its parameters must be theirs.
        assert_raises_rpc_error(-8, "acts on the wallet of its parameters", node.webuihost, {"kind": "host", "method": "wallet.lock", "params": {"wallet": "main"}, "wallet": "spare"})
        assert_raises_rpc_error(-8, "acts on the wallet of its parameters", node.webuihost, {"kind": "host", "method": "rpc.confirmed", "params": {"method": "setgenerate", "params": [True]}, "wallet": "main"})
        # The same wallet named in both: the token runs it.
        same = dict(d["request"], wallet="main")
        assert node.verifymessage(address, node.webuihost(same, d["token"], "secret phrase")["result"], "hello")
        # A token of "main" sent for the same call in "spare": refused, and used up.
        d = node.describeconfirmation("signmessage", [address, "hello"], "main")
        other = {"kind": "host", "method": "rpc.confirmed", "params": {"method": "signmessage", "params": [address, "hello"], "wallet": "spare"}}
        assert_raises_rpc_error(REFUSED, "not the one that was confirmed", node.webuihost, other, d["token"])
        assert_raises_rpc_error(REFUSED, "not known", node.webuihost, d["request"], d["token"])
        # describeconfirmation: the wallet of the URI and the one named, the same.
        assert_raises_rpc_error(-8, "differs from the wallet of the URI", node.get_wallet_rpc("main").describeconfirmation, "signmessage", [address, "hello"], "spare")

    def test_chosen_file_swapped(self, node):
        self.log.info("a chosen file swapped for a link after it was chosen: refused, nothing written there")
        folder = os.path.realpath(self.options.tmpdir)
        target = os.path.join(folder, "not-chosen.dat")
        with open(target, "w") as f:
            f.write("keep")
        chosen = os.path.join(folder, "chosen-backup.dat")
        request = {"kind": "host", "method": "wallet.backup", "params": {"wallet": "main", "file": chosen}}
        token = node.webuichosenfile(chosen, True)
        os.symlink(target, chosen)
        assert_raises_rpc_error(-32600, "not the one it was when chosen", node.webuihost, request, "", "", [token])
        with open(target) as f:
            assert_equal(f.read(), "keep")
        os.remove(chosen)
        # An existing file replaced by a link: the same.
        with open(chosen, "w") as f:
            f.write("old backup")
        token = node.webuichosenfile(chosen, True, True)
        os.rename(chosen, chosen + ".moved")
        os.symlink(target, chosen)
        assert_raises_rpc_error(-32600, "not the one it was when chosen", node.webuihost, request, "", "", [token])
        with open(target) as f:
            assert_equal(f.read(), "keep")
        with open(chosen + ".moved") as f:
            assert_equal(f.read(), "old backup")
        os.remove(chosen)
        # Unchanged: written.
        token = node.webuichosenfile(chosen, True)
        assert_equal(node.webuihost(request, "", "", [token])["result"], chosen)
        assert os.path.getsize(chosen) > 0
        assert "2 minutes" in node.help("webuichosenfile")
        # Nothing of the host's own private copies is left behind.
        assert_equal(os.listdir(node.chain_path / "webui-files"), [])

    def user_rpc(self, node, user, password):
        url = urllib.parse.urlparse(node.url)
        return AuthServiceProxy(f"http://{user}:{password}@{url.hostname}:{url.port}", timeout=60)

    def test_whitelist(self, address):
        self.log.info("-rpcwhitelist: what runs inside webuihost and describeconfirmation, only what the user may call")
        self.restart_node(0, extra_args=[
            f"-rpcauth={rpcauth('pages', 'pw1')}",
            "-rpcwhitelist=pages:webuihost,describeconfirmation,getblockcount",
            f"-rpcauth={rpcauth('signer', 'pw2')}",
            "-rpcwhitelist=signer:webuihost,describeconfirmation,signmessage",
            f"-rpcauth={rpcauth('app', 'pw3')}",
            "-rpcwhitelist=app:" + ",".join(APP_METHODS + APP_NEEDS),
            f"-rpcauth={rpcauth('named', 'pw4')}",
            "-rpcwhitelist=named:webuihost,wallet.info,webuichosenfile,backupwallet,restorewallet",
            "-webuichooser=app",
            "-rpcwhitelistdefault=0",
        ])
        node = self.nodes[0]
        node.loadwallet("main")
        node.loadwallet("spare")
        pages = self.user_rpc(node, "pages", "pw1")
        # Directly: only its own.
        assert_raises_rpc_error(None, "403", pages.getblockchaininfo)
        # Through webuihost: a read of the pages it may call runs, one it may not is refused.
        assert_equal(pages.webuihost({"kind": "rpc", "method": "getblockcount", "params": []})["result"], node.getblockcount())
        assert_raises_rpc_error(NOT_ALLOWED, "may not call getblockchaininfo", pages.webuihost, {"kind": "rpc", "method": "getblockchaininfo", "params": []})
        assert_raises_rpc_error(NOT_ALLOWED, "may not call getbalances", pages.webuihost, {"kind": "rpc", "method": "getbalances", "params": [], "wallet": "main"})
        # The calls of rpc.confirmed: refused before any question.
        confirmed = {"kind": "host", "method": "rpc.confirmed", "params": {"method": "signmessage", "params": [address, "hello"], "wallet": "main"}}
        assert_raises_rpc_error(NOT_ALLOWED, "may not call signmessage", pages.webuihost, confirmed)
        assert_raises_rpc_error(NOT_ALLOWED, "may not call signmessage", pages.describeconfirmation, "signmessage", [address, "hello"], "main")
        # The commands of the console, nested ones too.
        out = pages.webuihost({"kind": "host", "method": "console.run", "params": {"line": "getblockchaininfo"}})["result"]
        assert not out["ok"] and "may not call getblockchaininfo" in out["error"]
        out = pages.webuihost({"kind": "host", "method": "console.run", "params": {"line": "getblockcount"}})["result"]
        assert out["ok"]
        # Host methods: only with the privilege of what they do (a spend, a backup, a deposit, merged mining...).
        to = node.get_wallet_rpc("spare").getnewaddress()
        send = {"kind": "host", "method": "wallet.send.prepare", "params": {"wallet": "main", "recipients": [{"address": to, "amount": 10000000}]}}
        assert_raises_rpc_error(NOT_ALLOWED, "may not run wallet.send.prepare", pages.webuihost, send)
        assert_raises_rpc_error(NOT_ALLOWED, "may not run wallet.send.commit", pages.webuihost, {"kind": "host", "method": "wallet.send.commit", "params": {"wallet": "main", "token": "x"}})
        assert_raises_rpc_error(NOT_ALLOWED, "may not run wallet.backup", pages.webuihost, {"kind": "host", "method": "wallet.backup", "params": {"wallet": "main", "file": "/x"}})
        assert_raises_rpc_error(NOT_ALLOWED, "may not run wallet.unlock", pages.webuihost, {"kind": "host", "method": "wallet.unlock", "params": {"wallet": "main", "passphrase": "x"}})
        assert_raises_rpc_error(NOT_ALLOWED, "may not run peers.ban", pages.webuihost, {"kind": "host", "method": "peers.ban", "params": {}})
        assert_raises_rpc_error(NOT_ALLOWED, "may not run wallet.info", pages.webuihost, {"kind": "host", "method": "wallet.info", "params": {"wallet": "main"}})
        assert_raises_rpc_error(NOT_ALLOWED, "may not run some.own", pages.webuihost, {"kind": "host", "method": "some.own", "params": {}})
        # A host method named in the whitelist itself runs; others still not.
        named = self.user_rpc(node, "named", "pw4")
        assert_equal(named.webuihost({"kind": "host", "method": "wallet.info", "params": {"wallet": "main"}})["result"]["name"], "main")
        assert_raises_rpc_error(NOT_ALLOWED, "may not run wallet.unlock", named.webuihost, {"kind": "host", "method": "wallet.unlock", "params": {"wallet": "main", "passphrase": "x"}})
        # Files: only the app's own chooser (-webuichooser) gives them, even to a user that may back up.
        folder = os.path.realpath(self.options.tmpdir)
        assert_raises_rpc_error(NOT_ALLOWED, "Only the app's own chooser", named.webuichosenfile, os.path.join(folder, "named.dat"), True)

        signer = self.user_rpc(node, "signer", "pw2")
        d = signer.describeconfirmation("signmessage", [address, "hello"], "main")
        assert node.verifymessage(address, signer.webuihost(d["request"], d["token"], "secret phrase")["result"], "hello")
        assert_raises_rpc_error(NOT_ALLOWED, "may not call getblockcount", signer.webuihost, {"kind": "rpc", "method": "getblockcount", "params": []})

        self.log.info("the Chains app's own user (its -rpcwhitelist): what it calls still runs")
        app = self.user_rpc(node, "app", "pw3")
        hello = app.webuihost({"kind": "host", "method": "app.hello", "params": {}})["result"]
        assert_equal(hello["phase"], "ready")
        assert_equal(app.webuihost({"kind": "rpc", "method": "getblockcount", "params": []})["result"], node.getblockcount())
        assert "trusted" in app.webuihost({"kind": "rpc", "method": "getbalances", "params": [], "wallet": "main"})["result"]["mine"]
        app.webuievents(0, 0)
        to = node.get_wallet_rpc("spare").getnewaddress()
        prepared = app.webuihost({"kind": "host", "method": "wallet.send.prepare", "params": {"wallet": "main", "recipients": [{"address": to, "amount": 10000000}]}})["result"]
        commit = {"kind": "host", "method": "wallet.send.commit", "params": {"wallet": "main", "token": prepared["token"]}}
        ask = app.webuihost(commit)["confirm"]
        sent = app.webuihost(commit, ask["token"], "secret phrase")["result"]
        assert sent["txid"] in node.getrawmempool()
        backup = os.path.join(os.path.realpath(self.options.tmpdir), "app-backup.dat")
        token = app.webuichosenfile(backup, True)
        backup_request = {"kind": "host", "method": "wallet.backup", "params": {"wallet": "main", "file": backup}}
        # The token is the app's: another user cannot use it.
        assert_raises_rpc_error(-8, "unknown, used or expired", named.webuihost, backup_request, "", "", [token])
        token = app.webuichosenfile(backup, True)
        assert_equal(app.webuihost(backup_request, "", "", [token])["result"], backup)
        assert os.path.getsize(backup) > 0
        # Not in its whitelist: refused through webuihost too.
        assert_raises_rpc_error(NOT_ALLOWED, "may not call signmessage", app.describeconfirmation, "signmessage", [address, "hello"], "main")
        assert_raises_rpc_error(NOT_ALLOWED, "may not call getrawmempool", app.webuihost, {"kind": "rpc", "method": "getrawmempool", "params": []})


if __name__ == "__main__":
    DescribeConfirmationTest(__file__).main()
