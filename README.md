# zSide

zSide is a sidechain of Chains for private payments, with the **Orchard** shielded pool of Zcash:
coins in it move without showing who pays whom, or how much; only the sender and the recipient know.

| | |
|---|---|
| Slot on the mainchain | 6 |
| Coin | CHN, deposited from [Chains](https://github.com/block-fabric/chains) and withdrawn back to it |
| Addresses | `zs1…` (`tzs1…` on the test networks); shielded `zo1…` (`tzo1…`) |
| P2P port | 9455 (testnet 19455, signet 39455, regtest 29455) |
| RPC port | 9454 (testnet 19454, signet 39454, regtest 29454) |

zSide is the [sidechain template](doc/sidechain.md) of Chains with the shielded pool added. A zSide node needs a
Chains node (`chainsd` or `chains-qt`) to follow, reached over its RPC interface.

## Installing

There is no packaged build yet: build it from source, below.

## Building from source

On Debian or Ubuntu (24.04):

```sh
sudo apt install build-essential cmake pkgconf python3 libevent-dev libboost-dev \
    libsqlite3-dev libzmq3-dev qt6-base-dev qt6-tools-dev qt6-l10n-tools libqrencode-dev
# The Orchard library is Rust: install a current stable toolchain.
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
git clone https://github.com/block-fabric/zside.git
cd zside
cmake -B build -DBUILD_GUI=ON
cmake --build build -j$(nproc)
```

CMake builds the Rust library (`src/rust`) with `cargo`, with the crate versions pinned in
`Cargo.lock`; the first build downloads them. It looks for `cargo` in the `PATH` and in
`~/.cargo/bin`. `-DZSIDE_CARGO_JOBS=<n>` sets how many jobs cargo uses (default 4).

Leave out `-DBUILD_GUI=ON` (and the Qt packages) for a node without a window. Other systems and
options: [doc/build-unix.md](doc/build-unix.md) and the other `doc/build-*.md`.

The programs, in `build/bin`:

| Program | What it is |
|---|---|
| `zside-qt` | the wallet, with its window |
| `zsided` | the node, without a window |
| `zside-cli` | commands for a running node or wallet |
| `zside-wallet`, `zside-tx`, `zside-util` | tools that work without a node |

Tests: `ctest --test-dir build` (unit tests) and `build/test/functional/test_runner.py`. The
sidechain tests need a built Chains tree next to this one (`../chains`), or `MAINCHAIN_BIN_DIR`
set to the folder of its programs.

## Running it

First a Chains node, with RPC on (`server=1` in `chains.conf`, or `-server`), on the same network.

The simplest is to let the Chains wallet run zSide: in its **Sidechain Nodes** window, select
zSide, press **Locate programs…** and choose the folder with `zsided` and `zside-qt`, then **Start node**
or **Open wallet**.

By hand, on the same computer as the Chains node:

```sh
zside-qt                 # the main network
zside-qt -testnet        # the test network
```

It finds the Chains node by itself: RPC on 127.0.0.1 at the port of the network, and the cookie
file in the Chains data folder (`~/.chains/.cookie`, `~/.chains/testnet/.cookie`). Options for
anything else:

| Option | What it is |
|---|---|
| `-mainchainrpcconnect=<ip>` | address of the Chains node (default 127.0.0.1) |
| `-mainchainrpcport=<port>` | its RPC port (default 9554, testnet 19554, signet 39554, regtest 29554) |
| `-mainchainrpccookiefile=<file>` | its cookie file |
| `-mainchaindatadir=<dir>` | its data folder, where the cookie file is looked for (default `~/.chains`) |
| `-mainchainrpcuser=<user>`, `-mainchainrpcpassword=<pw>` | credentials, instead of the cookie |
| `-mainchainrpcwallet=<name>` | the Chains wallet that pays for merged mining (default: its only loaded wallet) |

The slot is fixed on the main and test networks; `-sidechainslot=<n>` is for regtest only, where
it makes the chain a sidechain in slot `n` (see [doc/sidechain.md](doc/sidechain.md)).

Data and the configuration file `zside.conf` are in `~/.zside`; the test network has a subfolder
`testnet`. The networks have no DNS seeds: give the node a peer with `addnode`. A sample
`~/.zside/zside.conf`:

```ini
server=1
fallbackfee=0.0002

# For the test network instead, uncomment:
# testnet=1

[main]
addnode=<peer>:9455
mainchainrpcwallet=<wallet of the Chains node>

[test]
addnode=<peer>:19455
mainchainrpcwallet=<wallet of the Chains node>
```

On the test network, the `-mainchainrpcport`, `-mainchainrpccookiefile` and `-mainchainrpcwallet`
options are read only from the `[test]` section.

## Coins in and out

- **Deposit.** In zSide, get a deposit address (`getdepositaddress`, or the **Mainchain** page):
  `s6_<address>_<checksum>`. In Chains, send to it from the **Sidechains** page, or
  `chains-cli createsidechaindeposit 6 <deposit address> <amount>`. The coins appear with the
  next zSide block.
- **Withdraw.** `createwithdrawal <Chains address> <amount> ( <fee for Chains miners> )`, or the
  **Mainchain** page. Withdrawals are paid in bundles that Chains miners vote on (about three
  months on the main network, 600 blocks on the test network). A withdrawal not in the bundle
  being voted on can be taken back: `refundwithdrawal <txid> <vout>`.
- **Merged mining.** zSide blocks are mined by Chains miners, for a fee in CHN paid by the Chains
  wallet. `setbmm true <zSide address>` has the node ask for a block whenever the fees waiting pay
  for one; it offers the miners 99% and keeps 1% at the address. `requestbmmblock <address>
  <amount>` asks for one block now, for a fee of your choice (for a deposit on a quiet chain).
  `getbmminfo` shows how it goes.

How deposits, withdrawals, bundles and merged mining work: [doc/sidechain.md](doc/sidechain.md).

## Private payments

A zSide wallet has two balances:

- **Transparent**, as on any sidechain: addresses `zs1…` (`tzs1…`). Deposits arrive here, and
  withdrawals leave from here.
- **Shielded**, in the Orchard pool: addresses `zo1…` (`tzo1…`). A shielded payment shows nothing of
  its sender, recipient or amount; the recipient alone reads its **memo**.

**Shielding** (`shieldcoins`) moves transparent coins in; **paying privately** (`sendshielded`) spends
shielded notes, to a shielded address or, unshielding, to a transparent one. Every proof (Halo 2, no
trusted setup) and signature is checked by every node, and the pool can never pay out more than went
in. The node keeps the note commitment tree (`-orchardindex`, on by default), which wallets need to
spend. The shielded key comes from the wallet's own keys: nothing more to back up.

### Melt & cast

What goes into the pool and what comes out are public, and can be linked: 13.47 shielded, 13.47
unshielded an hour later. **Melt & cast** (Paul Sztorc's design) leaves nothing to link:

- **Melt** (`meltcoins`) shields each transparent coin alone and whole, at random blocks over the
  next hours, so no two coins are ever seen together.
- **Cast** (`castcoins`) takes coins out only as standard bills (0.01 CHN times a power of two), each
  to a new address of the wallet, at the blocks every wallet uses for that bill.

The wallet keeps the plan and carries it out as blocks come: keep it running and unlocked. It is a
wallet feature, not a rule of the chain.

How it works: [doc/zside.md](doc/zside.md).

## Commands

Shielded, on the node: `getshieldedinfo`.

Shielded, in the wallet: `getshieldedaddress`, `getshieldedbalance`, `listshieldednotes`,
`shieldcoins`, `sendshielded`, `listshieldedtransactions`; melt & cast: `meltcoins`, `castcoins`,
`listmeltcast`, `cancelmeltcast`.

Sidechain, on the node: `getmainchaininfo`, `syncmainchain`, `setbmm`, `getbmminfo`, `requestbmmblock`, `createbmmblock`, `listwithdrawals`, `getwithdrawalbundle`.

Sidechain, in the wallet: `getdepositaddress`, `createwithdrawal`, `refundwithdrawal`.

## Wallet

The pages: Overview, Send, Receive, Transactions, **Mainchain** (deposits, withdrawals, merged
mining), and **Private** (shielded balance, address with QR code, paying privately with a memo, shielding, melt & cast, history).

## Credits

zSide is Paul Sztorc's idea of a Zcash sidechain, made with the `orchard` crate of the Zcash
developers (MIT/Apache-2.0), the library zcashd and Zebra use. It is built on the sidechain template of Chains, which is based on
[Bitcoin Core](https://github.com/bitcoin/bitcoin) v32; its README is in
[doc/README-bitcoin-core.md](doc/README-bitcoin-core.md).

## License

MIT: see [COPYING](COPYING). The Rust crates it links keep their own licenses (see `src/rust/Cargo.lock`).
