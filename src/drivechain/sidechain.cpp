// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/sidechain.h>

#include <crypto/common.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <script/script.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/strencodings.h>

#include <algorithm>
#include <span>

namespace drivechain {

namespace {

/** Script consisting of OP_RETURN and a single push of `tag` followed by `payload`. */
CScript MessageScript(std::span<const unsigned char> tag, std::span<const unsigned char> payload)
{
    std::vector<unsigned char> data(tag.begin(), tag.end());
    data.insert(data.end(), payload.begin(), payload.end());
    return CScript() << OP_RETURN << data;
}

/** If `script` is OP_RETURN and a single data push and nothing else, return the pushed data. */
std::optional<std::vector<unsigned char>> ParsePushScript(const CScript& script)
{
    if (script.empty() || script[0] != OP_RETURN) return std::nullopt;
    CScript::const_iterator pc{script.begin() + 1};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode, data)) return std::nullopt;
    if (opcode > OP_PUSHDATA4) return std::nullopt;
    if (pc != script.end()) return std::nullopt;
    return data;
}

/** The payload of a message script carrying `tag`. */
std::optional<std::vector<unsigned char>> ParseMessageScript(const CScript& script, std::span<const unsigned char> tag)
{
    auto data{ParsePushScript(script)};
    if (!data || data->size() < tag.size()) return std::nullopt;
    if (!std::equal(tag.begin(), tag.end(), data->begin())) return std::nullopt;
    data->erase(data->begin(), data->begin() + tag.size());
    return data;
}

std::vector<unsigned char> SlotAndHash(SidechainId slot, const uint256& hash)
{
    std::vector<unsigned char> payload{static_cast<unsigned char>(slot)};
    payload.insert(payload.end(), hash.begin(), hash.end());
    return payload;
}

std::optional<std::pair<SidechainId, uint256>> ParseSlotAndHash(const std::vector<unsigned char>& payload)
{
    if (payload.size() != 1 + 32) return std::nullopt;
    return std::make_pair(SidechainId{payload[0]}, uint256{std::span<const unsigned char>{payload}.subspan(1)});
}

//! OP_DRIVECHAIN OP_PUSHBYTES_1 <slot> OP_TRUE
constexpr size_t ESCROW_SCRIPT_SIZE{4};
//! OP_RETURN OP_PUSHBYTES_68 <tag> <slot> <sidechain block hash> <previous mainchain block hash>
constexpr size_t BMM_REQUEST_DATA_SIZE{3 + 1 + 32 + 32};

} // namespace

std::vector<unsigned char> Sidechain::Description() const
{
    std::vector<unsigned char> data;
    data.push_back(static_cast<unsigned char>(version));
    data.push_back(static_cast<unsigned char>(title.size()));
    data.insert(data.end(), title.begin(), title.end());
    data.insert(data.end(), description.begin(), description.end());
    data.insert(data.end(), hash_id1.begin(), hash_id1.end());
    data.insert(data.end(), hash_id2.begin(), hash_id2.end());
    return data;
}

uint256 Sidechain::GetHash() const
{
    return Hash(Description());
}

bool Sidechain::IsValid(uint32_t max_sidechains) const
{
    return slot < std::min(max_sidechains, MAX_SLOTS) &&
           version >= 0 && version <= SIDECHAIN_VERSION_MAX &&
           !title.empty() && title.size() <= MAX_TITLE_SIZE &&
           description.size() <= MAX_DESCRIPTION_SIZE;
}

CScript EscrowScript(SidechainId slot)
{
    return CScript() << OP_DRIVECHAIN << std::vector<unsigned char>{static_cast<unsigned char>(slot)} << OP_TRUE;
}

std::optional<SidechainId> ParseEscrowScript(const CScript& script)
{
    if (script.size() != ESCROW_SCRIPT_SIZE) return std::nullopt;
    if (script[0] != OP_DRIVECHAIN || script[1] != 1 || script[3] != OP_TRUE) return std::nullopt;
    return SidechainId{script[2]};
}

