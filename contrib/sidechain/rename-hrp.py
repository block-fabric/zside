#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Give the addresses in the tests another human-readable part.

A sidechain has its own address prefix (bech32_hrp in src/kernel/chainparams.cpp).
The tests are full of addresses with the prefix they were written for. This
re-encodes every valid one, in place, and leaves alone what is not a valid
address (the tests of invalid addresses need a look by hand).

    contrib/sidechain/rename-hrp.py <old> <new>      for example: sc thunder

Run it from the top of the source tree, once per network whose prefix changed.
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "test", "functional"))
from test_framework.descriptors import descsum_create  # noqa: E402
from test_framework.segwit_addr import bech32_decode, bech32_encode  # noqa: E402


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    old, new = sys.argv[1].lower(), sys.argv[2].lower()
    token = re.compile(r"\b(?:%s1[0-9a-z]+|%s1[0-9A-Z]+)\b" % (re.escape(old), re.escape(old.upper())))
    files = subprocess.run(
        ["git", "grep", "-lIi", old + "1", "--", "src/test", "src/wallet/test", "src/qt/test", "src/bench", "test/functional", "src/qt/guiutil.cpp", "src/wallet/test/util.h"],
        capture_output=True, text=True, check=False).stdout.split()
    changed, skipped = 0, []

    for path in files:
        with open(path, encoding="utf8") as f:
            text = f.read()

        def convert(match):
            nonlocal changed
            address = match.group(0)
            encoding, hrp, data = bech32_decode(address)
            if encoding is None or hrp != old:
                skipped.append((path, address))
                return address
            changed += 1
            converted = bech32_encode(encoding, new, data)
            return converted.upper() if address.isupper() else converted

        converted = token.sub(convert, text)
        # The checksum of a descriptor covers the address in it.
        converted = re.sub(r"(addr\(%s1[0-9a-z]+\))#[0-9a-z]{8}" % re.escape(new), lambda m: descsum_create(m.group(1)), converted)
        if converted != text:
            with open(path, "w", encoding="utf8") as f:
                f.write(converted)

    # The test framework builds addresses too.
    constants = os.path.join("test", "functional", "test_framework", "address.py")
    with open(constants, encoding="utf8") as f:
        text = f.read()
    line = re.search(r"^HRP_BY_CHAIN = .*$", text, re.M)
    if line:
        text = text.replace(line.group(0), line.group(0).replace(f'"{old}"', f'"{new}"'))
        with open(constants, "w", encoding="utf8") as f:
            f.write(text)

    # The expected output of the tool that prints the parameters of a chain.
    util_data = os.path.join("test", "functional", "data", "util")
    for name in os.listdir(util_data):
        if not name.startswith("getchainparams-"):
            continue
        with open(os.path.join(util_data, name), encoding="utf8") as f:
            text = f.read()
        with open(os.path.join(util_data, name), "w", encoding="utf8") as f:
            f.write(text.replace(f'"bech32_hrp": "{old}"', f'"bech32_hrp": "{new}"'))

    # A test of the framework itself names the prefix of regtest.
    own_test = os.path.join("test", "functional", "test_framework", "segwit_addr.py")
    with open(own_test, encoding="utf8") as f:
        text = f.read()
    with open(own_test, "w", encoding="utf8") as f:
        f.write(text.replace(f'self.assertEqual(hrp, "{old}")', f'self.assertEqual(hrp, "{new}")'))

    print(f"re-encoded {changed} addresses in {len(files)} files")
    for path, address in skipped:
        print(f"left alone, not a valid address: {path}: {address}")


if __name__ == "__main__":
    main()
