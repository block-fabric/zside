// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_SIDECHAIN_H
#define BITCOIN_DRIVECHAIN_SIDECHAIN_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * Drivechain data structures and script formats.
 *
 * The formats are those of BIP300/BIP301 as implemented by the Layer Two Labs
 * enforcer (bip300301_enforcer), so that pools and tools made for it read a
 * Chains block the same way. Miners coordinate sidechains through messages
 * placed in OP_RETURN outputs of the coinbase transaction. Every message is a
 * single data push that starts with a four byte tag; a slot is one byte:
 *
 *   M1  propose sidechain    PROPOSAL_TAG | slot | description (see Sidechain::Description)
 *   M2  ack proposal         ACK_TAG      | slot | sha256d(description)
 *   M3  propose bundle       BUNDLE_TAG   | slot | M6 id
 *   M4  vote on bundles      VOTE_TAG     | form | one vote per active sidechain, in slot order
 *   M7  accept BMM request   BMM_ACCEPT_TAG | slot | sidechain block hash
 *
 * Three more are ordinary transactions:
 *
 *   M5  deposit      adds coins to the treasury output of a sidechain; the
 *                    output after it is OP_RETURN <destination on the sidechain>
 *   M6  withdrawal   spends the treasury output (its only input) and pays out a
 *                    bundle; output 0 is the new treasury output
 *   M8  BMM request  output 0: OP_RETURN BMM_REQUEST_TAG | slot | sidechain block hash | previous mainchain block hash
 */
namespace drivechain {

/** Sidechain slot number. On the wire it is a single byte. */
using SidechainId = uint32_t;
/** Number of slots a single byte can name. */
inline constexpr uint32_t MAX_SLOTS{256};

/** Highest sidechain description version this software understands. */
inline constexpr int32_t SIDECHAIN_VERSION_MAX{0};
/** Largest title of a sidechain proposal, in bytes; its length is a single byte. */
inline constexpr size_t MAX_TITLE_SIZE{255};
/** Largest description of a sidechain proposal, in bytes. */
inline constexpr size_t MAX_DESCRIPTION_SIZE{1024};
/** Largest deposit destination, in bytes. */
inline constexpr size_t MAX_DEPOSIT_DESTINATION_SIZE{100};
/** Destination reported to sidechain software for the change of a withdrawal (not part of any transaction). */
inline const std::string WITHDRAWAL_RETURN_DEST{"D"};

/** Forms of the vote message (M4). */
enum class VoteForm : uint8_t {
    //! The votes of the previous block again.
    REPEAT_PREVIOUS = 0x00,
    //! One byte per active sidechain.
    ONE_BYTE = 0x01,
    //! Two bytes, little endian, per active sidechain; only when some value does not fit in a byte.
    TWO_BYTES = 0x02,
    //! Upvote, for every sidechain, the bundle that leads the next one by LEADING_BY_50_MARGIN.
    LEADING_BY_50 = 0x03,
};
/** Vote value: abstain from voting on the bundles of a sidechain. */
inline constexpr uint16_t VOTE_ABSTAIN{0xFFFF};
/** Vote value ("alarm"): downvote every bundle of a sidechain. */
inline constexpr uint16_t VOTE_DOWNVOTE{0xFFFE};
inline constexpr uint8_t VOTE_ABSTAIN_ONE_BYTE{0xFF};
inline constexpr uint8_t VOTE_DOWNVOTE_ONE_BYTE{0xFE};
/** Largest bundle index the one byte form can carry. */
inline constexpr uint16_t VOTE_MAX_ONE_BYTE_INDEX{0xFD};
/** How many votes the leading bundle needs over the next one for LEADING_BY_50. */
inline constexpr uint32_t LEADING_BY_50_MARGIN{50};

inline constexpr unsigned char PROPOSAL_TAG[4]{0xD5, 0xE0, 0xC4, 0xAF};
inline constexpr unsigned char ACK_TAG[4]{0xD6, 0xE1, 0xC5, 0xDF};
inline constexpr unsigned char BUNDLE_TAG[4]{0xD4, 0x5A, 0xA9, 0x43};
inline constexpr unsigned char VOTE_TAG[4]{0xD7, 0x7D, 0x17, 0x76};
inline constexpr unsigned char BMM_ACCEPT_TAG[4]{0xD1, 0x61, 0x73, 0x68};
inline constexpr unsigned char BMM_REQUEST_TAG[3]{0x00, 0xBF, 0x00};

/** Description of a sidechain, as proposed by a miner. */
struct Sidechain {
    SidechainId slot{0};
    int32_t version{0};
    std::string title;
    std::string description;
    //! Hash of the release tarball of the sidechain software.
    uint256 hash_id1;
    //! Hash of the commit the sidechain software was built from.
    uint160 hash_id2;