CScript ProposalScript(const Sidechain& sidechain)
{
    std::vector<unsigned char> payload{static_cast<unsigned char>(sidechain.slot)};
    const std::vector<unsigned char> description{sidechain.Description()};
    payload.insert(payload.end(), description.begin(), description.end());
    return MessageScript(PROPOSAL_TAG, payload);
}

std::optional<Sidechain> ParseProposalScript(const CScript& script)
{
    const auto payload{ParseMessageScript(script, PROPOSAL_TAG)};
    // slot, version, title length, hash_id1, hash_id2
    if (!payload || payload->size() < 1 + 1 + 1 + 32 + 20) return std::nullopt;
    const std::span<const unsigned char> bytes{*payload};
    Sidechain sidechain;
    sidechain.slot = bytes[0];
    sidechain.version = bytes[1];
    // Only version 0 is defined; the rest of a proposal of another version is not ours to read.
    if (sidechain.version != 0) return std::nullopt;
    const size_t title_size{bytes[2]};
    if (bytes.size() < 3 + title_size + 32 + 20) return std::nullopt;
    const auto title{bytes.subspan(3, title_size)};
    const auto description{bytes.subspan(3 + title_size, bytes.size() - 3 - title_size - 32 - 20)};
    sidechain.title.assign(title.begin(), title.end());
    sidechain.description.assign(description.begin(), description.end());
    sidechain.hash_id1 = uint256{bytes.subspan(bytes.size() - 52, 32)};
    sidechain.hash_id2 = uint160{bytes.subspan(bytes.size() - 20, 20)};
    return sidechain;
}

CScript AckScript(SidechainId slot, const uint256& proposal_hash)
{
    return MessageScript(ACK_TAG, SlotAndHash(slot, proposal_hash));
}

std::optional<std::pair<SidechainId, uint256>> ParseAckScript(const CScript& script)
{
    const auto payload{ParseMessageScript(script, ACK_TAG)};
    if (!payload) return std::nullopt;
    return ParseSlotAndHash(*payload);
}

CScript BundleScript(SidechainId slot, const uint256& bundle_hash)
{
    return MessageScript(BUNDLE_TAG, SlotAndHash(slot, bundle_hash));
}

std::optional<std::pair<SidechainId, uint256>> ParseBundleScript(const CScript& script)
{
    const auto payload{ParseMessageScript(script, BUNDLE_TAG)};
    if (!payload) return std::nullopt;
    return ParseSlotAndHash(*payload);
}

VoteMessage MakeVoteMessage(const std::vector<uint16_t>& votes)
{
    VoteMessage message;
    message.votes = votes;
    const bool two_bytes{std::any_of(votes.begin(), votes.end(), [](uint16_t v) { return v > VOTE_MAX_ONE_BYTE_INDEX && v != VOTE_ABSTAIN && v != VOTE_DOWNVOTE; })};
    message.form = two_bytes ? VoteForm::TWO_BYTES : VoteForm::ONE_BYTE;
    return message;
}

CScript VoteScript(const VoteMessage& message)
{
    std::vector<unsigned char> payload{static_cast<unsigned char>(message.form)};
    for (const uint16_t vote : message.votes) {
        if (message.form == VoteForm::ONE_BYTE) {
            payload.push_back(vote == VOTE_ABSTAIN ? VOTE_ABSTAIN_ONE_BYTE : vote == VOTE_DOWNVOTE ? VOTE_DOWNVOTE_ONE_BYTE : static_cast<unsigned char>(vote));
        } else if (message.form == VoteForm::TWO_BYTES) {
            payload.push_back(vote & 0xff);
            payload.push_back(vote >> 8);
        }
    }
    return MessageScript(VOTE_TAG, payload);
}

