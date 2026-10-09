# zSide: private payments on a sidechain

zSide is a sidechain of Chains (slot 6) with the **Orchard** shielded pool of Zcash: coins in it
move without showing who pays whom, or how much — only the sender and the recipient know. It is
the idea of Paul Sztorc's zSide (a Zcash-like sidechain of Bitcoin), built on the sidechain
template of Chains with the Orchard library of the Zcash developers (`orchard`, Rust,
MIT/Apache-2.0), which zcashd and Zebra use.

## Transparent and shielded coins

A zSide wallet has two balances:

- **Transparent**, as on any sidechain: addresses `zs1…` (`tzs1…` on the test network), visible
  on the chain. Deposits from Chains arrive here, and withdrawals leave from here.
- **Shielded**, in the Orchard pool: addresses `zo1…` (`tzo1…`). A shielded payment shows nothing
  of its sender, recipient or amount; the recipient alone can read its **memo** (up to 512 bytes).

**Shielding** (`shieldcoins`) moves transparent coins in: that move is public, what happens to
the coins after is not. **Paying privately** (`sendshielded`) spends shielded notes; to a
transparent address it **unshields** — the amount and the address are then public, but not where
the coins came from. A shielded payment has no transparent input at all: its fee comes out of the
pool.

For privacy: shield amounts that do not stand out, wait a while before you unshield, and use a new
shielded address (`getshieldedaddress true`) for each payer — they all pay the same balance, and
nobody can tell they belong together.

## Melt & cast

The pool hides who pays whom, but what goes in and what comes out are public, and can link the
two: 13.47 shielded and 13.47 unshielded an hour later. Melt & cast, Paul Sztorc's design
(https://www.truthcoin.info/blog/zside-meltcast/), leaves nothing to link. It is a wallet feature:
nothing of it is a rule of the chain.

- **Melt** (`meltcoins ( spread )`): every transparent coin of the wallet in a block (of at least
  four times its fee) is shielded alone and whole: the coin is the only input of its transaction,
  the fee comes out of it, and there is no change. Each goes at a random height over the next
  `spread` blocks (36 by default). The coins are locked until they go, so nothing else spends them.
- **Cast** (`castcoins amount ( spread )`): the amount comes out as **bills**, 0.01 CHN times a
  power of two (0.01, 0.02, 0.04 … 10485.76): its binary digits, in hundredths. What is below
  0.01 stays shielded. Each bill goes to a new transparent address of the wallet, no two to one,
  with the fee out of the pool.
- **Common timings**: bill *k* goes out only in its **window**: with the tip at a height *h* where
  (*h* − 7*k*) mod 144 is below 3 (a period of 144 blocks, 12 on regtest). Every wallet casts that
  bill in the same blocks, so one cast of 0.64 looks like any other. A cast picks one of its bill's
  next `spread` + 1 windows (2 by default) at random.

The plan is kept in the wallet's database (`listmeltcast`, `cancelmeltcast`), and the wallet
carries it out as the blocks come, so it has to be running, and unlocked, at those heights. A step
that cannot go (the wallet locked, or not enough in blocks yet) tries again: a melt at the next
block, a cast at its bill's next window. To spend cast bills without linking them again, pay from
one bill at a time.

## How it is carried

- A transaction carries an Orchard **bundle**, encoded as in a Zcash v5 transaction (ZIP 225), in
  an OP_RETURN **marker** output: `ZORC`, a version byte, the bundle.
- The bundle's **value balance** is what it takes out of the pool: it counts as an input of the
  transaction (transparent outputs and the fee come out of it). What a bundle puts in, the marker
  output carries as its value, which the transparent inputs pay; it leaves circulation.
- A transaction that spends only shielded notes has no transparent input. It says so in its
  encoding: the extended format with flag 2 (as segwit's flag 1).
- The bundle's proof and signatures sign the **sighash**: the transaction, with the marker taken
  as the commitment to the bundle's effects (ZIP 244) instead of the bundle. The transparent
  inputs sign the whole transaction, the bundle included. The sighash also commits to the chain
  (its genesis block, its slot and a branch id): a bundle signed for one chain is invalid on any
  other.

## The rules

- Every bundle's proof (Halo 2, no trusted setup) and signatures are checked. The circuit is the
  one fixed at Zcash's NU6.2 (`FixedPostNu6_2`): the original Orchard circuit had a soundness flaw,
  and is never used.
- A note is spent once: the **nullifier** it reveals is kept, and a second one is refused.
- A bundle names its **anchor**, the root of the note commitment tree at the end of an earlier
  block; it proves that the notes it spends are in that tree.
- **The pool never holds less than nothing**: what bundles take out never exceeds what went in.
  Should the proofs ever have a flaw, no more than the pool holds could be taken.
- No bundle in a coinbase. A bundle has at most 256 actions.
- The tree holds fewer than 2^32 notes (a full tree's root would be the empty tree's).

## The node and the wallet

- The node keeps the right edge of the note commitment tree, the roots that are anchors, the
  nullifiers and the pool's value in its sidechain state. **`-orchardindex`** (on by default)
  keeps the whole tree, from which a wallet gets the path of its notes; wallets need it to spend.
- A spend in the mempool whose anchor a reorg took away leaves it once the reorg is done (not on
  the way, against a state in between); the wallet then abandons it, and its notes are the
  account's again.
- A payment spends at most as many notes as a transaction the network relays holds (about 30);
  past that the wallet says so: pay less, or join notes first by paying the account itself.
- Melt & cast run on a thread of the wallets of their own (not the scheduler's, which runs the
  block notifications), which stops between steps at shutdown or when the wallet is unloaded. The
  same thread scans again, in the background, the blocks an account set up late (on unlocking) has
  to: the commands that only read do not wait for it (`getshieldedbalance` says `rescanning_from`).
- A payment is proved without the wallet's lock (seconds): its notes and coins are picked and held
  back under the lock, and committed under it again if still unspent. Two payments made at once
  spend different notes.
- The wallet's shielded key is derived from the extended private key of its descriptors: nothing
  more to back up, and it is only at hand when the wallet is unlocked. Accounts set up from the
  fourth audit on derive it with the chain's genesis block and slot (version 2), so one seed makes
  unrelated keys on each chain; they also watch the key of the older derivation (version 1, the same
  on every chain) of the same descriptor, so a wallet restored from an older one's descriptor finds
  and spends its notes. An account set up before keeps its keys. The viewing keys are kept in the
  wallet, so that a locked wallet still sees what it receives. A rescan finds the notes again, also
  when a note's bundle went out in another transaction than the one the wallet saw first.
- A wallet with `-walletbroadcast=0` keeps the payments it makes for the user to send.
- The **Private** page of the wallet: the balances, the address with its QR code, paying
  privately with a memo, shielding, melt & cast with the plan, the history with the memos. It
  never calls the node on the GUI thread.

## Commands

Node: `getshieldedinfo`.
Wallet: `getshieldedaddress`, `getshieldedbalance`, `listshieldednotes`, `shieldcoins`,
`sendshielded`, `listshieldedtransactions`; `meltcoins`, `castcoins`, `listmeltcast`, `cancelmeltcast`.

## Building

The Orchard library is Rust: the build needs `cargo` (https://rustup.rs). CMake builds it
(`src/rust`, with the versions pinned in `Cargo.lock`) and links it in.
