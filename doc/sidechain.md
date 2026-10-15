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
6. For blocks that are not all merged mined (a block producer of your own, a
   chain order of your own, blocks that take the peg actions only now and
   then), derive from `sidechain::ChainHooks` and install it with
   `sidechain::SetHooks` at startup; see "Hooks" below.
7. Have the sidechain proposed and activated on the mainchain
   (`createsidechainproposal` there), with the hash of your release as its
   identifier.

Run `ctest --test-dir build` and `build/test/functional/test_runner.py`; the
sidechain test needs a built mainchain tree next to this one (`../chains`),
or `MAINCHAIN_BIN_DIR` set to the directory of its programs.

## Hooks

`src/sidechain/hooks.h` has the extension points for sidechains whose blocks
are not all merged mined. Nothing changes unless a sidechain installs hooks of
its own (`sidechain::SetHooks`, at startup, before any block is read): the
defaults are the template's behaviour, which `src/test/sidechain_hooks_tests.cpp`
checks.

| Hook | Where it is called | Default |
|---|---|---|
| `BlockExtensions` | once, at `SetHooks`: switches extended blocks on (`BLOCK_EXTENSION_BIT` in `nVersion`: a 32-byte `hashSections` and a 64-byte signature after the 80-byte header, and `vSections` after the transactions, in blocks, the block index, `headers` and compact blocks). The block hash does not cover the signature: a block for a header the node has must carry that header's extension, or it is a mutated block (`bad-blk-extension`; the header stays valid), and a compact block with another one is not reconstructed | off: the bit means nothing |
| `Kind`, `CheckHeader` | `ContextualCheckBlockHeader`: a `PLUGIN` block has no proof of work and no BMM rule; the sidechain checks its header | every block is `BMM` |
| `CheckBlock`, `SectionsHash` | `CheckBlock`; sections other than those the header commits to make the block mutated (`bad-sections-hash`) | no rules; tagged hash `sidechain/sections` |
| `HeaderCheckable`, `UncheckedLimits` | `AcceptBlockHeader`: a header the node cannot check all of yet (a signature whose signer it knows only once it connected the chain below), and every header on top of one, is taken *unchecked*: held by the peers that send it, within each one's own budget (`unchecked-budget`, not punished), never written to the block index database, its block stored apart from the block files until connected, and both gone when no peer holds it; checked when its block is connected. See "Headers the node cannot check yet" | every header can be checked; budget 2000 headers and 32 MiB of blocks an outbound peer, 500 and 8 MiB an inbound one, 8 inbound peers holding any at once |
| `HeaderSpare`, `HeaderKept` | `AcceptBlockHeader`: a header of a kind the block index has as many of as the chain can need is taken from a peer only with a header on top of it in the same message, or alone if it would be the best header or is the better tip an outbound peer announces, and then unchecked; otherwise alone, refused (`header-spare`, not punished). See "Spare headers" | none |
| `CheckOtherExtension`, `ExtensionFailure` | `AcceptBlockHeader`, `AcceptBlock`, `ProcessNewBlock` (not compact blocks: one with another extension is not reconstructed, nor judged -- a signature check for 200 bytes anyone can send for ever; the block comes whole): a header or block for an extended header the node has, with another extension. `EQUIVALENT` (another valid signature of the same block): taken with the extension the node has, which it keeps, and the peer is not punished; `REPLACE` (valid, and the one the node has is not: taken unchecked): replaces it (`Chainstate::ReplaceBlockExtension`; the stored block is written again with it, and a failure of the block with the old one taken back); `UNKNOWN`: the block is the same block, taken with its header's extension. A block whose extension fails when it is connected (`ExtensionFailure`), its header unchecked: the copy's fault (see below) | `INVALID`: a mutated block (`bad-blk-extension`); no extension failures |
| `MaxFutureBlockTime` | `ContextualCheckBlockHeader`: how far ahead of the node's clock a block's time may be (`time-too-new`, a header that may come later) | two hours, as Bitcoin |
| `CustomChainOrder`, `ChainWork` | `nChainWork` when a header is added or the index loaded, and for a block and all above it when the record takes back its failure (`ReconsiderRecordFailure`: a key that reads the record may have changed); with a chain order of its own, headers presync (a minimum of work) is skipped, and so is the anti-DoS work threshold near the tip (a compact block whose parent has a lower key is not low-work) | most work |
| `PardonRecordFailures` | `AcceptBlockHeader`: a header of a block that failed against the record of the mainchain (`Mainchain::Failure::RECORD`, the block or the failed block it descends from), or on one, is `BLOCK_TIME_FUTURE` (`record-failed`) rather than `BLOCK_CACHED_INVALID`/`BLOCK_INVALID_PREV`: the peer is not punished, its record may be ahead; and a block that fails against the record when connected is `BLOCK_TIME_FUTURE` rather than `BLOCK_CONSENSUS`. `RecordIndependent` names the reasons of the sidechain's own rules that fail a block whatever the record says (not failures against the record) | on (off: punished as in Bitcoin); no reasons of its own |
| `IsWaitReason`, `WaitingReady` | headers that may become valid later wait in the bounded set of `ChainstateManager::AddBmmWaiting`, and are tried again in this order | `bmm-unknown`, by commitment height |
| `CommitmentOf`, `BlocksCommittedBy` | the BMM order rule, the Follower (blocks that lost their commitment, catching up) | a block needs a commitment to its own hash |
| `GetPegStep`, `BeforePegStep`, `AfterBlock`, `RefundsOnlyInPegSteps` | `State::ConnectBlock`, split into `ApplyPegStep` (mainchain events, bundle), `ApplyTxs` and `TakePayouts`. A block without a peg step pays nothing (what it owes is queued), cannot start a bundle (`bad-sc-bundle-not-peg`) and, with the switch, takes no refund request (`bad-sc-refund-not-peg`). `PegStep::max_withdrawal_height` limits the withdrawals a bundle takes; `PegStep::peg_height`, if set, is the height the peg actions count as at in place of the block's (the bundle's lock time, the height of a failure the retry delay counts from), so that blocks that take them for the same block build the same bundle. `AfterBlock` is told what the coinbase paid out (`minted`), and writes the sidechain's own tables through `State::Writable` | every block takes the peg actions as committed, with its parent as the bundle nonce |
| `MergeMinedOutputs` | block assembly of a merged mined block (`SidechainAssembly::Peg::BMM`): outputs of the sidechain's own in the coinbase, after the payouts and the bundle commitment (proofs its rules take, say), given the state with the block so far | none |
| `ApplyTx` | `State::ApplyTx`, after the template's rules for a transaction, on its own overlay (all or nothing; what it owes is paid as refunds are), so in blocks and block assembly; and the mempool (`DrivechainChecks`), on an overlay that is dropped, with the next block's height | no rules |
| `HasTxRules`, `TxRuleKeys` | `Chainstate::RemoveStaleDrivechainTxs`: after a change of the tip, the mempool's transactions `HasTxRules` names (those `ApplyTx` may refuse) are checked again on the new tip, and dropped, with what spends them, if the next block could no longer take them. Only those whose keys of the state (`TxRuleKeys`: those whose values alone decide) the blocks connected or disconnected changed (`Chainstate::NoteSideChanges`; all of them when the hooks cannot tell, or the state was loaded), and `MAX_RULE_CHECKS_PER_TIP` (500) at most per change of the tip, the rest queued for the next ones (`CTxMemPool::QueueRuleCheck`): the work is that of what blocks change, not that of the mempool, which anyone can fill with such transactions | none; keys: none (all, after every change of the tip) |
| `node::SidechainAssembly` | `BlockAssembler`: a block with the template's peg actions, none, or those of a given `PegStep`, its sections, and a transaction filter. Assembly calls `BeforePegStep` as `ConnectBlock` does, with the block as far as it is known (parent, version, time, sections, and a coinbase paying the coinbase script), so that the coinbase pays what it owes; `AfterBlock` is for validation only | the template's block |
| `FollowerRound`, `StartServices`, `StopServices` | the Follower's loop (in place of its merged mining), and node start and shutdown | the Follower mines; no services |
| `ProcessMessage`; `PeerManager::SendSidechainMessage`, `BroadcastSidechainMessage` | message types Bitcoin does not know, after the handshake | ignored |