    SERIALIZE_METHODS(Sidechain, obj) { READWRITE(obj.slot, obj.version, obj.title, obj.description, obj.hash_id1, obj.hash_id2); }

    /**
     * The description as it appears in a proposal (BIP300 M1 v0):
     * version (1 byte) | title length (1 byte) | title | description | hash_id1 | hash_id2.
     */
    std::vector<unsigned char> Description() const;
    /** sha256d of the description: what miners ack, together with the slot. */
    uint256 GetHash() const;
    /** Whether the fields are within the limits a proposal has to respect. */
    bool IsValid(uint32_t max_sidechains) const;

    friend bool operator==(const Sidechain&, const Sidechain&) = default;
};

/** Work score of a bundle in the block that proposes it. */
inline constexpr uint32_t NEW_BUNDLE_SCORE{1};
/** Proposals (M1) a block may make; any beyond are ignored. */
inline constexpr size_t MAX_PROPOSALS_PER_BLOCK{8};

/** A sidechain proposal that is collecting acks. */
struct Proposal {
    Sidechain sidechain;
    //! sidechain.GetHash(), kept so that looking proposals up hashes nothing.
    uint256 hash;
    //! Height of the block that contained the proposal.
    int32_t height{0};
    //! Number of blocks after the proposal block that acked the proposal.
    uint32_t acks{0};

    SERIALIZE_METHODS(Proposal, obj) { READWRITE(obj.sidechain, obj.hash, obj.height, obj.acks); }

    friend bool operator==(const Proposal&, const Proposal&) = default;
};

/** The escrow output of a sidechain ("critical txid-index pair"). */
struct Ctip {
    COutPoint outpoint;
    CAmount amount{0};

    SERIALIZE_METHODS(Ctip, obj) { READWRITE(obj.outpoint, obj.amount); }

    friend bool operator==(const Ctip&, const Ctip&) = default;
};

/** A withdrawal bundle that miners are voting on. */
struct Bundle {
    //! Blind hash of the withdrawal transaction.
    uint256 hash;
    //! Height of the block that proposed the bundle.
    int32_t height{0};
    uint32_t score{0};

    SERIALIZE_METHODS(Bundle, obj) { READWRITE(obj.hash, obj.height, obj.score); }

    friend bool operator==(const Bundle&, const Bundle&) = default;
};

/** Everything consensus tracks about an active sidechain. */
struct Slot {
    Sidechain sidechain;
    //! Height at which the sidechain was activated.
    int32_t activation_height{0};
    bool has_ctip{false};
    Ctip ctip;
    //! Pending withdrawal bundles, oldest first.
    std::vector<Bundle> bundles;

    SERIALIZE_METHODS(Slot, obj) { READWRITE(obj.sidechain, obj.activation_height, obj.has_ctip, obj.ctip, obj.bundles); }

    friend bool operator==(const Slot&, const Slot&) = default;
};

/** A change of the coins held in escrow by a sidechain, reported to sidechain software. */
struct Deposit {
    SidechainId slot{0};
    //! Destination on the sidechain, or WITHDRAWAL_RETURN_DEST for the change of a withdrawal.
    std::string destination;
    //! The transaction, with its escrow output at index `burn_index`.
    CTransactionRef tx;
    uint32_t burn_index{0};
    //! Position of the transaction in its block.
    uint32_t tx_index{0};
    //! Coins added to the escrow by this transaction; zero for a withdrawal.
    CAmount amount{0};
    //! Coins held in escrow after this transaction.
    CAmount total{0};
    uint256 block_hash;
    //! For a withdrawal: the bundle it paid out (its M6 id).
    uint256 bundle;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << slot << destination << TX_WITH_WITNESS(tx) << burn_index << tx_index << amount << total << block_hash << bundle;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> slot >> destination >> TX_WITH_WITNESS(tx) >> burn_index >> tx_index >> amount >> total >> block_hash >> bundle;
    }
};

/**
 * A deposit address: where on a sidechain a deposit goes, in a form that says
 * which sidechain it is for and that does not survive a typing error:
 *
 *     s<slot>_<address on the sidechain>_<checksum>
 *
 * The checksum is the first six hexadecimal digits of the SHA-256 hash of what
 * comes before it, the last underscore included.
 */
struct DepositAddress {
    SidechainId slot{0};
    std::string address;
};

enum class DepositAddressKind {
    //! Not in the format of a deposit address: a destination as it is.
    PLAIN,
    VALID,
    //! In the format, but for the checksum or the slot number.
    INVALID,
};

std::string FormatDepositAddress(const DepositAddress& deposit_address);
/** Tell what `text` is, and take it apart if it is a valid deposit address. */
DepositAddressKind ParseDepositAddress(const std::string& text, DepositAddress& deposit_address);

