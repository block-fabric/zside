# Drivechain notes

Notes on choices in the drivechain rules of Chains that the code alone does not explain.
The rules themselves are in `src/drivechain/scdb.cpp`; their parameters, per network, in
`Consensus::DrivechainParams` (`src/consensus/params.h`, `src/kernel/chainparams.cpp`).

## Withdrawal bundles nobody vouches for

Any miner can propose (M3) a withdrawal bundle for any sidechain: a 32 byte hash, which need not be
the hash of anything. Sidechain software has to treat every bundle pending for its slot as one that
may still be paid, so a pending bundle holds up the refunds of the withdrawals it might pay, and the
next bundle of the sidechain. A bundle nobody vouches for therefore stalls the withdrawals of a
sidechain for as long as it stays pending.

How long a bundle stays pending, from `audit2_height` on (0 on mainnet, signet and regtest):

* it fails once `upvote_expiry_blocks` blocks in a row did not upvote it, counted from the block that
  proposed it or from its last upvote (144 blocks, about two and a half hours, on mainnet; 30 on the
  test network; 20 on regtest);
* it fails at a score of 0 once it is `idle_expiry_blocks` old (1008 on mainnet): miners that run the
  sidechain downvote the bundles their sidechain node does not vouch for, which brings them to 0;
* it fails once the blocks left in the withdrawal period cannot bring it to the score it needs, and at
  the end of the withdrawal period.

A bundle the sidechain's miners vouch for is upvoted by every block they find, so the first rule
never touches it as long as they find a block every `upvote_expiry_blocks` blocks; a bundle that is to
be paid needs more than half of the blocks to upvote it anyway. The miner of this node upvotes the
bundle its sidechain node vouches for in every block from the one after the block that proposed it.