std::optional<VoteMessage> ParseVoteScript(const CScript& script)
{
    const auto payload{ParseMessageScript(script, VOTE_TAG)};
    if (!payload || payload->empty()) return std::nullopt;
    VoteMessage message;
    switch ((*payload)[0]) {
    case static_cast<uint8_t>(VoteForm::REPEAT_PREVIOUS):
    case static_cast<uint8_t>(VoteForm::LEADING_BY_50):
        if (payload->size() != 1) return std::nullopt;
        message.form = static_cast<VoteForm>((*payload)[0]);
        return message;
    case static_cast<uint8_t>(VoteForm::ONE_BYTE):
        message.form = VoteForm::ONE_BYTE;
        for (size_t i{1}; i < payload->size(); ++i) {
            const uint8_t vote{(*payload)[i]};
            message.votes.push_back(vote == VOTE_ABSTAIN_ONE_BYTE ? VOTE_ABSTAIN : vote == VOTE_DOWNVOTE_ONE_BYTE ? VOTE_DOWNVOTE : vote);
        }
        return message;
    case static_cast<uint8_t>(VoteForm::TWO_BYTES):
        if ((payload->size() - 1) % 2 != 0) return std::nullopt;
        message.form = VoteForm::TWO_BYTES;
        for (size_t i{1}; i < payload->size(); i += 2) {
            message.votes.push_back(ReadLE16(payload->data() + i));
        }
        return message;
    }
    return std::nullopt;
}

CScript BmmAcceptScript(SidechainId slot, const uint256& side_block_hash)
{
    return MessageScript(BMM_ACCEPT_TAG, SlotAndHash(slot, side_block_hash));
}

std::optional<std::pair<SidechainId, uint256>> ParseBmmAcceptScript(const CScript& script)
{
    const auto payload{ParseMessageScript(script, BMM_ACCEPT_TAG)};
    if (!payload) return std::nullopt;
    return ParseSlotAndHash(*payload);
}

CScript BmmRequestScript(const BmmRequest& request)
{
    std::vector<unsigned char> payload{SlotAndHash(request.slot, request.side_block_hash)};
    payload.insert(payload.end(), request.prev_main_block_hash.begin(), request.prev_main_block_hash.end());
    return MessageScript(BMM_REQUEST_TAG, payload);
}

std::optional<BmmRequest> ParseBmmRequestScript(const CScript& script)
{
    // Exactly OP_RETURN and a direct push of the 68 bytes.
    if (script.size() != 2 + BMM_REQUEST_DATA_SIZE || script[1] != BMM_REQUEST_DATA_SIZE) return std::nullopt;
    const auto payload{ParseMessageScript(script, BMM_REQUEST_TAG)};
    if (!payload || payload->size() != 1 + 32 + 32) return std::nullopt;
    const std::span<const unsigned char> bytes{*payload};
    BmmRequest request;
    request.slot = bytes[0];
    request.side_block_hash = uint256{bytes.subspan(1, 32)};
    request.prev_main_block_hash = uint256{bytes.subspan(33, 32)};
    return request;
}

CScript DestinationScript(const std::string& destination)
{
    return CScript() << OP_RETURN << std::vector<unsigned char>(destination.begin(), destination.end());
}

std::optional<std::string> ParseDestinationScript(const CScript& script)
{
    const auto data{ParsePushScript(script)};
    if (!data) return std::nullopt;
    return std::string{data->begin(), data->end()};
}

CScript WithdrawalFeeScript(CAmount fee)
{
    std::vector<unsigned char> amount(8);
    WriteBE64(amount.data(), fee);
    return CScript() << OP_RETURN << amount;
}

std::optional<CAmount> ParseWithdrawalFeeScript(const CScript& script)
{
    // Exactly OP_RETURN OP_PUSHBYTES_8 <fee>: the M6 id is computed with this encoding.
    if (script.size() != 10 || script[1] != 8) return std::nullopt;
    const auto data{ParsePushScript(script)};
    if (!data || data->size() != 8) return std::nullopt;
    const CAmount fee{static_cast<CAmount>(ReadBE64(data->data()))};
    if (!MoneyRange(fee)) return std::nullopt;
    return fee;
}

bool IsBlindWithdrawal(const CTransaction& tx)
{
    if (!tx.vin.empty() || tx.vout.size() < 2) return false;
    if (tx.vout[0].nValue != 0 || !ParseWithdrawalFeeScript(tx.vout[0].scriptPubKey)) return false;
    CAmount payout{0};
    for (const CTxOut& out : tx.vout) {
        if (!MoneyRange(out.nValue) || ParseEscrowScript(out.scriptPubKey)) return false;
        payout += out.nValue;
        if (!MoneyRange(payout)) return false;
    }
    return payout > 0;
}

