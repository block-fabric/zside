# Sidechain template

This tree is a sidechain of the [Chains](../../chains) mainchain, with nothing
in it but what every sidechain needs. It is meant to be copied: Thunder and
Hivemind are this tree with their own parameters and features on top.

It is the Chains mainchain tree (Bitcoin Core 32 with drivechains) with the
sidechain rules added, so everything a mainchain node has is here too: the
wallet, the GUI with its tools and themes, the RPC interface, the tests.

## What makes the chain a sidechain

A sidechain has no miners and no coins of its own.

- **Blind merged mining.** A block is valid only if a block of the mainchain
  committed to its hash. Whoever wants a block mined builds it, and pays
  mainchain miners a fee, on the mainchain, to commit to it. The miners need
  not run the sidechain or know what the block contains. There is at most one
  sidechain block per mainchain block. The proof of work field of the header
  is kept as a formality that takes a couple of hashes.
- **Deposits.** Coins sent to the escrow of the sidechain on the mainchain,
  with a deposit address of the sidechain as the destination, are created on
  the sidechain by the coinbase of its next block. There is no block subsidy:
  the miner of a block earns the transaction fees.
- **Mining pays for itself.** A node that mines automatically (`setbmm true
  <address>`) asks for a block when there are transactions whose fees pay for
  one, and offers the miners of the mainchain 99% of those fees. The fees go
  to the address, so a hundredth is what the operator of the node earns. With
  no fees it asks for nothing and pays nothing. A block can be mined by hand
  for a fee of one's choosing (`requestbmmblock <address> <amount>`), empty
  or not: that is how a deposit gets paid, or a withdrawal bundle started,
  on a chain without transactions.
- **No waiting period.** What a block pays, deposits and fees alike, can be
  spent in the next block.
- **Deposit addresses.** A deposit address is `s<slot>_<address>_<checksum>`:
  the slot of the sidechain, an address of it, and the first six hexadecimal
  digits of the SHA-256 hash of what comes before. The wallet gives one with
  `getdepositaddress`. The mainchain wallet refuses one that is mistyped or
  for another slot than the deposit; the sidechain burns a deposit that names
  one anyway. A bare address of the sidechain is accepted too, unchecked.
  Every sidechain has address prefixes of its own on every network, regtest
  included, so that an address of one chain is not an address of another.
- **Withdrawals.** A transaction burns coins in a withdrawal output that names
  a mainchain address and a fee for mainchain miners. Blocks commit to
  withdrawal *bundles*: the mainchain transaction that pays the waiting
  withdrawals. Which withdrawals a bundle pays is fixed by the rules, so every
  node builds the same bundle from the same chain. The mainchain miners vote
  on the bundle for a long time (three months on the Chains main network),
  then the mainchain pays it out of the escrow, or lets it fail.
- **Refunds.** A withdrawal that is not in the bundle being voted on can be
  taken back by whoever made it. The withdrawals of a failed bundle go in a
  later one, or can be taken back.
- **Payouts per block.** A coinbase pays at most 1000 outputs for deposits,
  refunds and the like; what is owed beyond waits, in order, for the next
  blocks. Without a limit, enough deposits at once would make every block too
  large to be valid.
- **Bundles carry the hash of the block before the one that commits to them**,
  in an OP_RETURN output, so that nobody can know a bundle's hash, and get the
  mainchain to close it, before the sidechain commits to it. A withdrawal has
  to pay a standard output (P2PKH, P2SH, P2WPKH, P2WSH, P2TR): the mainchain
  relays nothing else.
- **Bundles paid on another branch.** The mainchain says what a withdrawal
  bundle it paid out paid (`getsidechainevents`). A branch of the sidechain
  that never had that bundle takes the withdrawals it paid as paid: for every
  payout, the oldest withdrawal paying the same output with the same amount.
  Without this, the branch would pay them again. The mainchain pays one bundle
  per slot and fails the others pending, so a bundle of another branch pending
  on the mainchain does not hold back a new bundle of this one: only one of
  them is paid. A node whose record shows a mainchain block that paid a bundle
  and left another pending logs a warning: that mainchain does not pay one
  bundle per slot.
