#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Name the sidechain.

    contrib/sidechain/rename.py <Name> <stem>        for example: Thunder thunder

<Name> is what users see. <stem> names the programs (<stem>d, <stem>-cli,
<stem>-app, ...), the data directory (~/.<stem>) and the configuration file
(<stem>.conf). The name is set in CMakeLists.txt and in the chain of the app
(src/webui/app/src/chain/chain.json, with the scheme of its payment URIs, <stem>:);
this also brings the tests along, which know the programs and files by name.
Then run `npm run i18n` in src/webui/app: the catalogs have the name in them.

Run it from the top of the source tree. The identity of the chain on each
network (first block, magic bytes, ports, address prefix) is not touched: see
doc/sidechain.md.
"""

import re
import subprocess
import sys


def main():
    if len(sys.argv) != 3 or not re.fullmatch(r"[a-z][a-z0-9]*", sys.argv[2]):
        sys.exit(__doc__)
    name, stem = sys.argv[1], sys.argv[2]

    with open("CMakeLists.txt", encoding="utf8") as f:
        cmake = f.read()
    old_name = re.search(r'^set\(CLIENT_NAME "([^"]*)"\)', cmake, re.M).group(1)
    old_stem = re.search(r'^set\(CLIENT_BIN_NAME "([^"]*)"\)', cmake, re.M).group(1)
    cmake = cmake.replace(f'set(CLIENT_NAME "{old_name}")', f'set(CLIENT_NAME "{name}")')
    cmake = cmake.replace(f'set(CLIENT_BIN_NAME "{old_stem}")', f'set(CLIENT_BIN_NAME "{stem}")')
    with open("CMakeLists.txt", "w", encoding="utf8") as f:
        f.write(cmake)

    # The chain of the app (src/webui/app/src/chain/chain.json): its name, its programs, the scheme of its URIs.
    chain_json = "src/webui/app/src/chain/chain.json"
    with open(chain_json, encoding="utf8") as f:
        chain = f.read()
    chain = re.sub(r'^(  "name": )"[^"]*"', rf'\1"{name}"', chain, count=1, flags=re.M)
    chain = re.sub(r'^(  "bin": )"[^"]*"', rf'\1"{stem}"', chain, count=1, flags=re.M)
    chain = re.sub(r'^(  "uriScheme": )"[^"]*"', rf'\1"{stem}"', chain, count=1, flags=re.M)
    with open(chain_json, "w", encoding="utf8") as f:
        f.write(chain)

    o, n = re.escape(old_stem), stem
    patterns = [
        (rf"\b{o}d\b", f"{n}d"),
        (rf"\b{o}-(cli|app|util|wallet|tx|chainstate|node)\b", rf"{n}-\1"),
        (rf"\b{o}\.conf\b", f"{n}.conf"),
        (rf'"\.{o}"', f'".{n}"'),
        (rf'"{o}": "BITCOIN_BIN"', f'"{n}": "BITCOIN_BIN"'),
        (rf'/ "{re.escape(old_name)}"', f'/ "{name}"'),
        (rf'Application Support/{re.escape(old_name)}"', f'Application Support/{name}"'),
    ]
    files = subprocess.run(["git", "grep", "-lI", "-e", old_stem, "-e", old_name, "--", "test", "src/test", "src/wallet/test", "src/webui/test"],
                           capture_output=True, text=True, check=False).stdout.split()
    changed = 0
    for path in files:
        with open(path, encoding="utf8") as f:
            text = f.read()
        renamed = text
        for pattern, replacement in patterns:
            renamed = re.sub(pattern, replacement, renamed)
        if renamed != text:
            changed += 1
            with open(path, "w", encoding="utf8") as f:
                f.write(renamed)
    print(f"{old_name} ({old_stem}) is now {name} ({stem}); {changed} test files follow; now `npm run i18n` in src/webui/app")


if __name__ == "__main__":
    main()
