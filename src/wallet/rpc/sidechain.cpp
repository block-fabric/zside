// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <core_io.h>
#include <drivechain/sidechain.h>
#include <key_io.h>
#include <rpc/util.h>
#include <sidechain/state.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>

namespace wallet {
namespace {

/** Make, sign and send a transaction with a single output of `value` and the script `script`. */
CTransactionRef SendScript(CWallet& wallet, const CScript& script, CAmount value)
{
    std::vector<CRecipient> recipients{{CNoDestination{script}, value, /*fSubtractFeeFromAmount=*/false}};
    CCoinControl coin_control;
    auto res{CreateTransaction(wallet, recipients, /*change_pos=*/std::nullopt, coin_control, /*sign=*/true)};
    if (!res) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, util::ErrorString(res).original);
    wallet.CommitTransaction(res->tx, /*replaces_txid=*/std::nullopt, /*comment=*/std::nullopt, /*comment_to=*/std::nullopt);
    return res->tx;
}

} // namespace

RPCMethod getdepositaddress()
{
    return RPCMethod{
        "getdepositaddress",
        "Returns a deposit address: what to give the wallet of the mainchain as the destination of a deposit to this\n"
        "sidechain. It names this sidechain and ends in a checksum, so that the mainchain wallet refuses it if it is\n"
        "mistyped or used for another sidechain.",
        {
            {"address", RPCArg::Type::STR, RPCArg::DefaultHint{"a new address of this wallet"}, "The address of this chain to credit"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR, "depositaddress", "The deposit address"},
            {RPCResult::Type::STR, "address", "The address of this chain that deposits to it are credited to"},
            {RPCResult::Type::NUM, "slot", "The slot of this sidechain on the mainchain"},
        }},
        RPCExamples{HelpExampleCli("getdepositaddress", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    const auto slot{pwallet->chain().getSidechainSlot()};
    if (!slot) throw JSONRPCError(RPC_MISC_ERROR, "This chain is not running as a sidechain");
    std::string address;
    if (request.params[0].isNull()) {
        const auto dest{pwallet->GetNewDestination(OutputType::BECH32, "deposit")};
        if (!dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(dest).original);
        address = EncodeDestination(*dest);
    } else {
        address = request.params[0].get_str();
        if (!IsValidDestinationString(address)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Not an address of this chain");
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("depositaddress", drivechain::FormatDepositAddress({*slot, address}));
    result.pushKV("address", address);
    result.pushKV("slot", *slot);
    return result;
},
    };
}

RPCMethod createwithdrawal()
{
    return RPCMethod{
        "createwithdrawal",
        "Withdraw coins to the mainchain. The coins leave this wallet at once. They are paid on the mainchain once the\n"
        "withdrawal is part of a withdrawal bundle that the miners of the mainchain have voted through, which takes long.\n"
        "Until the withdrawal is in a bundle it can be taken back with refundwithdrawal." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"mainchainaddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The address on the mainchain to pay"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in " + CURRENCY_UNIT + " to pay on the mainchain"},
            {"mainchainfee", RPCArg::Type::AMOUNT, RPCArg::Default{"0.0001"}, "The fee in " + CURRENCY_UNIT + " offered to the miners of the mainchain, on top of the amount. Withdrawals that offer more are bundled first."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The id of the transaction"},
            {RPCResult::Type::NUM, "vout", "The output that burned the coins; with the txid it identifies the withdrawal"},
            {RPCResult::Type::STR, "refundaddress", "The address of this wallet that a refund of the withdrawal would pay"},
        }},
        RPCExamples{HelpExampleCli("createwithdrawal", "\"mainchainaddress\" 1.5")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    pwallet->BlockUntilSyncedToCurrentChain();

    const CAmount amount{AmountFromValue(request.params[1])};
    const CAmount main_fee{request.params[2].isNull() ? 10000 : AmountFromValue(request.params[2])};
    if (amount <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "The amount must be positive");

    // Only the mainchain node knows what an address of the mainchain stands for.
    const auto main_script{pwallet->chain().getMainchainScript(request.params[0].get_str())};
    if (!main_script) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, util::ErrorString(main_script).original);
    if (main_script->size() > sidechain::MAX_MAIN_SCRIPT_SIZE) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "The script of this mainchain address is too long for a withdrawal");

    EnsureWalletIsUnlocked(*pwallet);

    // A refund is paid to a key of this wallet, which has to sign the request for it.
    const auto refund_dest{pwallet->GetNewDestination(OutputType::BECH32, "withdrawal refund")};
    if (!refund_dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(refund_dest).original);
    const WitnessV0KeyHash* keyhash{std::get_if<WitnessV0KeyHash>(&*refund_dest)};
    if (!keyhash) throw JSONRPCError(RPC_WALLET_ERROR, "The wallet cannot make a P2WPKH address for the refund");

    const CScript script{sidechain::WithdrawalScript(main_fee, uint160{*keyhash}, *main_script)};
    const CTransactionRef tx{SendScript(*pwallet, script, amount + main_fee)};
    if (!pwallet->chain().isInMempool(tx->GetHash())) {
        pwallet->AbandonTransaction(tx->GetHash());
        throw JSONRPCError(RPC_WALLET_ERROR, "The withdrawal was not accepted into the mempool; the amount may be below the minimum");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    for (uint32_t n{0}; n < tx->vout.size(); ++n) {
        if (tx->vout[n].scriptPubKey == script) result.pushKV("vout", n);
    }
    result.pushKV("refundaddress", EncodeDestination(*refund_dest));
    return result;
},
    };
}