`Follower::RequestCommitment` also takes any hash to commit to, and
`Mainchain` has `HashAt(height)` and `TimeHeight(time)` (the highest block
with a median time at or below `time`).

## Headers the node cannot check yet

A sidechain whose headers carry a signature by a signer the node knows only from
the chain below (a validator set made by an anchor, say) cannot check a header
before it has connected that chain: a node that catches up, or that sees a
branch it never connected. It cannot refuse such a header either (it would never
catch up, nor leave a branch for one with more work). The hooks say which headers
these are (`HeaderCheckable`); the template does the rest, the same for every
chain that has them, and nothing for one that does not:

- **They belong to the peers that send them** (`ChainstateManager::m_unchecked`).
  A header taken unchecked, and every header on top of one, is *unchecked* until
  its block is connected. Each has *holders*: the peers that sent it, in a headers
  message, a compact block or a block, or sent a header on top of it. The node
  (its own blocks, its disk, its operator) is a holder that never goes.
- **Bounded, by peer.** An outbound or manual peer holds at most
  `UncheckedLimits().per_peer` headers (2000 by default), and the blocks the node
  stored of the headers it holds weigh at most
  `UncheckedLimits().block_bytes_per_peer` (32 MiB). An inbound peer, which
  anyone can open, has a much smaller budget, `per_inbound_peer` (500) and
  `block_bytes_per_inbound_peer` (8 MiB, a block of the largest size), and only
  `inbound_holders` (8) inbound peers may hold unchecked headers at once: a
  further one is refused until one of them holds none. Beyond any of these, a
  header is refused (`unchecked-budget`, `BLOCK_HEADER_LOW_WORK`: not punished,
  and the peer is asked for its headers again once the tip moved). Every
  connected peer has its whole budget whatever the others hold or share: no
  peer's headers are dropped to make room for another's, and no peer is
  disconnected for it. (A node-wide total cannot give that: attackers holding
  the same headers in pairs, or many new connections, would take it from an
  honest peer, as the adversarial review of the earlier design showed.) The
  honest peer a node syncs from is usually one it chose (outbound): inbound
  attackers, however many, take at most the inbound holders' budgets, and never
  an outbound peer's. Inbound peers beyond the cap get no unchecked headers
  until a slot frees, which costs them only the headers the node cannot check
  yet (their checked headers and blocks are taken as from any peer).
