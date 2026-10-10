// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_SHIELDED_H
#define BITCOIN_WALLET_SHIELDED_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <support/allocators/secure.h>
#include <uint256.h>
#include <wallet/meltcast.h>
#include <zside/orchard.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class CChainParams;

/**
 * zSide: the shielded account of a wallet.
 *
 * Its spending key is derived from the extended private key of the wallet's active bech32
 * descriptor, so it needs nothing more to back up, and is only at hand when the wallet is
 * unlocked. Version 2 of the derivation (accounts set up from the fourth audit on) mixes in the
 * chain's genesis block and slot: one seed makes unrelated keys on each chain. An account set up
 * before keeps its keys of version 1 (the same on every chain); one set up since also watches the
 * keys of version 1 of its descriptor, so that notes paid to them (before a restore from the
 * descriptor, say) are found and spent. Its full viewing key is kept in the wallet, so that a locked wallet still sees
 * what it receives. The notes are found by decrypting the shielded transactions the wallet
 * keeps: they come back with a rescan, and need no records of their own.
 *
 * The account is set up as soon as the key is at hand: on load, on unlocking, before the rescan of
 * an import. Set up after the wallet scanned blocks without it, it has the wallet scan them again,
 * from the wallet's birth (ShieldedRescan): a transaction that only pays the account, with no
 * transparent output of the wallet, was not kept then.
 */
