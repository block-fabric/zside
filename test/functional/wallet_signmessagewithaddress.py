#!/usr/bin/env python3
# Copyright (c) 2016-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test Wallet commands for signing and verifying messages."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

class SignMessagesWithAddressTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-addresstype=legacy"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        message = 'This is just a test message'

        self.log.info('test signing with an address with wallet')
        address = self.nodes[0].getnewaddress()
        signature = self.nodes[0].signmessage(address, message)
        assert self.nodes[0].verifymessage(address, signature, message)

        self.log.info('test verifying with another address should not work')
        other_address = self.nodes[0].getnewaddress()
        other_signature = self.nodes[0].signmessage(other_address, message)
        assert not self.nodes[0].verifymessage(other_address, signature, message)
        assert not self.nodes[0].verifymessage(address, other_signature, message)

        self.log.info('test signing with P2WPKH and P2TR addresses')
        for address_type in ['bech32', 'bech32m']:
            typed_address = self.nodes[0].getnewaddress(address_type=address_type)
            typed_signature = self.nodes[0].signmessage(typed_address, message)
            assert self.nodes[0].verifymessage(typed_address, typed_signature, message)
            assert not self.nodes[0].verifymessage(typed_address, typed_signature, message + '!')
            assert not self.nodes[0].verifymessage(self.nodes[0].getnewaddress(address_type=address_type), typed_signature, message)
            assert not self.nodes[0].verifymessage(typed_address, signature, message)
        self.log.info('test signing with the key of a P2TR address the wallet does not have')
        self.nodes[0].createwallet("other")
        other = self.nodes[0].get_wallet_rpc("other")
        wallet = self.nodes[0].get_wallet_rpc(self.default_wallet_name)
        foreign = other.getnewaddress(address_type='bech32m')
        assert_raises_rpc_error(-4, "Private key not available", wallet.signmessage, foreign, message)
        self.log.info('test signing with a P2TR address of a wallet without private keys')
        self.nodes[0].createwallet("watch", disable_private_keys=True)
        watch = self.nodes[0].get_wallet_rpc("watch")
        assert_equal(watch.importdescriptors([{"desc": other.getaddressinfo(foreign)["desc"], "timestamp": "now"}])[0]["success"], True)
        assert_equal(watch.getaddressinfo(foreign)["ismine"], True)
        assert_raises_rpc_error(-4, "Private key not available", watch.signmessage, foreign, message)
        assert other.verifymessage(foreign, other.signmessage(foreign, message), message)
        self.nodes[0].unloadwallet("other")
        self.nodes[0].unloadwallet("watch")

        # An address of scripts stands for no single key
        assert_raises_rpc_error(-3, "Address does not refer to key", self.nodes[0].signmessage, self.nodes[0].getnewaddress(address_type='p2sh-segwit'), message)

        self.log.info('test parameter validity and error codes')
        # signmessage has two required parameters
        for num_params in [0, 1, 3, 4, 5]:
            param_list = ["dummy"]*num_params
            assert_raises_rpc_error(-1, "signmessage", self.nodes[0].signmessage, *param_list)
        # invalid key or address provided
        assert_raises_rpc_error(-5, "Invalid address", self.nodes[0].signmessage, "invalid_addr", message)


if __name__ == '__main__':
    SignMessagesWithAddressTest(__file__).main()