RPCMethod refundwithdrawal()
{
    return RPCMethod{
        "refundwithdrawal",
        "Take back a withdrawal that this wallet made, as long as it is not in a withdrawal bundle. The block that\n"
        "includes the request pays the coins, the fee for the mainchain included, to the refund address of the withdrawal." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The id of the transaction that made the withdrawal"},
            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output of that transaction that burned the coins"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The id of the transaction that asks for the refund"},
            {RPCResult::Type::STR_AMOUNT, "amount", "The amount that will be refunded"},
            {RPCResult::Type::STR, "refundaddress", "The address that will be paid"},
        }},
        RPCExamples{HelpExampleCli("refundwithdrawal", "\"txid\" 0")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    pwallet->BlockUntilSyncedToCurrentChain();

    const COutPoint outpoint{Txid::FromUint256(ParseHashV(request.params[0], "txid")), request.params[1].getInt<uint32_t>()};
    std::optional<sidechain::Withdrawal> withdrawal;
    {
        LOCK(pwallet->cs_wallet);
        const CWalletTx* wtx{pwallet->GetWalletTx(outpoint.hash)};
        if (!wtx || outpoint.n >= wtx->GetTx()->vout.size()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "This wallet has no such transaction output");
        withdrawal = sidechain::ParseWithdrawalOutput(wtx->GetTx()->vout[outpoint.n]);
    }
    if (!withdrawal) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "This transaction output is not a withdrawal");

    EnsureWalletIsUnlocked(*pwallet);

    std::string signature_base64;
    const SigningResult signing{pwallet->SignMessage(sidechain::RefundMessage(outpoint), PKHash{withdrawal->refund_keyhash}, signature_base64)};
    if (signing != SigningResult::OK) throw JSONRPCError(RPC_WALLET_ERROR, "This wallet does not have the refund key of the withdrawal: " + SigningResultString(signing));
    sidechain::RefundRequest refund;
    refund.withdrawal = outpoint;
    refund.signature = *Assert(DecodeBase64(signature_base64));

    const CTransactionRef tx{SendScript(*pwallet, sidechain::RefundScript(refund), 0)};
    if (!pwallet->chain().isInMempool(tx->GetHash())) {
        pwallet->AbandonTransaction(tx->GetHash());
        throw JSONRPCError(RPC_WALLET_ERROR, "The refund was not accepted into the mempool: the withdrawal is not mined yet, is in a bundle, was paid, or was already refunded");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("amount", ValueFromAmount(withdrawal->Burned()));
    result.pushKV("refundaddress", EncodeDestination(WitnessV0KeyHash{withdrawal->refund_keyhash}));
    return result;
},
    };
}

} // namespace wallet