namespace wallet {
class CWallet;
class CWalletTx;

using OrchardAddress = std::array<uint8_t, zside::ADDRESS_SIZE>;
using OrchardFvk = std::array<uint8_t, zside::FVK_SIZE>;
using OrchardNoteBytes = std::array<uint8_t, zside::NOTE_SIZE>;
using OrchardMemo = std::array<uint8_t, zside::MEMO_SIZE>;

/** A note of the wallet. */
struct ShieldedNote {
    //! The transactions that carry it, and its action in each: one, unless its bundle went out in
    //! another transaction too (proved again, or its transparent signatures malleated), which may be
    //! the one mined. The note is there while any of them is.
    std::vector<std::pair<Txid, uint32_t>> carriers;
    OrchardNoteBytes note{};
    OrchardMemo memo{};
    //! Change: to an internal address.
    bool internal{false};
    //! To a key of version 1 that an account of version 2 watches (ShieldedAccount::legacy_fvk).
    bool legacy{false};
    uint256 nullifier;
    uint256 cmx;
    CAmount value{0};
};

/** A payment the wallet made out of its shielded account, recovered from the chain. */
struct ShieldedPayment {
    Txid txid;
    uint32_t action{0};
    OrchardAddress recipient{};
    CAmount value{0};
    OrchardMemo memo{};
};

/** The shielded account of a wallet, as the wallet holds it. */
struct ShieldedAccount {
    //! The viewing key of the account: its addresses, its change, what it pays (kept in the database).
    std::optional<OrchardFvk> fvk;
    //! The version of its derivation (KEY_ANY_CHAIN when the database has none: set up before version 2).
    int key_version{1};
    //! Version 2: the viewing key of version 1 of the same descriptor, watched for the notes paid to it.
    std::optional<OrchardFvk> legacy_fvk;
    //! The notes the wallet received, by nullifier.
    std::map<uint256, ShieldedNote> notes;
    //! What the wallet's transactions spent: nullifier -> the transactions that reveal it.
    std::multimap<uint256, Txid> spends;
    //! The payments the wallet's transactions made to others (and to itself), by (txid, action).
    std::map<std::pair<Txid, uint32_t>, ShieldedPayment> payments;
    //! Melt/cast: the plan, by id (kept in the wallet's database).
    std::map<uint64_t, MeltCastJob> jobs;
    //! Set up after the wallet scanned blocks without it: the height to scan again from (kept in the database).
    std::optional<int> rescan_from;
    //! The notes payments being proved spend (by nullifier), which the wallet's lock is not held for:
    //! no other payment picks them meanwhile (not kept).
    std::set<uint256> in_flight;
};

/** A spending key, in memory that is locked and wiped when freed. */
using ShieldedKey = std::vector<uint8_t, secure_allocator<uint8_t>>;

/** Notes are spent at the tree of the block with this many confirmations, or of the newest of their own blocks:
 *  a spend is not undone by a reorg of the blocks above it. */
inline constexpr int ANCHOR_CONFIRMATIONS{3};

/** Shielded addresses: Bech32m, "zo" on the main network ("tzo" on the test networks, "rzo" on regtest). */
std::string EncodeShieldedAddress(const OrchardAddress& address);
std::optional<OrchardAddress> DecodeShieldedAddress(const std::string& text);
/** A memo as text: UTF-8 up to the first zero byte; empty for the "no memo" memo. */
std::string MemoText(const OrchardMemo& memo);
/** A memo from text (at most 512 bytes); the "no memo" memo (0xF6) for empty text. */
std::optional<OrchardMemo> MemoFromText(const std::string& text);

/** The derivations of the account's keys: of the descriptor's extended key alone, or of the chain too. */
inline constexpr int KEY_ANY_CHAIN{1};
inline constexpr int KEY_THIS_CHAIN{2};
/** A spending key of the wallet's shielded account, of a version: nullopt if the wallet is locked or has no private keys. */
std::optional<ShieldedKey> ShieldedSpendingKey(const CWallet& wallet, int version);
/** The same, for the chain of `genesis` and `slot` (version 2; version 1 ignores them). */
std::optional<ShieldedKey> ShieldedSpendingKey(const CWallet& wallet, int version, const uint256& genesis, uint32_t slot);
/**
 * The spending key of a viewing key of the account, of a version: of the wallet's active descriptor, or
 * of another of its descriptors (encryptwallet makes new ones active; another may have been imported
 * active since the account was set up). Nullopt if no descriptor of the wallet makes it, or the wallet
 * is locked.
 */
std::optional<ShieldedKey> AccountSpendingKey(const CWallet& wallet, int version, const OrchardFvk& fvk);
/** The viewing keys the account watches, the account's own first; with whether it is the one of version 1 it also watches. */
std::vector<std::pair<const OrchardFvk*, bool>> ShieldedViewingKeys(const ShieldedAccount& account);
/** What an address is to the account: 1 one of its own, 2 its change, 0 another's (zo_address_scope, for each key it watches). */
int ShieldedAddressScope(const ShieldedAccount& account, const OrchardAddress& address);
/**
 * Make the viewing key known (from the spending key) and keep it, if it is not yet. If the wallet
 * scanned blocks already, it notes they are to be scanned again (ShieldedRescan).
 */
bool SetupShieldedAccount(CWallet& wallet);
/** Scan the blocks again that the wallet scanned before its shielded account was set up, if any (the wallet not locked by the caller). */
void ShieldedRescan(CWallet& wallet);
/** A scan of the chain from `start_height` to the tip went through: the account needs no other. */
void ShieldedScanned(CWallet& wallet, int start_height);
/** The transactions of the wallet, other than `tx`, that reveal a nullifier it reveals: they conflict with it. */
std::vector<Txid> ShieldedConflicts(const CWallet& wallet, const CTransaction& tx);
/** An address of the account: `index` 0 is the default one; others look unrelated to it. */
std::optional<OrchardAddress> ShieldedAddress(const CWallet& wallet, uint64_t index, bool internal = false);

/** Take in a transaction the wallet keeps: the notes it pays the wallet, what it spends, what it paid. */
void ShieldedTxAdded(CWallet& wallet, const CWalletTx& wtx);
/** Whether a transaction concerns the shielded account: pays it, spends its notes, or was made by it. */
bool IsShieldedMine(const CWallet& wallet, const CTransaction& tx);

/** A note with where it stands. */
struct NoteStatus {
    const ShieldedNote* note{nullptr};
    //! The transaction that carries it, of those not abandoned nor conflicted: the one deepest in the chain.
    Txid txid;
    uint32_t action{0};
    int confirmations{0};
    //! The transaction that spends it, if any (in a block or waiting).
    std::optional<Txid> spent_by;
};
/** The notes of the account a transaction of the wallet carries (abandoned and conflicted ones left out). */
std::vector<NoteStatus> ShieldedNotes(const CWallet& wallet);

struct ShieldedBalance {
    //! In notes in blocks, not spent.
    CAmount available{0};
    //! In notes waiting for a block (change and incoming), not spent.
    CAmount pending{0};
    int notes{0};
};
ShieldedBalance GetShieldedBalance(const CWallet& wallet);

/** A payment to make: to a shielded address (with a memo) or to a transparent script. */
struct ShieldedRecipient {
    std::optional<OrchardAddress> shielded;
    CScript script;
    CAmount amount{0};
    OrchardMemo memo{};
};

/**
 * How many notes a payment out of the shielded account may spend, with these transparent outputs and
 * shielded ones (its change included), for a transaction the network relays (MAX_STANDARD_TX_WEIGHT):
 * nullopt if the outputs alone make it too large.
 */
std::optional<size_t> MaxShieldedSpends(const std::vector<CTxOut>& transparent, size_t shielded_outputs);
/**
 * Pay out of the shielded account: no transparent inputs; the change goes back to the account.
 * @param[in] fee  the fee, or none for the wallet's fee rate
 * @param[in] subtract_fee  take the fee out of the one transparent recipient's amount (unshielding all of it)
 * @return the transaction, or the error
 * Not with the wallet's lock held: it waits for -orchardindex to catch up first (but not on the
 * scheduler's thread, where it finds the notes in the index as it is). The notes are picked and the
 * bundle built under the wallet's lock; it is proved without it (seconds), its notes and coins held
 * back from other payments meanwhile; it is committed under the lock again if they are still unspent.
 * So are ShieldCoins, ShieldCoin and BumpShieldedFee made.
 */
std::variant<CTransactionRef, std::string> SendShielded(CWallet& wallet, const std::vector<ShieldedRecipient>& recipients, std::optional<CAmount> fee = std::nullopt, bool subtract_fee = false);
/**
 * Pay the same again, with a higher fee: a transaction that replaces one of the wallet's, out of its shielded
 * account, waiting for a block (it spends the same notes, and more if they do not cover the fee).
 * @param[in] fee  the new fee, or none for the lowest the mempool takes as a replacement
 * Not with the wallet's lock held, as SendShielded.
 */
std::variant<CTransactionRef, std::string> BumpShieldedFee(CWallet& wallet, const Txid& txid, std::optional<CAmount> fee = std::nullopt);
/**
 * Whether the mempool refused a transaction the wallet just committed: then it is abandoned, and this
 * is the error. One a block took from the mempool since the commit went, and is kept (no error):
 * waits for the wallet to catch up with the chain to tell. A wallet that does not broadcast
 * (-walletbroadcast=0) refuses nothing: it never hands its transactions to the mempool.
 * Not with the wallet's lock held.
 */
std::optional<std::string> RefusedCommit(CWallet& wallet, const CTransactionRef& tx);
/** Move transparent coins of the wallet into its shielded account (or to another shielded address). Not with the wallet's lock held. */
std::variant<CTransactionRef, std::string> ShieldCoins(CWallet& wallet, CAmount amount, const OrchardAddress& to, const OrchardMemo& memo);

/**
 * Move one transparent coin of the wallet, whole, into its shielded account: that coin as the only
 * input, the fee out of it, no change. Melting shields each coin so, linking no two coins.
 * Not with the wallet's lock held.
 */
std::variant<CTransactionRef, std::string> ShieldCoin(CWallet& wallet, const COutPoint& coin, const OrchardAddress& to);

/** Bytes of a bundle of `actions` actions, as a marker carries it. */
size_t BundleSize(size_t actions);
/** Actions a bundle gets for so many spends and outputs (two at least, for privacy). */
size_t ActionCount(size_t spends, size_t outputs);

} // namespace wallet

#endif // BITCOIN_WALLET_SHIELDED_H