Residual risk. Proposing a bundle costs nothing but a block. A miner that wants to stall a sidechain
has to find, every `upvote_expiry_blocks` blocks, a block that upvotes its bundle (and, once the
honest miners' downvotes have taken it to 0, propose a new one every `idle_expiry_blocks`): about
0.7% of the hashrate on mainnet, where before it took one block in 1008 (or one in 64800 where nobody
downvotes). That makes the attack cost something for as long as it lasts, but does not rule it out:
a miner with a percent of the hashrate can keep a bundle pending for good. Rule it out takes a change
on the sidechain side: not counting a bundle as one that may be paid once it can no longer reach the
score in time (`score`, `blocksleft` and `lastupvote` are in `listwithdrawalbundles`, and
`getsidechainevents` tells what each block proposed and closed), or binding a bundle to something
only the sidechain can produce.

## What sidechain software reads: getsidechainevents

`getsidechainevents <slot> <height> [count]` gives, per block of the active chain, what the block
did that concerns one sidechain:

* `bmm`: the sidechain block it committed to (blind merged mining), if any;
* `deposits`: its changes to the escrow, deposits and the change of withdrawals (with what a
  withdrawal paid);
* `bundles`: the withdrawal bundles it closed, `{"hash", "paid"}` (paid out, or failed);
* `proposed`: the bundles it proposed (M3) that became pending;
* `pending`: the bundles of the sidechain pending after the block, `[{"hash": hex, "score": n}]`, in
  the order vote messages number them (empty if none).

`pending` is what a sidechain needs to know whether a withdrawal may still be paid (and so must not
be refunded yet): the bundles proposed and not closed as of that block, with their score after it.
It is computed when the block is connected and stored with the other events of the block, which are
kept for good, so it is the same for any height and at any time: after a restart, after the
database is rebuilt, and on every node. A reorg replaces the records of the blocks it takes out with
those of the blocks it connects. A sidechain that follows the chain block by block can take the
`pending` list of the last block it processed as the set of bundles that may still be paid, without
replaying every proposal and closure since its slot activated.

## What is kept, and for how long

* The sidechain database itself, in memory, with a snapshot (with a format version: a snapshot in
  another format is not read, and the database is rebuilt from the blocks) written whenever the
  chainstate is flushed. The drivechain data on disk has a format version too: a node that finds
  data of an older version wipes it and rebuilds it from the blocks when it starts (a pruned node,
  which no longer has them, has to be started with `-reindex`). The snapshot and the format marker
  also carry a fingerprint of all the drivechain parameters of the network (`ParamsFingerprint`):
  activation heights, periods, expiries, limits. A node whose parameters changed (a new release that
  moves an activation height, say) finds a fingerprint that does not match, and wipes and rebuilds
  the drivechain data from genesis the same way, rather than keep a state derived under the old rules.
* Per block, what it did that sidechain software follows (the bundles it closed, paid or failed,
  those it proposed, those pending after it with their scores, and its escrow changes): for good,
  for `getsidechainevents` and `listsidechaindeposits`. The pending bundles cost about 36 bytes per
  bundle per block for the sidechains that have any.
* Per block, the changes it made to the database, to take it back in a reorg: only for the blocks
  of the active chain less than `DRIVECHAIN_UNDO_DEPTH` (2880) below the last block flushed to disk
  (erased as the chainstate is flushed, not as blocks connect: a node that stops uncleanly restarts
  from the flushed block, and must find the undo data of every block a reorg from there can reach). A reorg deeper than that derives the
  database from the blocks (slow, and impossible on a pruned node, which cannot reorg that deep in
  any case); `verifychain` goes as deep as the undo data.
* Bundles that were paid out, for good: they can never be proposed again. Bundles that failed, for a
  withdrawal period after they failed (from `audit2_height`); after that the same hash can be proposed
  again, as a new bundle that needs all its votes again. By then the sidechain has had a whole
  withdrawal period to refund its withdrawals, and paying it out again would take the same majority
  of the hashrate, upvoting for as long, as paying out any bundle nobody vouches for.
  A failed bundle that no block ever upvoted is forgotten sooner, `unvoted_forget_blocks` after it
  failed (1008 on mainnet, 60 on the test network, 20 on regtest; never later than a withdrawal
  period). Those are what a miner adds to the state by proposing bundles nobody votes for, one per
  sidechain per block: remembered for a whole period (129600 blocks on mainnet) they would let it
  grow the state by up to that many entries per sidechain, for as long as it keeps at it; now by
  1008. Forgetting them sooner gives nothing away: proposed again, such a bundle starts where it
  started the first time, from the score of a new bundle, and needs every one of its votes from
  miners whose sidechain node does not vouch for it (the sidechain refunded it); its failure stays
  on record for good outside the state (`getsidechainevents`, the closure index behind
  `getwithdrawalbundle`, and the miner's own record), so no node takes it for a new one. A bundle
  that some block upvoted, which some miner did back, keeps the whole period.
  `getwithdrawalbundle` keeps answering "failed" (from the index of the blocks that closed each
  bundle, kept for good), and `getsidechainevents` keeps saying it failed.
* The miner of a node keeps, with each bundle a sidechain node handed it (`drivechain_miner.dat`),
  whether the chain proposed it and the height of the block that closed it. A bundle the chain
  closed once is never proposed or upvoted again by this node, even once the chain forgot that it
  failed (the index of closures tells, whenever the node builds a block); it is dropped
  `PRUNE_DEPTH` (6) blocks after it closed. A handed bundle no block proposed is dropped after
  1008 blocks, unless it is the one the sidechain node vouches for.

## Other limits

* UTXO snapshots (assumeutxo) do not hold the drivechain state: `loadtxoutset` refuses them, and so
  does `ActivateSnapshot` on every network but regtest (whose upstream tests load snapshots of
  heights where no sidechain exists; the chainstate made from one starts with an empty sidechain
  database).
* Deriving the sidechain database from the blocks (after a format or parameter change, or for a
  reorg deeper than the undo data) can take long. A shutdown interrupts it cleanly: at startup what
  was derived so far is kept and the next start goes on from there; during a deep reorg the node
  stays on the tip it had.
* `listsidechaindeposits` lists only the records of blocks of the active chain (a crash between a
  reorg and the next flush can leave records of others); they are skipped without counting towards
  `count`, and an `after` whose record is such a stale one resumes from the record of the same
  transaction in the active chain, or is refused.