- **Worst case, node-wide** (review of audit 6, item 10). The peers that can
  hold the outbound budget are bounded by Bitcoin's connection limits, not by
  `-maxconnections`: 11 automatic outbound connections (8 full relay, 2 block
  relay only and a feeler; the extra peers of a stale tip or of block-relay
  rotation, and address fetches, share the same 11 grants, `semOutbound`) and 8
  manual ones (`-addnode`, `semAddnode`): 19. (Private broadcast connections
  ignore headers. Manual connections beyond those, `addnode onetry` or
  `-connect`, are the operator's.) So at most 19 x `per_peer` +
  `inbound_holders` x `per_inbound_peer` unchecked headers, and
  19 x `block_bytes_per_peer` + `inbound_holders` x
  `block_bytes_per_inbound_peer` of their blocks on disk. An unchecked header
  costs about 600 bytes of memory: its block index entry (`CBlockIndex`, 176
  bytes, in a node of the block map, about 240), its extension (about 128), its
  `m_unchecked` entry (about 80, and its list of holders) and an entry in
  `m_unchecked_held` for each holder (about 64); count 1 KiB for allocator
  overhead and hash buckets. With the template's defaults: 42,000 headers
  (about 25 MB, 41 MiB at 1 KiB each) and 672 MiB of blocks at the very worst; a chain sets its own
  (FastBlocks: see doc/fastblocks.md). Entries taken out of the block index are
  freed a batch later (the last batch whole, up to `per_peer`, and 4096
  before it): defence in depth only (below).
- **Volatile.** Unchecked headers are not written to the block index database,
  and leave the block index when no peer holds them (the last holder
  disconnected): a reconnection or a new address never leaves headers behind.
  This is new to Bitcoin's code, where a block index entry, once made, lives as
  long as the node: see the next point.
- **Pointers into the block index.** Only unchecked headers ever leave the block
  index, any time `cs_main` is free (a peer disconnects), and a header once
  checked stays so. So no code may use an unchecked header's entry once it
  released `cs_main` since it found it. Every path that can reach one does one of
  three things:
  - *holds `cs_main` while it uses it*: `getblockheader`, `getchaintips`,
    `getblockfrompeer`, `getblock` of an unchecked header's block (the block of a
    checked one is read without the lock, as ever), the wallet's `findBlock` and
    the other `interfaces::Chain` lookups (the block of an unchecked header, or
    read while the caller still uses one, read with the lock held), the locator
    of the best header and the header tip notified, what a `block` message reads
    of its parent, `RegenerateCommitments`, the log of `ProcessNewBlockHeaders`;
  - *looks it up again by hash* once it has the lock again, if any header left
    meanwhile (each removal counts, `m_unchecked_erasures`): `ActivateBestChain`
    (its most-work chain), `InvalidateBlock` (the block, and its map of blocks
    that may become the best header or a candidate, built again), the headers
    and compact block handlers (the last header; the lock kept while pointers
    to it are stored), `RetryBmmWaiting` (returns hashes). `PreciousBlock`,
    `InvalidateBlock` and the follower's invalidation of a block that lost its
    commitment take a hash, looked up under their own lock;
    `ProcessNewBlockHeaders` hands back its last header with that contract
    (`ppindex`);
  - *never sees unchecked headers*: `BlockManager::LookupCheckedBlockIndex`, for
    the code that uses what it finds without the lock: `getblockstats`,
    `gettxoutsetinfo` and `dumptxoutset` by hash, `getblockfilter`,
    `gettxoutproof`, `getrawtransaction` with a block hash, REST's `block`,
    `spenttxouts` and `blockfilter`, the wallet interface's block filter match,
    `getblocktxn` (the node announces only blocks it connected), and the kernel
    library's lookups (its best entry is the best header's highest checked
    ancestor). A low-work headers sync (`HeadersSyncState`, which keeps its start
    without the lock) never starts on an unchecked header: headers on one are
    taken if they have the work, and ignored otherwise.

  The rest sees only checked entries: the active chain, the validation interface
  (connected blocks; `NewPoWValidBlock` is called with `cs_main` held), the
  indexes, ZMQ, the GUI (through RPCs and `interfaces::Node`, which reads the
  header tip with the lock held), and every lookup used under the lock. The
  peers' pointers (best known block, last common block, best header sent, the
  header of the chain sync timeout, blocks in flight) move to the nearest block
  that stays, under the same lock (`PeerManagerImpl::UncheckedErased`). The
  entries taken out are freed a batch later (`m_erased_headers`: the last batch
  whole, and up to 4096 before it), only as defence in depth: a path missed
  would read a dead entry rather than freed memory. Tests:
  `unchecked_erased_while_used` (a peer leaves exactly where each of these uses
  an entry, from another thread when `cs_main` is free there, and the entries
  that leave are overwritten so that a use shows; it failed on the code
  before), `attack_holder_leaves_while_its_blocks_connect`. (Review of audit 6: a
  holder that left while `ActivateBestChain` connected toward its headers made
  the node read a deleted block, a fatal error; `InvalidateBlock` could make an
  entry that left the best header, and `getblock` read a deleted block.)
