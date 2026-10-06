#!/usr/bin/env python3
# Copyright (c) 2020-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test error messages for 'getaddressinfo' and 'validateaddress' RPC commands."""

from test_framework.address import HRP_BY_CHAIN
from test_framework.segwit_addr import Encoding, bech32_encode, convertbits
from test_framework.test_framework import BitcoinTestFramework

from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

# The Bech32 addresses are made here, so that they follow the address prefix of the chain.
HRP = HRP_BY_CHAIN["regtest"]
L = len(HRP)
CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
KEYHASH = bytes.fromhex("5ec3eaf488f0555e43f85c20c96dfa0503715483")
SCRIPTHASH = bytes.fromhex("6a23b20d0c13bdbf9c0fcc8d7fd8869f99f28e9a20ac940b18f8d35bd8262920")


def encode(version, program, encoding):
    return bech32_encode(encoding, HRP, [version] + convertbits(program, 8, 5))


def spoil(address, positions):
    """Replace the characters at the given positions by others."""
    chars = list(address)
    for position in positions:
        other = CHARSET[(CHARSET.index(chars[position].lower()) + 1) % len(CHARSET)]
        chars[position] = other.upper() if address.isupper() else other
    return "".join(chars)


BECH32_VALID = encode(0, KEYHASH, Encoding.BECH32)
BECH32_VALID_UNKNOWN_WITNESS = encode(1, bytes.fromhex("aa55"), Encoding.BECH32M)
BECH32_VALID_CAPITALS = BECH32_VALID.upper()
BECH32_VALID_MULTISIG = encode(0, SCRIPTHASH, Encoding.BECH32)

BECH32_INVALID_BECH32 = encode(1, SCRIPTHASH, Encoding.BECH32)
BECH32_INVALID_BECH32M = encode(0, KEYHASH, Encoding.BECH32M)
BECH32_INVALID_VERSION = encode(17, SCRIPTHASH, Encoding.BECH32M)
BECH32_INVALID_SIZE = encode(16, bytes(41), Encoding.BECH32M)
BECH32_INVALID_V0_SIZE = encode(0, bytes(21), Encoding.BECH32)
BECH32_INVALID_PREFIX = 'bc1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7k7grplx'
BECH32_TOO_LONG = HRP + '1q049edschfnwystcqnsvyfpj23mpsg3jcedq9xv049edschfnwystcqnsvyfpj23mpsg3jcedq9xv049edschfnwystcqnsvyfpj23m'
ONE_ERROR_AT = [L + 5]
BECH32_ONE_ERROR = spoil(BECH32_VALID, ONE_ERROR_AT)
ONE_ERROR_CAPITALS_AT = [L + 34]
BECH32_ONE_ERROR_CAPITALS = spoil(BECH32_VALID_CAPITALS, ONE_ERROR_CAPITALS_AT)
TWO_ERRORS_AT = [L + 18, len(BECH32_VALID) - 1]
BECH32_TWO_ERRORS = spoil(BECH32_VALID, TWO_ERRORS_AT)
BECH32_NO_SEPARATOR = HRP + BECH32_VALID[L + 1:]
INVALID_CHAR_AT = [L + 4]
BECH32_INVALID_CHAR = BECH32_VALID[:L + 4] + 'o' + BECH32_VALID[L + 5:]
MULTISIG_TWO_ERRORS_AT = [L + 15, L + 26]
BECH32_MULTISIG_TWO_ERRORS = spoil(BECH32_VALID_MULTISIG, MULTISIG_TWO_ERRORS_AT)
# The same address with the witness version of another.
WRONG_VERSION_AT = [L + 1]
BECH32_WRONG_VERSION = BECH32_VALID[:L + 1] + 'p' + BECH32_VALID[L + 2:]

BASE58_VALID = 'mipcBbFg9gMiCh81Kj8tqqdgoZub1ZJRfn'
BASE58_INVALID_PREFIX = '17VZNX1SN5NtKa8UQFxwQbFeFc3iqRYhem'
BASE58_INVALID_CHECKSUM = 'mipcBbFg9gMiCh81Kj8tqqdgoZub1ZJJfn'
BASE58_INVALID_LENGTH = '2VKf7XKMrp4bVNVmuRbyCewkP8FhGLP2E54LHDPakr9Sq5mtU2'

INVALID_ADDRESS = 'asfah14i8fajz0123f'
INVALID_ADDRESS_2 = '1q049ldschfnwystcqnsvyfpj23mpsg3jcedq9xv'

class InvalidAddressErrorMessageTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.uses_wallet = None

    def check_valid(self, addr):
        info = self.nodes[0].validateaddress(addr)
        assert info['isvalid']
        assert 'error' not in info
        assert 'error_locations' not in info

    def check_invalid(self, addr, error_str, error_locations=None):
        res = self.nodes[0].validateaddress(addr)
        assert not res['isvalid']
        assert_equal(res['error'], error_str)
        if error_locations:
            assert_equal(res['error_locations'], error_locations)
        else:
            assert_equal(res['error_locations'], [])

    def test_validateaddress(self):
        # Invalid Bech32
        self.check_invalid(BECH32_INVALID_SIZE, "Invalid Bech32 address program size (41 bytes)")
        self.check_invalid(BECH32_INVALID_PREFIX, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(BECH32_INVALID_BECH32, 'Version 1+ witness address must use Bech32m checksum')
        self.check_invalid(BECH32_INVALID_BECH32M, 'Version 0 witness address must use Bech32 checksum')
        self.check_invalid(BECH32_INVALID_VERSION, 'Invalid Bech32 address witness version')
        self.check_invalid(BECH32_INVALID_V0_SIZE, "Invalid Bech32 v0 address program size (21 bytes), per BIP141")
        self.check_invalid(BECH32_TOO_LONG, 'Bech32 string too long', [90])
        self.check_invalid(BECH32_ONE_ERROR, 'Invalid Bech32 checksum', ONE_ERROR_AT)
        self.check_invalid(BECH32_TWO_ERRORS, 'Invalid Bech32 checksum', TWO_ERRORS_AT)
        self.check_invalid(BECH32_ONE_ERROR_CAPITALS, 'Invalid Bech32 checksum', ONE_ERROR_CAPITALS_AT)
        self.check_invalid(BECH32_NO_SEPARATOR, 'Missing separator')
        self.check_invalid(BECH32_INVALID_CHAR, 'Invalid Base 32 character', INVALID_CHAR_AT)
        self.check_invalid(BECH32_MULTISIG_TWO_ERRORS, 'Invalid Bech32 checksum', MULTISIG_TWO_ERRORS_AT)
        self.check_invalid(BECH32_WRONG_VERSION, 'Invalid Bech32 checksum', WRONG_VERSION_AT)

        # Valid Bech32
        self.check_valid(BECH32_VALID)
        self.check_valid(BECH32_VALID_UNKNOWN_WITNESS)
        self.check_valid(BECH32_VALID_CAPITALS)
        self.check_valid(BECH32_VALID_MULTISIG)

        # Invalid Base58
        self.check_invalid(BASE58_INVALID_PREFIX, 'Invalid or unsupported Base58-encoded address.')
        self.check_invalid(BASE58_INVALID_CHECKSUM, 'Invalid checksum or length of Base58 address (P2PKH or P2SH)')
        self.check_invalid(BASE58_INVALID_LENGTH, 'Invalid checksum or length of Base58 address (P2PKH or P2SH)')

        # Valid Base58
        self.check_valid(BASE58_VALID)

        # Invalid address format
        self.check_invalid(INVALID_ADDRESS, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')
        self.check_invalid(INVALID_ADDRESS_2, 'Invalid or unsupported Segwit (Bech32) or Base58 encoding.')

        node = self.nodes[0]


        if not self.options.usecli:
            # Missing arg returns the help text
            assert_raises_rpc_error(-1, "Return information about the given bitcoin address.", node.validateaddress)
            # Explicit None is not allowed for required parameters
            assert_raises_rpc_error(-3, "JSON value of type null is not of expected type string", node.validateaddress, None)

    def test_getaddressinfo(self):
        node = self.nodes[0]

        assert_raises_rpc_error(-5, "Invalid Bech32 address program size (41 bytes)", node.getaddressinfo, BECH32_INVALID_SIZE)
        assert_raises_rpc_error(-5, "Invalid or unsupported Segwit (Bech32) or Base58 encoding.", node.getaddressinfo, BECH32_INVALID_PREFIX)
        assert_raises_rpc_error(-5, "Invalid or unsupported Base58-encoded address.", node.getaddressinfo, BASE58_INVALID_PREFIX)
        assert_raises_rpc_error(-5, "Invalid or unsupported Segwit (Bech32) or Base58 encoding.", node.getaddressinfo, INVALID_ADDRESS)
        assert "isscript" not in node.getaddressinfo(BECH32_VALID_UNKNOWN_WITNESS)

    def run_test(self):
        self.test_validateaddress()

        if self.is_wallet_compiled():
            self.init_wallet(node=0)
            self.test_getaddressinfo()


if __name__ == '__main__':
    InvalidAddressErrorMessageTest(__file__).main()