std::optional<CMutableTransaction> BlindWithdrawalTx(const CTransaction& tx, CAmount treasury_amount)
{
    if (tx.vin.size() != 1 || tx.vout.empty() || !ParseEscrowScript(tx.vout[0].scriptPubKey)) return std::nullopt;
    CAmount spent{tx.vout[0].nValue};
    if (!MoneyRange(spent)) return std::nullopt;
    for (size_t i{1}; i < tx.vout.size(); ++i) {
        if (!MoneyRange(tx.vout[i].nValue)) return std::nullopt;
        spent += tx.vout[i].nValue;
        if (!MoneyRange(spent)) return std::nullopt;
    }
    if (spent > treasury_amount) return std::nullopt;
    CMutableTransaction blind{tx};
    blind.vin.clear();
    blind.vout[0] = CTxOut{0, WithdrawalFeeScript(treasury_amount - spent)};
    return blind;
}

std::optional<uint256> BlindWithdrawalHash(const CTransaction& tx, CAmount treasury_amount)
{
    const auto blind{BlindWithdrawalTx(tx, treasury_amount)};
    if (!blind) return std::nullopt;
    return blind->GetHash().ToUint256();
}

std::optional<CMutableTransaction> CompleteWithdrawal(const CTransaction& blind, SidechainId slot, const Ctip& ctip, CAmount* fee)
{
    if (!IsBlindWithdrawal(blind)) return std::nullopt;
    const CAmount miner_fee{*ParseWithdrawalFeeScript(blind.vout[0].scriptPubKey)};
    CAmount paid_out{0};
    for (const CTxOut& out : blind.vout) paid_out += out.nValue;
    if (paid_out + miner_fee > ctip.amount) return std::nullopt;
    // The treasury output is the only input, and the new one takes the place of the fee output.
    CMutableTransaction tx{blind};
    tx.vin.assign(1, CTxIn{ctip.outpoint});
    tx.vout[0] = CTxOut{ctip.amount - paid_out - miner_fee, EscrowScript(slot)};
    if (fee) *fee = miner_fee;
    return tx;
}

std::optional<BmmRequest> GetBmmRequest(const CTransaction& tx)
{
    if (tx.vout.empty()) return std::nullopt;
    return ParseBmmRequestScript(tx.vout[0].scriptPubKey);
}

std::vector<BmmRequest> GetBmmRequests(const CTransaction& tx)
{
    std::vector<BmmRequest> requests;
    if (const auto request{GetBmmRequest(tx)}) requests.push_back(*request);
    return requests;
}

namespace {
std::string DepositChecksum(const std::string& prefix)
{
    unsigned char hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(reinterpret_cast<const unsigned char*>(prefix.data()), prefix.size()).Finalize(hash);
    return HexStr(hash).substr(0, 6);
}
} // namespace

std::string FormatDepositAddress(const DepositAddress& deposit_address)
{
    const std::string prefix{strprintf("s%u_%s_", deposit_address.slot, deposit_address.address)};
    return prefix + DepositChecksum(prefix);
}

DepositAddressKind ParseDepositAddress(const std::string& text, DepositAddress& deposit_address)
{
    // s, digits, an underscore, anything, an underscore, six characters.
    const size_t first{text.find('_')};
    const size_t last{text.rfind('_')};
    if (text.size() < 10 || text[0] != 's' || first == std::string::npos || first < 2 || last <= first + 1 || last + 7 != text.size()) return DepositAddressKind::PLAIN;
    const std::string digits{text.substr(1, first - 1)};
    if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) return DepositAddressKind::PLAIN;
    const auto slot{ToIntegral<SidechainId>(digits)};
    if (!slot || text.substr(last + 1) != DepositChecksum(text.substr(0, last + 1))) return DepositAddressKind::INVALID;
    deposit_address.slot = *slot;
    deposit_address.address = text.substr(first + 1, last - first - 1);
    return DepositAddressKind::VALID;
}

} // namespace drivechain