- **Waiting headers keep their sender** (`AddBmmWaiting`). A header that waits
  (for the commitment to it, say) is kept with the peer that sent it, whichever
  way it came (a headers message, a compact block, a block). One whose sender is
  not known is tried again as `UNKNOWN_SOURCE`, never as the node's own: it holds
  no unchecked header, so it is taken in only if the node can check it. (Review
  of audit 6: blocks whose headers waited were taken as the node's, outside every
  budget, and never left the block index.)
- **Their blocks are stored apart.** The block of an unchecked header is stored
  in a file of its own (`blocks/unchecked/unc?????.dat`,
  `BlockManager::WriteVolatileBlock`), never in the block files, if its sender
  holds the header and the block fits its byte budget (else it is not stored, no
  fault of the peer's). It is charged to every holder of the header; a holder it
  does not fit stops holding the header and those on it. The file is deleted
  with the header, and the number reused, so sending the same block again after
  reconnecting, or other blocks, never grows the disk past what the connected
  peers hold. Once the block passes every rule as it is connected, it is written
  to the block files (before its undo data) and its own file deleted. The
  directory is emptied when the node starts; `-reindex` never sees these blocks.
- **One extension a header.** The first that came, until another valid one
  replaces it (`REPLACE`); a block with another one the node cannot check is the
  same block, stored with its header's. No other extension is kept.
- **A failed extension is the copy's fault.** A block whose header was unchecked
  and that fails for its extension alone when connected (`ExtensionFailure`) is
  marked failed in memory only, no fault of the peers (`BLOCK_TIME_FUTURE`, and
  `extension-failed` for the headers on it), and taken back by any other
  extension (`REPLACE`, or any other one if the node still cannot check it). The
  node asks the peers that hold the block's header, or whose best chain has it,
  for the headers from its parent (one pending `getheaders` a peer; with one
  pending already, from the last block both share, whose answer has the peer's
  copy of either): an honest peer's copy of the extension replaces the failed
  one. A failed block is never asked for again, and nothing is kept for a
  failure but that one pending request: unlike the bounded list of failed blocks
  asked for again before (review of audit 6, item 8a), there is nothing an
  attacker can fill (`extension_failure_asked_again_whatever_fails`: 300 blocks
  of an attacker's own fail around an honest one's failure, whose peer is still
  asked, and whose block connects).

Nothing in it is a rule of a block: it decides only whether and when the node
holds a header, and which copy of an extension it tries first. Tests:
`sidechain_hooks_tests.cpp` (`unchecked_*`, among them `unchecked_no_peer_drops_another`,
`unchecked_inbound_budget_and_holders` and `unchecked_blocks_stored_apart`, `block_extension_unknown_taken`,
`extension_failure_not_the_peers_fault`, `unchecked_erased_while_used`,
`extension_failure_asked_again_whatever_fails`) and the fuzz target
`sidechain_unchecked`.

## Spare headers

Where a header costs nothing to make (a signature, not proof of work), its signer
can make as many of one block as it likes (another merkle root each), and a node
would keep each one for good in its block index (on disk, in memory) -- no anti-DoS
work threshold applies to a chain whose order is not work (`CustomChainOrder`).
`HeaderSpare` lets the sidechain say that the block index has as many headers like
this one as the chain can need (FastBlocks: two of a slot, the first and the proof
that its signer signed twice); `HeaderKept` tells it of every entry that is in the
block index for good (taken checked, its block connected, loaded at startup).
A spare header from a peer (`AcceptBlockHeader`):

- **alone** (no next header of the same message on top of it): refused,
  `header-spare` (`BLOCK_HEADER_LOW_WORK`: not the peer's fault, the peer is asked
  again once the tip moved). A block whose header is spare and that the node does
  not have comes in alone too (a block, a compact block): refused the same way.
  But not one the node may need now (`ChainstateManager::SpareWanted`): one that
  would be its best header (more work, or the better key of the chain's own order,
  than any header it has), or the tip an outbound peer (one the node chose)
  announces, of a chain better than the active one. New tips are announced alone (a
  `headers` message of one header, a compact block), and the first headers like it
  may be junk its signer signed: such a header is taken *unchecked* as below, and
  its block fetched at once, not a block later.
- **with a header on top of it** in the same message (a branch some block builds
  on: a peer that sends a block on it answers `getheaders` from the fork, the spare
  header and what is on it): taken *unchecked* (above): held by that peer within
  its budget, not written to the block index database, gone with its holders, kept
  for good once its block is connected. So a node never cuts itself off from the
  branch the network builds on, and spare headers it keeps for good are those of
  connected blocks.

The node's own headers (source -1: its blocks, `-reindex`, `-loadblock`) are never
spare. Not a rule of a block. Tests: `sidechain_hooks_tests.cpp`
(`spare_headers_only_on_a_branch`, `spare_header_announced_by_an_outbound_peer`), and a sidechain's own (FastBlocks:
`attack6_leader_signs_headers_without_bound`).

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
