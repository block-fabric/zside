// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/state.h>

#include <addresstype.h>
#include <crypto/common.h>
#include <drivechain/sidechain.h>
#include <key_io.h>
#include <pubkey.h>
#include <script/solver.h>
#include <tinyformat.h>

#include <algorithm>

// The script formats of the sidechain rules. They are apart from the rules
// themselves because wallets need them too.

namespace sidechain {
namespace {

/** If `script` is OP_RETURN followed by a single push that starts with `tag`, return what follows the tag. */
std::optional<std::vector<unsigned char>> ParseTagged(const CScript& script, std::span<const unsigned char> tag)
{
    CScript::const_iterator pc{script.begin()};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode) || opcode != OP_RETURN) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode > OP_PUSHDATA4 || pc != script.end()) return std::nullopt;
    if (data.size() < tag.size() || !std::equal(tag.begin(), tag.end(), data.begin())) return std::nullopt;
    data.erase(data.begin(), data.begin() + tag.size());
    return data;
}

CScript Tagged(std::span<const unsigned char> tag, std::span<const unsigned char> payload)
{
    std::vector<unsigned char> data{tag.begin(), tag.end()};
    data.insert(data.end(), payload.begin(), payload.end());
    return CScript() << OP_RETURN << data;
}

} // namespace

CScript WithdrawalScript(CAmount main_fee, const uint160& refund_keyhash, const CScript& main_script)
{
    std::vector<unsigned char> payload(8);
    WriteLE64(payload.data(), static_cast<uint64_t>(main_fee));
    payload.insert(payload.end(), refund_keyhash.begin(), refund_keyhash.end());
    payload.insert(payload.end(), main_script.begin(), main_script.end());
    return Tagged(WITHDRAWAL_TAG, payload);
}

bool IsWithdrawalScript(const CScript& script)
{
    return ParseTagged(script, WITHDRAWAL_TAG).has_value();
}

std::optional<Withdrawal> ParseWithdrawalOutput(const CTxOut& out)
{
    const auto payload{ParseTagged(out.scriptPubKey, WITHDRAWAL_TAG)};
    if (!payload || payload->size() <= 28 || payload->size() > 28 + MAX_MAIN_SCRIPT_SIZE) return std::nullopt;
    Withdrawal withdrawal;
    const uint64_t fee{ReadLE64(payload->data())};
    if (fee > static_cast<uint64_t>(MAX_MONEY) || !MoneyRange(out.nValue) || static_cast<CAmount>(fee) >= out.nValue) return std::nullopt;
    withdrawal.main_fee = static_cast<CAmount>(fee);
    withdrawal.amount = out.nValue - withdrawal.main_fee;
    std::copy(payload->begin() + 8, payload->begin() + 28, withdrawal.refund_keyhash.begin());
    withdrawal.main_script = CScript{payload->begin() + 28, payload->end()};
    // Paying an output that cannot be spent would only take the coins out of the escrow for nobody.
    if (withdrawal.main_script.IsUnspendable()) return std::nullopt;
    // Only output types every mainchain node relays: a bundle that pays anything else -- a sidechain
    // treasury, bare multisig, a script of one's own -- cannot be broadcast, and would hold up
    // every withdrawal behind it.
    std::vector<std::vector<unsigned char>> solutions;
    switch (Solver(withdrawal.main_script, solutions)) {
    case TxoutType::PUBKEYHASH:
    case TxoutType::SCRIPTHASH:
    case TxoutType::WITNESS_V0_KEYHASH:
    case TxoutType::WITNESS_V0_SCRIPTHASH:
    case TxoutType::WITNESS_V1_TAPROOT:
        break;
    default:
        return std::nullopt;
    }
    return withdrawal;
}

CScript RefundScript(const RefundRequest& request)
{
    const uint256& txid{request.withdrawal.hash.ToUint256()};
    std::vector<unsigned char> payload{txid.begin(), txid.end()};
    payload.resize(36);
    WriteLE32(payload.data() + 32, request.withdrawal.n);
    payload.insert(payload.end(), request.signature.begin(), request.signature.end());
    return Tagged(REFUND_TAG, payload);
}

std::optional<RefundRequest> ParseRefundScript(const CScript& script)
{
    const auto payload{ParseTagged(script, REFUND_TAG)};
    if (!payload || payload->size() != 36 + CPubKey::COMPACT_SIGNATURE_SIZE) return std::nullopt;
    RefundRequest request;
    request.withdrawal = COutPoint{Txid::FromUint256(uint256{std::span{*payload}.first(32)}), ReadLE32(payload->data() + 32)};
    request.signature.assign(payload->begin() + 36, payload->end());
    return request;
}

std::string RefundMessage(const COutPoint& withdrawal)
{
    return strprintf("Refund withdrawal %s:%u", withdrawal.hash.GetHex(), withdrawal.n);
}

CScript BundleCommitScript(const uint256& hash)
{
    return Tagged(BUNDLE_COMMIT_TAG, hash);
}

std::optional<uint256> ParseBundleCommitScript(const CScript& script)
{
    const auto payload{ParseTagged(script, BUNDLE_COMMIT_TAG)};
    if (!payload || payload->size() != 32) return std::nullopt;
    return uint256{std::span{*payload}};
}

CScript DepositScript(const std::string& destination, uint32_t slot)
{
    std::string address{destination};
    drivechain::DepositAddress deposit_address;
    switch (drivechain::ParseDepositAddress(destination, deposit_address)) {
    case drivechain::DepositAddressKind::PLAIN: break;
    case drivechain::DepositAddressKind::INVALID: return CScript() << OP_RETURN;
    case drivechain::DepositAddressKind::VALID:
        if (deposit_address.slot != slot) return CScript() << OP_RETURN;
        address = deposit_address.address;
        break;
    }
    const CTxDestination dest{DecodeDestination(address)};
    if (!IsValidDestination(dest)) return CScript() << OP_RETURN;
    return GetScriptForDestination(dest);
}

} // namespace sidechain