- **Refunds and pending bundles.** A bundle of another branch pending on the
  mainchain may hold a withdrawal that this branch would refund. Refunds wait
  while such a bundle has a work score of
  `pending_min_score` or more (`getsidechainevents` reports the bundles pending
  after each block with their scores); below it, anyone could freeze refunds by
  proposing a bundle. Mainchain miners in follow mode (`LEADING_BY_50`)
  upvote whichever bundle leads its slot, so refunds also wait while the
  bundle with the highest score (strictly, and not this chain's own) rose by
  3 or more over the last 12 mainchain blocks (from 1, what a proposal starts
  with, if proposed since): net upvotes in a quarter of the blocks, half the
  pace a payout takes, so on its way to being paid rather than to failing. A
  bare proposal nobody upvotes never rises, and one that the miners who vouch
  for this chain downvote goes down: neither holds refunds back, and keeping
  one rising takes a good share of the hashrate all along. A withdrawal
  refunded while a bundle that holds it has neither can still be paid twice if a majority of mainchain miners votes
  that bundle through over the downvotes of those who vouch for this chain's
  own: that is the drivechain security model.
- **Bundles nobody proposes.** A bundle the mainchain has not proposed `unproposed_expiry_blocks` mainchain blocks after the block that
  committed to it fails, and its withdrawals go in a later one or can be taken
  back. A proposal made before that block, by someone who worked the hash
  out ahead, counts if the bundle is still pending then.
- **Committed twice.** A mainchain miner can commit to a sidechain block again
  later. The block keeps its first commitment; losing the second, in a reorg
  of the mainchain, changes nothing for it.

A node validates all of this itself. It does so against a record of the
mainchain that it keeps by following a mainchain node, so **a sidechain node
needs a mainchain node** (`chainsd`), reached over its RPC interface.

## Where the code is

| What | Where |
|---|---|
| Parameters of the rules | `Consensus::SidechainParams` in `src/consensus/params.h` |
| Identity of the chain on each network | `MakeSidechain(...)` calls in `src/kernel/chainparams.cpp` |
| The rules: state, deposits, withdrawals, bundles, refunds | `src/sidechain/state.{h,cpp}` |
| Script formats of withdrawals, refunds, bundle commitments | `src/sidechain/script.cpp` |
| Record of the mainchain | `src/sidechain/mainchain.{h,cpp}` |
| Keeping the record current, mainchain reorganisations, merged mining | `src/sidechain/follower.{h,cpp}` |
| RPC client for the mainchain node | `src/sidechain/mainclient.{h,cpp}` |
| Hooks in validation | `ContextualCheckBlockHeader`, `ConnectBlock`, `GetBlockSubsidy`, mempool checks in `src/validation.cpp` |
| Block assembly | `src/node/miner.cpp` |
| RPC commands | `src/rpc/sidechain.cpp`, `src/wallet/rpc/sidechain.cpp` |
| GUI page | `src/qt/sidechainpage.{h,cpp}` |
| Tests | `test/functional/feature_sidechain.py` |

The state of the sidechain rules (`sidechain::State`) is stored, copied and
reverted together with the drivechain database of the mainchain code
(`drivechain::SidechainDB`), which is otherwise unused here: a sidechain has
no sidechains of its own.

The validity of a block depends on the block, the blocks before it, and the
record of the mainchain; never on asking the mainchain node during validation.
A block committed to by the mainchain block at height H acts on what the
mainchain did up to height H - 1: it pays the deposits of those blocks and
takes note of the outcome of the bundle.

## Commands

Node: `getmainchaininfo`, `syncmainchain`, `createbmmblock`, `setbmm`,
`getbmminfo`, `listwithdrawals`, `getwithdrawalbundle`.

Wallet: `getdepositaddress`, `createwithdrawal`, `refundwithdrawal`.

On the mainchain node, for the sidechain: `createsidechaindeposit`,
`createbmmrequest`, `listwithdrawalbundles`, `setwithdrawalvote`,
`getsidechainevents`.

Options: `-mainchainrpcconnect`, `-mainchainrpcport`, `-mainchainrpcuser`,
`-mainchainrpcpassword`, `-mainchainrpccookiefile`, `-mainchaindatadir`,
`-mainchainrpcwallet`. Without user and password, the cookie file of the
mainchain node is used, looked for in `~/.chains`.

## Trying it on regtest

On regtest the chain is an ordinary chain unless `-sidechainslot=<n>` is
given, so that the tests inherited from the mainchain keep working.

```sh
# The mainchain: a wallet with coins, and the sidechain activated in slot 3.
chainsd -regtest -daemon -fallbackfee=0.0002
chains-cli -regtest createwallet miner
MAIN=$(chains-cli -regtest getnewaddress)
chains-cli -regtest generatetoaddress 110 $MAIN
chains-cli -regtest createsidechainproposal 3 "My sidechain"
chains-cli -regtest generatetoaddress 21 $MAIN

# The sidechain node follows it.
sidechaind -regtest -daemon -sidechainslot=3 -fallbackfee=0.0002
sidechain-cli -regtest getmainchaininfo
sidechain-cli -regtest createwallet side
SIDE=$(sidechain-cli -regtest getnewaddress)
# A deposit address for it: s3_<address>_<checksum>
DEPOSIT=$(sidechain-cli -regtest getdepositaddress $SIDE | jq -r .depositaddress)

# Deposit, and mine: one sidechain block per mainchain block.
chains-cli -regtest createsidechaindeposit 3 $DEPOSIT 10
sidechain-cli -regtest setbmm true $SIDE true 0.001
chains-cli -regtest generatetoaddress 5 $MAIN
sidechain-cli -regtest getbalance

# Withdraw.
sidechain-cli -regtest createwithdrawal $MAIN 2
chains-cli -regtest generatetoaddress 3 $MAIN
sidechain-cli -regtest getwithdrawalbundle
chains-cli -regtest listwithdrawalbundles 3
```

`sidechain-qt` does the same from its Mainchain tab.

## Making your own sidechain

1. Copy this tree.
2. Name it: `contrib/sidechain/rename.py Thunder thunder` sets `CLIENT_NAME`
   and `CLIENT_BIN_NAME` in `CMakeLists.txt` and brings the tests along. The
   programs, the data directory and the configuration file are named after
   `CLIENT_BIN_NAME`.
3. Give it an identity on each network: the `MakeSidechain(...)` calls in
   `src/kernel/chainparams.cpp` (message of the first block, magic bytes,
   port, address prefix), and the RPC ports in `src/chainparamsbase.cpp`. The
   first block is derived from the message, so a new message is a new chain.
   The slot is the one the mainchain activates your sidechain in. Once it is
   activated, set `main_activation_height` to the height of the mainchain
   block that activated it (`activationheight` in the mainchain's
   `getsidechain`): a main network release without it refuses to start,
   whatever the slot (slot 0 too), a testnet or signet one warns; regtest
   does not ask for it, nor do the template's own networks. Set
   `pending_min_score` (a tenth of the
   mainchain's `withdrawal_min_score`: 6480 on main and signet, 30 on testnet)
   and `unproposed_expiry_blocks` (1440 on main and signet, 60 on testnet).
4. Bring the tests along:
   - `contrib/sidechain/rename-hrp.py <old> <new>` re-encodes the addresses in
     the tests for each address prefix you changed;
   - the magic bytes of the networks are in
     `test/functional/test_framework/messages.py`, the port of regtest in
     `test/functional/p2p_addrfetch.py`, and the hash of the UTXO snapshot
     in `test/functional/rpc_dumptxoutset.py` follows the magic of regtest;
   - regenerate `test/functional/data/util/getchainparams-*.json` with
     `<name>-util [-testnet|-signet|-regtest] getchainparams`.
5. Add what makes your sidechain worth having. Consensus rules of your own
   go next to the sidechain rules in `sidechain::State::ConnectBlock`, with
   their state in `sidechain::State` if they have any; it is stored and
   reverted with the rest.
6. Have the sidechain proposed and activated on the mainchain
   (`createsidechainproposal` there), with the hash of your release as its
   identifier.

Run `ctest --test-dir build` and `build/test/functional/test_runner.py`; the
sidechain test needs a built mainchain tree next to this one (`../chains`),
or `MAINCHAIN_BIN_DIR` set to the directory of its programs.

## Limits to know

- A sidechain trusts the miners of the mainchain with its coins: a majority of
  them can vote through a bundle that the sidechain did not make. That is the
  drivechain security model; the long voting period is what gives users and
  honest miners time to object.
- Deposits to something that is not an address of the sidechain are burned,
  and so are deposits to a deposit address with a wrong checksum or the slot
  of another sidechain.
- A withdrawal refund pays a P2WPKH address of the wallet that made the
  withdrawal, and needs that wallet to sign the request.