/** A request for blind merged mining found in a transaction output. */
struct BmmRequest {
    SidechainId slot{0};
    //! Hash of the sidechain block to commit to.
    uint256 side_block_hash;
    //! Mainchain block the request must be mined on top of.
    uint256 prev_main_block_hash;
};

/** A vote message (M4). */
struct VoteMessage {
    VoteForm form{VoteForm::ONE_BYTE};
    //! For ONE_BYTE and TWO_BYTES: per active sidechain, a bundle index, VOTE_ABSTAIN or VOTE_DOWNVOTE.
    std::vector<uint16_t> votes;

    friend bool operator==(const VoteMessage&, const VoteMessage&) = default;
};

/** A vote of a miner on the withdrawal bundles of one sidechain. */
struct Vote {
    enum class Type : uint8_t { ABSTAIN, DOWNVOTE, UPVOTE };
    Type type{Type::ABSTAIN};
    //! Bundle to upvote.
    uint256 bundle;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << static_cast<uint8_t>(type) << bundle;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t value;
        s >> value >> bundle;
        type = value <= static_cast<uint8_t>(Type::UPVOTE) ? static_cast<Type>(value) : Type::ABSTAIN;
    }

    friend bool operator==(const Vote&, const Vote&) = default;
};

//
// Script formats.
//

/**
 * Script of the escrow ("treasury") output of the sidechain in `slot`:
 * OP_DRIVECHAIN OP_PUSHBYTES_1 <slot> OP_TRUE. At the script level anyone can
 * spend it, with an empty scriptSig; the drivechain rules restrict how.
 */
CScript EscrowScript(SidechainId slot);
/** If `script` is an escrow script, return its slot. */
std::optional<SidechainId> ParseEscrowScript(const CScript& script);

CScript ProposalScript(const Sidechain& sidechain);
std::optional<Sidechain> ParseProposalScript(const CScript& script);

CScript AckScript(SidechainId slot, const uint256& proposal_hash);
std::optional<std::pair<SidechainId, uint256>> ParseAckScript(const CScript& script);

CScript BundleScript(SidechainId slot, const uint256& bundle_hash);
std::optional<std::pair<SidechainId, uint256>> ParseBundleScript(const CScript& script);

/** The vote message for `votes` (one per active sidechain), in the one byte form unless an index needs two. */
VoteMessage MakeVoteMessage(const std::vector<uint16_t>& votes);
CScript VoteScript(const VoteMessage& message);
std::optional<VoteMessage> ParseVoteScript(const CScript& script);

CScript BmmAcceptScript(SidechainId slot, const uint256& side_block_hash);
std::optional<std::pair<SidechainId, uint256>> ParseBmmAcceptScript(const CScript& script);

CScript BmmRequestScript(const BmmRequest& request);
std::optional<BmmRequest> ParseBmmRequestScript(const CScript& script);

/** Script of an OP_RETURN output carrying a deposit destination; in a deposit it follows the treasury output. */
CScript DestinationScript(const std::string& destination);
/** If `script` is an OP_RETURN followed by a data push, return the pushed data as a string. */
std::optional<std::string> ParseDestinationScript(const CScript& script);

/** Output 0 of the blind form of a withdrawal: OP_RETURN and the fee it pays mainchain miners, 8 bytes big endian. */
CScript WithdrawalFeeScript(CAmount fee);
std::optional<CAmount> ParseWithdrawalFeeScript(const CScript& script);

//
// Withdrawal transactions.
//

/**
 * A sidechain builds a withdrawal bundle without knowing which treasury output
 * it will spend or how much will be left in it. It hands miners the blind form
 * (BIP300 "M6 blinded"): no inputs, output 0 OP_RETURN <fee> with no value,
 * then the payouts. Its txid is the M6 id miners vote on. The withdrawal
 * itself spends the treasury output as its only input and has the new
 * treasury output in place of output 0.
 */
bool IsBlindWithdrawal(const CTransaction& tx);
/** The blind form of the withdrawal `tx`, given the amount of the treasury output it spends. */
std::optional<CMutableTransaction> BlindWithdrawalTx(const CTransaction& tx, CAmount treasury_amount);
std::optional<uint256> BlindWithdrawalHash(const CTransaction& tx, CAmount treasury_amount);
/**
 * The withdrawal that pays out the blind bundle `blind` of the sidechain in
 * `slot` from its treasury output `ctip`; nullopt if the treasury cannot pay it.
 * Sets `fee` to what it leaves mainchain miners.
 */
std::optional<CMutableTransaction> CompleteWithdrawal(const CTransaction& blind, SidechainId slot, const Ctip& ctip, CAmount* fee = nullptr);

/** The BMM request of a transaction: output 0, if it is one. */
std::optional<BmmRequest> GetBmmRequest(const CTransaction& tx);
/** GetBmmRequest as a list, for loops. */
std::vector<BmmRequest> GetBmmRequests(const CTransaction& tx);

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_SIDECHAIN_H
