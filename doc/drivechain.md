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

## What is kept, and for how long

* The sidechain database itself, in memory, with a snapshot (with a format version: a snapshot in
  another format is not read, and the database is rebuilt from the blocks) written whenever the
  chainstate is flushed. The drivechain data on disk has a format version too: a node that finds
  data of an older version wipes it and rebuilds it from the blocks when it starts (a pruned node,
  which no longer has them, has to be started with `-reindex`).
* Per block, what it did that sidechain software follows (the bundles it closed, paid or failed, and
  those it proposed, and its escrow changes): for good, for `getsidechainevents` and
  `listsidechaindeposits`.
* Per block, the changes it made to the database, to take it back in a reorg: only for the last
  `DRIVECHAIN_UNDO_DEPTH` (2880) blocks of the active chain. A reorg deeper than that derives the
  database from the blocks (slow, and impossible on a pruned node, which cannot reorg that deep in
  any case); `verifychain` goes as deep as the undo data.
* Bundles that were paid out, for good: they can never be proposed again. Bundles that failed, for a
  withdrawal period after they failed (from `audit2_height`); after that the same hash can be proposed
  again, as a new bundle that needs all its votes again. By then the sidechain has had a whole
  withdrawal period to refund its withdrawals, and paying it out again would take the same majority
  of the hashrate, upvoting for as long, as paying out any bundle nobody vouches for.
  `getwithdrawalbundle` then answers "unknown"; `getsidechainevents` keeps saying it failed.
