// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <drivechain/db.h>
#include <drivechain/miner.h>
#include <drivechain/scdb.h>
#include <drivechain/sidechain.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <node/transaction.h>
#include <node/types.h>
#include <common/messages.h>
#include <primitives/block.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <sync.h>
#include <txmempool.h>
#include <univalue.h>
#include <validation.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using drivechain::Bundle;
using drivechain::Proposal;
using drivechain::Sidechain;
using drivechain::SidechainDB;
using drivechain::SidechainId;
using drivechain::Slot;
using drivechain::Vote;
using node::NodeContext;

namespace {

SidechainId ParseSlot(const UniValue& value, const ChainstateManager& chainman)
{
    const int64_t slot{value.getInt<int64_t>()};
    if (slot < 0 || slot >= chainman.GetConsensus().drivechain.max_sidechains) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Sidechain slot out of range (0 to %u)", chainman.GetConsensus().drivechain.max_sidechains - 1));
    }
    return static_cast<SidechainId>(slot);
}

UniValue SidechainToJSON(const Sidechain& sidechain)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("slot", sidechain.slot);
    obj.pushKV("title", sidechain.title);
    obj.pushKV("description", sidechain.description);
    obj.pushKV("version", sidechain.version);
    obj.pushKV("hashid1", sidechain.hash_id1.GetHex());
    obj.pushKV("hashid2", sidechain.hash_id2.GetHex());
    obj.pushKV("proposalhash", sidechain.GetHash().GetHex());
    return obj;
}

std::vector<RPCResult> SidechainResults()
{
    return {
        {RPCResult::Type::NUM, "slot", "The sidechain slot number"},
        {RPCResult::Type::STR, "title", "The title of the sidechain"},
        {RPCResult::Type::STR, "description", "The description of the sidechain"},
        {RPCResult::Type::NUM, "version", "The version of the sidechain description"},
        {RPCResult::Type::STR_HEX, "hashid1", "Hash of the release tarball of the sidechain software"},
        {RPCResult::Type::STR_HEX, "hashid2", "Hash of the commit the sidechain software was built from"},
        {RPCResult::Type::STR_HEX, "proposalhash", "The hash miners commit to in order to ack the sidechain"},
    };
}

UniValue SlotToJSON(const Slot& slot)
{
    UniValue obj{SidechainToJSON(slot.sidechain)};
    obj.pushKV("activationheight", slot.activation_height);
    obj.pushKV("escrowscript", HexStr(drivechain::EscrowScript(slot.sidechain.slot)));
    if (slot.has_ctip) {
        UniValue ctip(UniValue::VOBJ);
        ctip.pushKV("txid", slot.ctip.outpoint.hash.GetHex());
        ctip.pushKV("n", slot.ctip.outpoint.n);
        ctip.pushKV("amount", ValueFromAmount(slot.ctip.amount));
        obj.pushKV("escrow", std::move(ctip));
    }
    obj.pushKV("pendingbundles", slot.bundles.size());
    return obj;
}

std::vector<RPCResult> SlotResults()
{
    std::vector<RPCResult> results{SidechainResults()};
    results.emplace_back(RPCResult::Type::NUM, "activationheight", "Height of the block that activated the sidechain");
    results.emplace_back(RPCResult::Type::STR_HEX, "escrowscript", "Script of the escrow output of the sidechain");
    results.emplace_back(RPCResult::Type::OBJ, "escrow", /*optional=*/true, "The escrow output holding the coins deposited to the sidechain; absent before the first deposit",
        std::vector<RPCResult>{
            {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
            {RPCResult::Type::NUM, "n", "The output index"},
            {RPCResult::Type::STR_AMOUNT, "amount", "The amount held in escrow"},
        });
    results.emplace_back(RPCResult::Type::NUM, "pendingbundles", "Number of withdrawal bundles being voted on");
    return results;
}

RPCMethod getdrivechaininfo()
{
    return RPCMethod{
        "getdrivechaininfo",
        "Returns the drivechain parameters of this chain and a summary of the sidechain database.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "maxsidechains", "Number of sidechain slots"},
            {RPCResult::Type::NUM, "activationperiod", "Blocks a proposal for an empty slot needs to activate"},
            {RPCResult::Type::NUM, "activationmaxfailures", "Blocks without an ack at which a proposal is rejected"},
            {RPCResult::Type::NUM, "replacementperiod", "Blocks a proposal for a slot in use needs to activate"},
            {RPCResult::Type::NUM, "withdrawalperiod", "Blocks a withdrawal bundle has to collect its work score"},
            {RPCResult::Type::NUM, "withdrawalminscore", "Work score a withdrawal bundle needs to be paid out"},
            {RPCResult::Type::NUM, "maxpendingbundles", "Maximum number of pending withdrawal bundles per sidechain"},
            {RPCResult::Type::NUM, "upvoteexpiryblocks", "Blocks in a row without an upvote after which a pending bundle fails (counted from its proposal or its last upvote)"},
            {RPCResult::Type::NUM, "upvoteexpiryheight", "Height from which that rule, and the forgetting of failed bundles a withdrawal period on, apply"},
            {RPCResult::Type::NUM, "height", "Height of the block the sidechain database belongs to"},
            {RPCResult::Type::NUM, "activesidechains", "Number of active sidechains"},
            {RPCResult::Type::NUM, "proposals", "Number of sidechain proposals collecting acks"},
            {RPCResult::Type::NUM, "pendingbundles", "Number of withdrawal bundles being voted on"},
            {RPCResult::Type::STR_AMOUNT, "escrowtotal", "Coins held in escrow by all sidechains"},
            {RPCResult::Type::STR_HEX, "statehash", "Hash committing to the whole sidechain database"},
            {RPCResult::Type::STR, "defaultwithdrawalvote", "How this node votes on bundles it has no specific vote for: upvote, follow, downvote or abstain (see setdefaultwithdrawalvote)"},
        }},
        RPCExamples{HelpExampleCli("getdrivechaininfo", "") + HelpExampleRpc("getdrivechaininfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const Consensus::DrivechainParams& params{chainman.GetConsensus().drivechain};
    // Read in place, under the lock: a copy would cost as much as the database is large.
    LOCK(::cs_main);
    const SidechainDB& scdb{chainman.ActiveChainstate().m_scdb};
    const int height{chainman.ActiveHeight()};

    size_t bundles{0};
    CAmount escrow{0};
    for (const auto& [id, slot] : scdb.GetSlots()) {
        bundles += slot.bundles.size();
        if (slot.has_ctip) escrow += slot.ctip.amount;
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("maxsidechains", params.max_sidechains);
    obj.pushKV("activationperiod", params.activation_period);
    obj.pushKV("activationmaxfailures", params.activation_max_failures);
    obj.pushKV("replacementperiod", params.replacement_period);
    obj.pushKV("withdrawalperiod", params.withdrawal_period);
    obj.pushKV("withdrawalminscore", params.withdrawal_min_score);
    obj.pushKV("maxpendingbundles", params.max_pending_bundles);
    obj.pushKV("upvoteexpiryblocks", params.upvote_expiry_blocks);
    obj.pushKV("upvoteexpiryheight", params.audit2_height);
    obj.pushKV("height", height);
    obj.pushKV("activesidechains", scdb.GetSlots().size());
    obj.pushKV("proposals", scdb.GetProposals().size());
    obj.pushKV("pendingbundles", bundles);
    obj.pushKV("escrowtotal", ValueFromAmount(escrow));
    obj.pushKV("statehash", scdb.GetHash().GetHex());
    const Vote::Type default_vote{chainman.m_drivechain_miner.GetDefaultVote()};
    obj.pushKV("defaultwithdrawalvote", default_vote == Vote::Type::DOWNVOTE ? "downvote" :
                                        default_vote == Vote::Type::ABSTAIN  ? "abstain" :
                                        chainman.m_drivechain_miner.GetFollow() ? "follow" : "upvote");
    return obj;
},
    };
}

RPCMethod listactivesidechains()
{
    return RPCMethod{
        "listactivesidechains",
        "List the active sidechains.",
        {},
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "", SlotResults()}}},
        RPCExamples{HelpExampleCli("listactivesidechains", "") + HelpExampleRpc("listactivesidechains", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    // Read in place, under the lock: a copy would cost as much as the database is large.
    LOCK(::cs_main);
    const SidechainDB& scdb{chainman.ActiveChainstate().m_scdb};
    UniValue result(UniValue::VARR);
    for (const auto& [id, slot] : scdb.GetSlots()) result.push_back(SlotToJSON(slot));
    return result;
},
    };
}

RPCMethod getsidechain()
{
    return RPCMethod{
        "getsidechain",
        "Returns the sidechain in a slot.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sidechain slot number"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", SlotResults()},
        RPCExamples{HelpExampleCli("getsidechain", "0") + HelpExampleRpc("getsidechain", "0")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const SidechainId id{ParseSlot(request.params[0], chainman)};
    // Read in place, under the lock: a copy would cost as much as the database is large.
    LOCK(::cs_main);
    const SidechainDB& scdb{chainman.ActiveChainstate().m_scdb};
    const Slot* slot{scdb.GetSlot(id)};
    if (!slot) throw JSONRPCError(RPC_INVALID_PARAMETER, "No active sidechain in this slot");
    return SlotToJSON(*slot);
},
    };
}

RPCMethod listsidechainproposals()
{
    return RPCMethod{
        "listsidechainproposals",
        "List the sidechain proposals that are collecting acks, and the proposals this node will make in the blocks it builds.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::ARR, "pending", "Proposals in the chain",
            {
                {RPCResult::Type::OBJ, "", "", Cat(SidechainResults(), std::vector<RPCResult>{
                    {RPCResult::Type::NUM, "height", "Height of the block that made the proposal"},
                    {RPCResult::Type::NUM, "age", "Number of blocks since, counting the proposing block"},
                    {RPCResult::Type::NUM, "acks", "Number of blocks that acked the proposal"},
                    {RPCResult::Type::NUM, "failures", "Number of blocks that did not ack the proposal"},
                    {RPCResult::Type::NUM, "blocksleft", "Number of blocks until the proposal activates if it is not rejected first"},
                    {RPCResult::Type::BOOL, "replacement", "Whether the slot already holds a sidechain, which the proposal would replace"},
                    {RPCResult::Type::BOOL, "ack", "Whether this node acks the proposal in the blocks it builds"},
                })},
            }},
            {RPCResult::Type::ARR, "queued", "Proposals this node will make that are not in the chain yet",
            {
                {RPCResult::Type::OBJ, "", "", SidechainResults()},
            }},
        }},
        RPCExamples{HelpExampleCli("listsidechainproposals", "") + HelpExampleRpc("listsidechainproposals", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const Consensus::DrivechainParams& params{chainman.GetConsensus().drivechain};
    // Read in place, under the lock: a copy would cost as much as the database is large.
    LOCK(::cs_main);
    const SidechainDB& scdb{chainman.ActiveChainstate().m_scdb};
    const int height{chainman.ActiveHeight()};

    UniValue pending(UniValue::VARR);
    for (const Proposal& proposal : scdb.GetProposals()) {
        const bool replacement{scdb.IsActive(proposal.sidechain.slot)};
        UniValue obj{SidechainToJSON(proposal.sidechain)};
        obj.pushKV("height", proposal.height);
        obj.pushKV("age", height - proposal.height + 1);
        obj.pushKV("acks", proposal.acks);
        obj.pushKV("failures", SidechainDB::Failures(proposal, height));
        obj.pushKV("blocksleft", SidechainDB::BlocksUntilActivation(proposal, replacement, height, params));
        obj.pushKV("replacement", replacement);
        obj.pushKV("ack", chainman.m_drivechain_miner.IsAcked(proposal.sidechain.slot, proposal.hash));
        pending.push_back(std::move(obj));
    }

    UniValue queued(UniValue::VARR);
    for (const Sidechain& sidechain : chainman.m_drivechain_miner.GetProposals()) {
        if (scdb.GetProposal(sidechain.slot, sidechain.GetHash())) continue;
        if (const Slot* slot{scdb.GetSlot(sidechain.slot)}; slot && slot->sidechain == sidechain) continue;
        queued.push_back(SidechainToJSON(sidechain));
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("pending", std::move(pending));
    result.pushKV("queued", std::move(queued));
    return result;
},
    };
}

RPCMethod createsidechainproposal()
{
    return RPCMethod{
        "createsidechainproposal",
        "Propose a sidechain in the next block this node builds, and ack it in the blocks after.\n"
        "A proposal for a slot that already holds a sidechain replaces it if it activates.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sidechain slot number"},
            {"title", RPCArg::Type::STR, RPCArg::Optional::NO, "The title of the sidechain"},
            {"description", RPCArg::Type::STR, RPCArg::Default{""}, "The description of the sidechain"},
            {"hashid1", RPCArg::Type::STR_HEX, RPCArg::Default{"0000000000000000000000000000000000000000000000000000000000000000"}, "Hash of the release tarball of the sidechain software (256 bits)"},
            {"hashid2", RPCArg::Type::STR_HEX, RPCArg::Default{"0000000000000000000000000000000000000000"}, "Hash of the commit the sidechain software was built from (160 bits)"},
            {"version", RPCArg::Type::NUM, RPCArg::Default{0}, "The version of the sidechain description"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", SidechainResults()},
        RPCExamples{HelpExampleCli("createsidechainproposal", "0 \"Testchain\" \"A sidechain for testing\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    Sidechain sidechain;
    sidechain.slot = ParseSlot(request.params[0], chainman);
    sidechain.title = request.params[1].get_str();
    if (!request.params[2].isNull()) sidechain.description = request.params[2].get_str();
    if (!request.params[3].isNull()) sidechain.hash_id1 = ParseHashV(request.params[3], "hashid1");
    if (!request.params[4].isNull()) {
        const auto hash{uint160::FromHex(request.params[4].get_str())};
        if (!hash) throw JSONRPCError(RPC_INVALID_PARAMETER, "hashid2 must be 40 hexadecimal characters");
        sidechain.hash_id2 = *hash;
    }
    if (!request.params[5].isNull()) sidechain.version = request.params[5].getInt<int32_t>();
    if (!sidechain.IsValid(chainman.GetConsensus().drivechain.max_sidechains)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid proposal: the title must have 1 to %u bytes, the description at most %u, and the version must be at most %d",
                                                             drivechain::MAX_TITLE_SIZE, drivechain::MAX_DESCRIPTION_SIZE, drivechain::SIDECHAIN_VERSION_MAX));
    }
    chainman.m_drivechain_miner.AddProposal(sidechain);
    return SidechainToJSON(sidechain);
},
    };
}

RPCMethod removesidechainproposal()
{
    return RPCMethod{
        "removesidechainproposal",
        "Stop proposing a sidechain in the blocks this node builds, and stop acking it.\n"
        "A proposal that is already in the chain stays there until it activates or is rejected.",
        {
            {"proposalhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash of the proposal"},
        },
        RPCResult{RPCResult::Type::BOOL, "", "Whether this node was proposing the sidechain"},
        RPCExamples{HelpExampleCli("removesidechainproposal", "\"proposalhash\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const uint256 hash{ParseHashV(request.params[0], "proposalhash")};
    chainman.m_drivechain_miner.ClearAcks(hash);
    return chainman.m_drivechain_miner.RemoveProposal(hash);
},
    };
}

RPCMethod acksidechain()
{
    return RPCMethod{
        "acksidechain",
        "Ack, or stop acking, a sidechain proposal in the blocks this node builds.\n"
        "An ack is for the proposal in one slot: the same sidechain proposed in another slot later is not acked by it.\n"
        "Without a slot, the proposal is acked in the slots where it is pending now.\n"
        "A proposal of the sidechain a slot already has is never acked, since it would replace the sidechain\n"
        "with itself and fail its pending withdrawals; acks of a sidechain are dropped once it activates.",
        {
            {"proposalhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash of the proposal"},
            {"ack", RPCArg::Type::BOOL, RPCArg::Default{true}, "Whether to ack the proposal"},
            {"slot", RPCArg::Type::NUM, RPCArg::DefaultHint{"the slots where the proposal is pending"}, "The slot of the proposal"},
        },
        RPCResult{RPCResult::Type::ARR, "", "The slots the ack applies to", {{RPCResult::Type::NUM, "", "slot"}}},
        RPCExamples{HelpExampleCli("acksidechain", "\"proposalhash\"") + HelpExampleCli("acksidechain", "\"proposalhash\" true 2")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const uint256 hash{ParseHashV(request.params[0], "proposalhash")};
    const bool ack{request.params[1].isNull() || request.params[1].get_bool()};
    std::set<SidechainId> slots;
    if (!request.params[2].isNull()) {
        const int slot{request.params[2].getInt<int>()};
        if (slot < 0 || slot >= static_cast<int>(drivechain::MAX_SLOTS)) throw JSONRPCError(RPC_INVALID_PARAMETER, "slot out of range");
        slots.insert(static_cast<SidechainId>(slot));
    } else {
        LOCK(::cs_main);
        for (const Proposal& proposal : chainman.ActiveChainstate().m_scdb.GetProposals()) {
            if (proposal.hash == hash) slots.insert(proposal.sidechain.slot);
        }
        for (const Sidechain& sidechain : chainman.m_drivechain_miner.GetProposals()) {
            if (sidechain.GetHash() == hash) slots.insert(sidechain.slot);
        }
        if (slots.empty() && ack) throw JSONRPCError(RPC_INVALID_PARAMETER, "No proposal with this hash is pending: give the slot to ack it before it is proposed");
        if (!ack) {
            chainman.m_drivechain_miner.ClearAcks(hash);
            return UniValue{UniValue::VARR};
        }
    }
    UniValue result(UniValue::VARR);
    for (const SidechainId slot : slots) {
        chainman.m_drivechain_miner.SetAck(slot, hash, ack);
        result.push_back(slot);
    }
    return result;
},
    };
}

RPCMethod receivewithdrawalbundle()
{
    return RPCMethod{
        "receivewithdrawalbundle",
        "Hand a withdrawal bundle of a sidechain to this node, as the sidechain built it, in the blind form of BIP300:\n"
        "a transaction with no inputs whose output 0 is OP_RETURN and the fee (8 bytes, big endian), followed by the payouts.\n"
        "The node proposes the bundle in the next block it builds, and pays it out once it has the work score.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sidechain slot number"},
            {"hexstring", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The withdrawal bundle transaction"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "hash", "The hash miners vote on (the M6 id)"},
        }},
        RPCExamples{HelpExampleCli("receivewithdrawalbundle", "0 \"hexstring\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const SidechainId id{ParseSlot(request.params[0], chainman)};
    CMutableTransaction mtx;
    // A transaction without inputs only decodes without the witness flag.
    if (!DecodeHexTx(mtx, request.params[1].get_str(), /*try_no_witness=*/true, /*try_witness=*/false)) throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    if (!WITH_LOCK(::cs_main, return chainman.ActiveChainstate().m_scdb.IsActive(id))) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "No active sidechain in this slot");
    }
    std::string error;
    const auto hash{chainman.m_drivechain_miner.AddBundle(id, mtx, error)};
    if (!hash) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
    UniValue result(UniValue::VOBJ);
    result.pushKV("hash", hash->GetHex());
    return result;
},
    };
}

RPCMethod vouchwithdrawalbundle()
{
    return RPCMethod{
        "vouchwithdrawalbundle",
        "Tell this node which withdrawal bundle the sidechain in a slot has now, as its sidechain node sees its best chain:\n"
        "one handed with receivewithdrawalbundle, or none. With the default vote (upvote), the node then upvotes that bundle\n"
        "and only proposes that one; with none, it downvotes the pending bundles of the slot, which a reorg of the sidechain\n"
        "left behind or nobody made, so that they fail. Sidechain nodes call this on their mainchain node.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sidechain slot number"},
            {"hash", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "The hash of the bundle (the M6 id); none if left out"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{HelpExampleCli("vouchwithdrawalbundle", "0 \"hash\"") + HelpExampleCli("vouchwithdrawalbundle", "0")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const SidechainId id{ParseSlot(request.params[0], chainman)};
    std::optional<uint256> hash;
    if (!request.params[1].isNull()) hash = ParseHashV(request.params[1], "hash");
    if (!chainman.m_drivechain_miner.Vouch(id, hash)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "This node was not handed that bundle (receivewithdrawalbundle)");
    }
    return UniValue::VNULL;
},
    };
}

RPCMethod sendwithdrawalbundle()
{
    return RPCMethod{
        "sendwithdrawalbundle",
        "Broadcast the withdrawal that pays out a bundle this node was handed (see receivewithdrawalbundle), once the bundle\n"
        "has the work score. The withdrawal goes to the mempool and to peers like any transaction, so whichever miner finds\n"
        "the next block pays the bundle out and collects its fee: miners need not run the sidechain (BIP300 M6).\n"
        "Sidechain nodes call this on their mainchain node.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sidechain slot number"},
            {"hash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash of the bundle (the M6 id)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::BOOL, "sent", "Whether the withdrawal is in the mempool now"},
            {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "The withdrawal transaction"},
            {RPCResult::Type::STR, "reason", /*optional=*/true, "Why it was not sent"},
        }},
        RPCExamples{HelpExampleCli("sendwithdrawalbundle", "0 \"hash\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    node::NodeContext& node{EnsureAnyNodeContext(request.context)};
    ChainstateManager& chainman{EnsureChainman(node)};
    CTxMemPool& mempool{EnsureMemPool(node)};
    const SidechainId id{ParseSlot(request.params[0], chainman)};
    const uint256 hash{ParseHashV(request.params[1], "hash")};
    UniValue result(UniValue::VOBJ);
    const auto not_sent = [&](const std::string& reason) {
        result.pushKV("sent", false);
        result.pushKV("reason", reason);
        return result;
    };

    const auto blind{chainman.m_drivechain_miner.GetBundle(id, hash)};
    if (!blind) return not_sent("this node was not handed the bundle (receivewithdrawalbundle)");
    CTransactionRef tx;
    {
        LOCK2(::cs_main, mempool.cs);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        const Slot* slot{chainstate.m_scdb.GetSlot(id)};
        if (!slot) return not_sent("no active sidechain in this slot");
        const auto bundle{std::find_if(slot->bundles.begin(), slot->bundles.end(), [&](const Bundle& b) { return b.hash == hash; })};
        if (bundle == slot->bundles.end()) return not_sent("the bundle is not pending");
        if (bundle->score < static_cast<uint32_t>(chainman.GetConsensus().drivechain.withdrawal_min_score)) return not_sent("the bundle does not have the work score yet");
        // The treasury output as the deposits in the mempool leave it.
        const drivechain::SidechainDB after{chainstate.GetMempoolSidechainDB({id})};
        const Slot* current{after.GetSlot(id)};
        if (!current || std::none_of(current->bundles.begin(), current->bundles.end(), [&](const Bundle& b) { return b.hash == hash; })) {
            return not_sent("the withdrawal is in the mempool already");
        }
        if (!current->has_ctip) return not_sent("the sidechain has no coins in its treasury");
        auto mtx{drivechain::CompleteWithdrawal(CTransaction{*blind}, id, current->ctip)};
        if (!mtx) return not_sent("the treasury cannot pay the bundle");
        tx = MakeTransactionRef(std::move(*mtx));
    }
    std::string error;
    const auto err{node::BroadcastTransaction(node, tx, error, /*max_tx_fee=*/0, node::TxBroadcast::MEMPOOL_AND_BROADCAST_TO_ALL, /*wait_callback=*/true)};
    if (err != node::TransactionError::OK) return not_sent(error.empty() ? common::TransactionErrorString(err).original : error);
    result.pushKV("sent", true);
    result.pushKV("txid", tx->GetHash().GetHex());
    return result;
},
    };
}

/** What a bundle this node knows pays: its payouts and the fee for miners (left out when it is not known). */
static void PushBundlePayouts(UniValue& obj, const std::optional<CMutableTransaction>& blind)
{
    if (!blind || blind->vout.empty()) return;
    // The blind form: the fee output first, then the payouts (and outputs of nothing, such as its nonce).
    UniValue payouts(UniValue::VARR);
    for (size_t i{1}; i < blind->vout.size(); ++i) {
        if (blind->vout[i].nValue == 0) continue;
        UniValue payout(UniValue::VOBJ);
        payout.pushKV("amount", ValueFromAmount(blind->vout[i].nValue));
        payout.pushKV("script", HexStr(blind->vout[i].scriptPubKey));
        payouts.push_back(std::move(payout));
    }
    obj.pushKV("payouts", std::move(payouts));
    if (const auto fee{drivechain::ParseWithdrawalFeeScript(blind->vout[0].scriptPubKey)}) obj.pushKV("fee", ValueFromAmount(*fee));
}

static std::vector<RPCResult> BundlePayoutResults()
{
    return {
        {RPCResult::Type::ARR, "payouts", /*optional=*/true, "What the bundle pays, if this node has its transaction (\"known\")", {
            {RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::STR_AMOUNT, "amount", "The amount paid"},
                {RPCResult::Type::STR_HEX, "script", "The output script paid"},
            }},
        }},
        {RPCResult::Type::STR_AMOUNT, "fee", /*optional=*/true, "The fee the bundle pays the miner who pays it out, if this node has its transaction"},
    };
}

RPCMethod listwithdrawalbundles()
{
    return RPCMethod{
        "listwithdrawalbundles",
        "List the withdrawal bundles that are being voted on.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Only list the bundles of this sidechain"},
        },
        RPCResult{RPCResult::Type::ARR, "", "",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "slot", "The sidechain slot number"},
                {RPCResult::Type::STR_HEX, "hash", "The hash of the bundle"},
                {RPCResult::Type::NUM, "index", "Position of the bundle among the bundles of its sidechain, as used in vote messages"},
                {RPCResult::Type::NUM, "height", "Height of the block that proposed the bundle"},
                {RPCResult::Type::NUM, "score", "The work score of the bundle"},
                {RPCResult::Type::NUM, "lastupvote", "Height of the last block that upvoted the bundle, or of the block that proposed it"},
                {RPCResult::Type::NUM, "blocksleft", "Number of blocks before the bundle fails if it is not paid out"},
                {RPCResult::Type::BOOL, "payable", "Whether the bundle has the score to be paid out"},
                {RPCResult::Type::BOOL, "known", "Whether this node has the transaction of the bundle, which it needs to pay it out"},
                {RPCResult::Type::STR, "vote", "How this node votes on the bundle in the blocks it builds: upvote, downvote or abstain"},
                BundlePayoutResults()[0],
                BundlePayoutResults()[1],
            }},
        }},
        RPCExamples{HelpExampleCli("listwithdrawalbundles", "") + HelpExampleCli("listwithdrawalbundles", "0")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const Consensus::DrivechainParams& params{chainman.GetConsensus().drivechain};
    std::optional<SidechainId> only;
    if (!request.params[0].isNull()) only = ParseSlot(request.params[0], chainman);
    // Read in place, under the lock: a copy would cost as much as the database is large.
    LOCK(::cs_main);
    const SidechainDB& scdb{chainman.ActiveChainstate().m_scdb};
    const int height{chainman.ActiveHeight()};

    UniValue result(UniValue::VARR);
    for (const auto& [id, slot] : scdb.GetSlots()) {
        if (only && *only != id) continue;
        const Vote vote{chainman.m_drivechain_miner.ResolveVote(id, slot.bundles)};
        for (size_t i{0}; i < slot.bundles.size(); ++i) {
            const Bundle& bundle{slot.bundles[i]};
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("slot", id);
            obj.pushKV("hash", bundle.hash.GetHex());
            obj.pushKV("index", i);
            obj.pushKV("height", bundle.height);
            obj.pushKV("score", bundle.score);
            obj.pushKV("lastupvote", bundle.last_upvote);
            obj.pushKV("blocksleft", SidechainDB::BlocksLeft(bundle, height, params));
            obj.pushKV("payable", bundle.score >= static_cast<uint32_t>(params.withdrawal_min_score));
            const auto blind{chainman.m_drivechain_miner.GetBundle(id, bundle.hash)};
            obj.pushKV("known", blind.has_value());
            std::string vote_str{"abstain"};
            if (vote.type == Vote::Type::DOWNVOTE) {
                vote_str = "downvote";
            } else if (vote.type == Vote::Type::UPVOTE) {
                // Upvoting one bundle of a sidechain lowers the score of its others.
                vote_str = vote.bundle == bundle.hash ? "upvote" : "downvote";
            }
            obj.pushKV("vote", vote_str);
            PushBundlePayouts(obj, blind);
            result.push_back(std::move(obj));
        }
    }
    return result;
},
    };
}

RPCMethod getwithdrawalbundle()
{
    return RPCMethod{
        "getwithdrawalbundle",
        "Returns what became of a withdrawal bundle of a sidechain. Sidechain software uses it to learn\n"
        "whether the withdrawals of a bundle were paid, or have to be refunded because the bundle failed.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sidechain slot number"},
            {"hash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash of the bundle that miners vote on"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR, "status", "\"pending\" while miners vote on the bundle, \"paid\" once it was paid out, \"failed\" if it did not get the votes in time\n"
                                          "(also once the sidechain database forgot it, a withdrawal period later: the blocks that closed it are kept for good),\n"
                                          "\"unknown\" if no block of the active chain proposed it. A bundle proposed again after it was forgotten is \"pending\" again"},
            {RPCResult::Type::NUM, "score", /*optional=*/true, "The work score of a pending bundle"},
            {RPCResult::Type::NUM, "lastupvote", /*optional=*/true, "Height of the last block that upvoted a pending bundle, or of the block that proposed it"},
            {RPCResult::Type::NUM, "blocksleft", /*optional=*/true, "Number of blocks a pending bundle has left to reach the minimum work score"},
            {RPCResult::Type::BOOL, "payable", /*optional=*/true, "Whether a pending bundle has the work score to be paid out"},
            BundlePayoutResults()[0],
            BundlePayoutResults()[1],
        }},
        RPCExamples{HelpExampleCli("getwithdrawalbundle", "0 \"hash\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const SidechainId id{ParseSlot(request.params[0], chainman)};
    const uint256 hash{ParseHashV(request.params[1], "hash")};
    // Read in place, under the lock: a copy would cost as much as the database is large.
    LOCK(::cs_main);
    const SidechainDB& scdb{chainman.ActiveChainstate().m_scdb};
    const int height{chainman.ActiveHeight()};
    const auto& params{chainman.GetConsensus().drivechain};

    UniValue result(UniValue::VOBJ);
    if (const auto paid{scdb.WasPaid(id, hash)}) {
        result.pushKV("status", *paid ? "paid" : "failed");
        return result;
    }
    if (const Slot* slot{scdb.GetSlot(id)}) {
        for (const Bundle& bundle : slot->bundles) {
            if (bundle.hash != hash) continue;
            result.pushKV("status", "pending");
            result.pushKV("score", bundle.score);
            result.pushKV("lastupvote", bundle.last_upvote);
            result.pushKV("blocksleft", SidechainDB::BlocksLeft(bundle, height, params));
            result.pushKV("payable", bundle.score >= static_cast<uint32_t>(params.withdrawal_min_score));
            PushBundlePayouts(result, chainman.m_drivechain_miner.GetBundle(id, hash));
            return result;
        }
    }
    // Forgotten by the sidechain database: the record of the block of the active chain that closed it.
    const auto closure{chainman.m_blockman.m_drivechain_db->FindClosure(id, hash, [&](const uint256& block_hash) {
        AssertLockHeld(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(block_hash)};
        return index && chainman.ActiveChain().Contains(*index);
    })};
    if (closure) {
        result.pushKV("status", closure->paid ? "paid" : "failed");
        return result;
    }
    result.pushKV("status", "unknown");
    return result;
},
    };
}

RPCMethod getaveragefee()
{
    return RPCMethod{
        "getaveragefee",
        "Returns the average fee that the transactions of recent blocks paid. Sidechain software uses it\n"
        "to decide what fee a withdrawal bundle should pay on this chain.",
        {
            {"blocks", RPCArg::Type::NUM, RPCArg::Default{6}, "Number of blocks to average over"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_AMOUNT, "feeaverage", "Average fee of a transaction, in " + CURRENCY_UNIT + "; zero if the blocks have no transactions"},
            {RPCResult::Type::NUM, "transactions", "Number of transactions the average is of, coinbases excluded"},
        }},
        RPCExamples{HelpExampleCli("getaveragefee", "6")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const int blocks{request.params[0].isNull() ? 6 : request.params[0].getInt<int>()};
    if (blocks < 1 || blocks > 1000) throw JSONRPCError(RPC_INVALID_PARAMETER, "The number of blocks must be between 1 and 1000");

    std::vector<const CBlockIndex*> indexes;
    {
        LOCK(::cs_main);
        for (const CBlockIndex* pindex{chainman.ActiveChain().Tip()}; pindex && pindex->nHeight > 0 && static_cast<int>(indexes.size()) < blocks; pindex = pindex->pprev) {
            indexes.push_back(pindex);
        }
    }
    CAmount fees{0};
    int64_t transactions{0};
    for (const CBlockIndex* pindex : indexes) {
        CBlock block;
        if (!chainman.m_blockman.ReadBlock(block, *pindex)) throw JSONRPCError(RPC_MISC_ERROR, "Block not available");
        // The inputs of the transactions are spent, so their fees are taken
        // from what the coinbase claims above the subsidy. A miner may claim
        // less than it could, which makes this a lower bound.
        fees += std::max<CAmount>(0, block.vtx[0]->GetValueOut() - GetBlockSubsidy(pindex->nHeight, chainman.GetConsensus()));
        transactions += block.vtx.size() - 1;
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("feeaverage", ValueFromAmount(transactions > 0 ? fees / transactions : 0));
    result.pushKV("transactions", transactions);
    return result;
},
    };
}

RPCMethod setwithdrawalvote()
{
    return RPCMethod{
        "setwithdrawalvote",
        "Set how this node votes on the withdrawal bundles of a sidechain in the blocks it builds.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sidechain slot number"},
            {"vote", RPCArg::Type::STR, RPCArg::Optional::NO, "\"upvote\" (requires the hash of a bundle), \"downvote\", \"abstain\", or \"default\" to follow the default vote"},
            {"hash", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "The hash of the bundle to upvote; the other bundles of the sidechain are downvoted"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{HelpExampleCli("setwithdrawalvote", "0 upvote \"hash\"") + HelpExampleCli("setwithdrawalvote", "0 downvote")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const SidechainId id{ParseSlot(request.params[0], chainman)};
    const std::string type{request.params[1].get_str()};
    Vote vote;
    if (type == "upvote") {
        if (request.params[2].isNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "An upvote needs the hash of the bundle");
        vote.type = Vote::Type::UPVOTE;
        vote.bundle = ParseHashV(request.params[2], "hash");
    } else if (type == "downvote") {
        vote.type = Vote::Type::DOWNVOTE;
    } else if (type == "abstain") {
        vote.type = Vote::Type::ABSTAIN;
    } else if (type == "default") {
        chainman.m_drivechain_miner.ClearVote(id);
        return UniValue::VNULL;
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "vote must be upvote, downvote, abstain or default");
    }
    chainman.m_drivechain_miner.SetVote(id, vote);
    return UniValue::VNULL;
},
    };
}

RPCMethod setdefaultwithdrawalvote()
{
    return RPCMethod{
        "setdefaultwithdrawalvote",
        "Set how this node votes on the withdrawal bundles of sidechains it has no specific vote for.\n"
        "A new node upvotes: it votes for the bundle that a sidechain node handed to it with receivewithdrawalbundle,\n"
        "which is the bundle the sidechain node of its operator holds to be right, and abstains where it was handed none.\n"
        "\"follow\" also upvotes, where it was handed no bundle, the one that leads the next by 50 votes (BIP300\n"
        "\"leading by 50\"): for miners, such as pools, that do not run every sidechain and back what the miners\n"
        "who check the bundles back. It trusts those miners, so it is something to opt in to.",
        {
            {"vote", RPCArg::Type::STR, RPCArg::Optional::NO, "\"upvote\", \"follow\", \"downvote\" or \"abstain\""},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{HelpExampleCli("setdefaultwithdrawalvote", "downvote")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const std::string type{request.params[0].get_str()};
    if (type == "upvote" || type == "follow") {
        chainman.m_drivechain_miner.SetDefaultVote(Vote::Type::UPVOTE);
        chainman.m_drivechain_miner.SetFollow(type == "follow");
    } else if (type == "downvote") {
        chainman.m_drivechain_miner.SetDefaultVote(Vote::Type::DOWNVOTE);
    } else if (type == "abstain") {
        chainman.m_drivechain_miner.SetDefaultVote(Vote::Type::ABSTAIN);
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "vote must be upvote, follow, downvote or abstain");
    }
    return UniValue::VNULL;
},
    };
}

//! Most escrow changes listsidechaindeposits returns at a time.
static constexpr unsigned int MAX_DEPOSITS_LISTED{1000};

RPCMethod listsidechaindeposits()
{
    return RPCMethod{
        "listsidechaindeposits",
        "List the changes to the escrow of a sidechain in the active chain, oldest first: deposits, and the change returned by withdrawals.\n"
        "Sidechain software uses this to credit deposits.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sidechain slot number"},
            {"after", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Only list the changes after the transaction with this id"},
            {"count", RPCArg::Type::NUM, RPCArg::Default{MAX_DEPOSITS_LISTED}, strprintf("The maximum number of changes to return, at most %u; 0 for that many. To list more, ask again with the last one in 'after'", MAX_DEPOSITS_LISTED)},
        },
        RPCResult{RPCResult::Type::ARR, "", "",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "slot", "The sidechain slot number"},
                {RPCResult::Type::STR, "destination", "The destination on the sidechain; \"" + drivechain::WITHDRAWAL_RETURN_DEST + "\" for the change of a withdrawal"},
                {RPCResult::Type::STR_AMOUNT, "amount", "The amount added to the escrow; 0 for a withdrawal"},
                {RPCResult::Type::STR_AMOUNT, "total", "The amount held in escrow after the transaction"},
                {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                {RPCResult::Type::NUM, "burnindex", "The index of the escrow output in the transaction"},
                {RPCResult::Type::NUM, "txindex", "The position of the transaction in its block"},
                {RPCResult::Type::STR_HEX, "blockhash", "The hash of the block containing the transaction"},
                {RPCResult::Type::NUM, "confirmations", "The number of confirmations of the transaction"},
                {RPCResult::Type::STR_HEX, "hex", "The transaction"},
            }},
        }},
        RPCExamples{HelpExampleCli("listsidechaindeposits", "0") + HelpExampleRpc("listsidechaindeposits", "0")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const SidechainId id{ParseSlot(request.params[0], chainman)};
    std::optional<uint256> after;
    if (!request.params[1].isNull()) after = ParseHashV(request.params[1], "after");
    int64_t count{request.params[2].isNull() ? 0 : request.params[2].getInt<int64_t>()};
    if (count < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "count must not be negative");
    if (count > MAX_DEPOSITS_LISTED) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("count must be at most %u", MAX_DEPOSITS_LISTED));
    // Bounded, since the main lock is held throughout.
    if (count == 0) count = MAX_DEPOSITS_LISTED;

    LOCK(::cs_main);
    // Records of blocks the active chain no longer has (blocks that left it keep theirs) are neither
    // listed nor counted, nor taken as the place to go on from.
    const auto in_active_chain{[&](const uint256& block_hash) {
        AssertLockHeld(::cs_main);
        const CBlockIndex* pindex{chainman.m_blockman.LookupBlockIndex(block_hash)};
        return pindex && chainman.ActiveChain().Contains(*pindex);
    }};
    const auto deposits{chainman.m_blockman.m_drivechain_db->ListDeposits(id, after, static_cast<size_t>(count), in_active_chain)};
    if (!deposits) throw JSONRPCError(RPC_INVALID_PARAMETER, "The transaction given in 'after' is not an escrow change of this sidechain in the active chain");

    UniValue result(UniValue::VARR);
    for (const drivechain::Deposit& deposit : *deposits) {
        const CBlockIndex* pindex{Assert(chainman.m_blockman.LookupBlockIndex(deposit.block_hash))};
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("slot", deposit.slot);
        obj.pushKV("destination", deposit.destination);
        obj.pushKV("amount", ValueFromAmount(deposit.amount));
        obj.pushKV("total", ValueFromAmount(deposit.total));
        obj.pushKV("txid", deposit.tx->GetHash().GetHex());
        obj.pushKV("burnindex", deposit.burn_index);
        obj.pushKV("txindex", deposit.tx_index);
        obj.pushKV("blockhash", deposit.block_hash.GetHex());
        obj.pushKV("confirmations", chainman.ActiveHeight() - pindex->nHeight + 1);
        obj.pushKV("hex", EncodeHexTx(*deposit.tx));
        result.push_back(std::move(obj));
    }
    return result;
},
    };
}

RPCMethod getsidechainevents()
{
    return RPCMethod{
        "getsidechainevents",
        "Returns everything that blocks of the active chain did that concerns one sidechain: the sidechain block they\n"
        "committed to (blind merged mining), the changes they made to its escrow, the withdrawal bundles they proposed\n"
        "and closed, and the bundles pending after each of them.\n"
        "Sidechain software follows this chain with it, block by block.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sidechain slot number"},
            {"height", RPCArg::Type::NUM, RPCArg::Optional::NO, "The height of the first block"},
            {"count", RPCArg::Type::NUM, RPCArg::Default{1}, "The number of blocks, at most 2000. Blocks past the tip are left out."},
        },
        RPCResult{RPCResult::Type::ARR, "", "One entry per block, in chain order",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "height", "The height of the block"},
                {RPCResult::Type::STR_HEX, "hash", "The hash of the block"},
                {RPCResult::Type::STR_HEX, "previousblockhash", /*optional=*/true, "The hash of the block before it"},
                {RPCResult::Type::NUM_TIME, "time", "The time of the block"},
                {RPCResult::Type::NUM_TIME, "mediantime", "The median time of the block and the 10 before it, which a miner cannot set ahead"},
                {RPCResult::Type::STR_HEX, "bmm", /*optional=*/true, "The hash of the sidechain block the block committed to, if any"},
                {RPCResult::Type::ARR, "deposits", "The escrow changes, in block order",
                {
                    {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "destination", "The destination on the sidechain; \"" + drivechain::WITHDRAWAL_RETURN_DEST + "\" for the change of a withdrawal"},
                        {RPCResult::Type::STR_AMOUNT, "amount", "The amount added to the escrow"},
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                        {RPCResult::Type::NUM, "burnindex", "The index of the escrow output in the transaction"},
                        {RPCResult::Type::STR_HEX, "bundle", /*optional=*/true, "For the change of a withdrawal: the bundle it paid out"},
                        {RPCResult::Type::ARR, "payouts", /*optional=*/true, "For the change of a withdrawal: what it paid", {
                            {RPCResult::Type::OBJ, "", "", {
                                {RPCResult::Type::STR_AMOUNT, "amount", "The amount paid"},
                                {RPCResult::Type::STR_HEX, "script", "The output script paid"},
                            }},
                        }},
                    }},
                }},
                {RPCResult::Type::ARR, "bundles", "The withdrawal bundles the block closed",
                {
                    {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hash", "The hash of the bundle that miners voted on"},
                        {RPCResult::Type::BOOL, "paid", "Whether the bundle was paid out; if not, it failed"},
                    }},
                }},
                {RPCResult::Type::ARR, "proposed", "The withdrawal bundles the block proposed (BIP300 M3): pending from it until a block closes them",
                {
                    {RPCResult::Type::STR_HEX, "", "The hash of the bundle"},
                }},
                {RPCResult::Type::ARR, "pending", "The withdrawal bundles of the sidechain that are pending after the block (proposed, and not closed yet),\n"
                                                  "in the order vote messages number them. Recorded when the block was connected: the same for any height, at any time",
                {
                    {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hash", "The hash of the bundle"},
                        {RPCResult::Type::NUM, "score", "Its work score after the block"},
                    }},
                }},
            }},
        }},
        RPCExamples{HelpExampleCli("getsidechainevents", "0 1000 100")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const SidechainId id{ParseSlot(request.params[0], chainman)};
    const int first{request.params[1].getInt<int>()};
    const int count{request.params[2].isNull() ? 1 : request.params[2].getInt<int>()};
    if (first < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "The height must not be negative");
    if (count < 1 || count > 2000) throw JSONRPCError(RPC_INVALID_PARAMETER, "The count must be between 1 and 2000");

    UniValue result(UniValue::VARR);
    // The blocks are read without the main lock, which up to 2000 of them would hold too long; then,
    // with it, the chain is checked to be the one they were read from, and the rest is filled in.
    std::vector<std::pair<const CBlockIndex*, FlatFilePos>> wanted;
    {
        LOCK(::cs_main);
        const CChain& chain{chainman.ActiveChain()};
        // Counted from `first`, which may be near the largest int: first + count would overflow.
        for (int height{first}; height <= chain.Height() && height - first < count; ++height) {
            wanted.emplace_back(chain[height], chain[height]->GetBlockPos());
        }
    }
    // One block at a time: only the sidechain block its coinbase committed to is kept.
    std::vector<std::optional<uint256>> bmm(wanted.size());
    for (size_t i{0}; i < wanted.size(); ++i) {
        CBlock block;
        if (!chainman.m_blockman.ReadBlock(block, wanted[i].second, wanted[i].first->GetBlockHash())) {
            throw JSONRPCError(RPC_MISC_ERROR, strprintf("Block %d is not available (pruned?)", first + static_cast<int>(i)));
        }
        for (const CTxOut& out : block.vtx[0]->vout) {
            const auto accept{drivechain::ParseBmmAcceptScript(out.scriptPubKey)};
            if (accept && accept->first == id) {
                bmm[i] = accept->second;
                break;
            }
        }
    }
    LOCK(::cs_main);
    const CChain& chain{chainman.ActiveChain()};
    for (size_t i{0}; i < wanted.size(); ++i) {
        if (chain[first + static_cast<int>(i)] != wanted[i].first) throw JSONRPCError(RPC_MISC_ERROR, "The chain changed meanwhile; ask again");
    }
    for (int height{first}; height < first + static_cast<int>(wanted.size()); ++height) {
        const CBlockIndex* pindex{chain[height]};
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("height", height);
        obj.pushKV("hash", pindex->GetBlockHash().GetHex());
        if (pindex->pprev) obj.pushKV("previousblockhash", pindex->pprev->GetBlockHash().GetHex());
        obj.pushKV("time", pindex->GetBlockTime());
        obj.pushKV("mediantime", pindex->GetMedianTimePast());

        if (const auto& accepted{bmm[height - first]}) obj.pushKV("bmm", accepted->GetHex());

        UniValue deposits(UniValue::VARR);
        for (const drivechain::Deposit& deposit : chainman.m_blockman.m_drivechain_db->ListBlockDeposits(id, height, pindex->GetBlockHash())) {
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("destination", deposit.destination);
            entry.pushKV("amount", ValueFromAmount(deposit.amount));
            entry.pushKV("txid", deposit.tx->GetHash().GetHex());
            entry.pushKV("burnindex", deposit.burn_index);
            if (deposit.destination == drivechain::WITHDRAWAL_RETURN_DEST) {
                // What a withdrawal paid, so that sidechain software knows it was paid on any branch
                // of its chain, also one that never saw the bundle.
                entry.pushKV("bundle", deposit.bundle.GetHex());
                UniValue payouts(UniValue::VARR);
                for (size_t i{0}; i < deposit.tx->vout.size(); ++i) {
                    if (i == deposit.burn_index || deposit.tx->vout[i].nValue == 0) continue;
                    UniValue payout(UniValue::VOBJ);
                    payout.pushKV("amount", ValueFromAmount(deposit.tx->vout[i].nValue));
                    payout.pushKV("script", HexStr(deposit.tx->vout[i].scriptPubKey));
                    payouts.push_back(std::move(payout));
                }
                entry.pushKV("payouts", std::move(payouts));
            }
            deposits.push_back(std::move(entry));
        }
        obj.pushKV("deposits", std::move(deposits));

        UniValue bundles(UniValue::VARR);
        drivechain::BlockEvents events;
        // What a block closed and proposed is what sidechains act on: missing, it must not look like nothing.
        const bool have_events{chainman.m_blockman.m_drivechain_db->ReadBlockEvents(pindex->GetBlockHash(), events)};
        if (!have_events && height > 0) throw JSONRPCError(RPC_MISC_ERROR, strprintf("The drivechain data of block %d is not available", height));
        for (const drivechain::BlockUndo::Closed& closed : events.closed) {
            if (closed.id != id) continue;
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("hash", closed.hash.GetHex());
            entry.pushKV("paid", closed.paid);
            bundles.push_back(std::move(entry));
        }
        obj.pushKV("bundles", std::move(bundles));

        // The proposals the block made that became pending (a proposal for a slot without a
        // sidechain is ignored, and changes nothing).
        UniValue proposed(UniValue::VARR);
        for (const auto& [slot, hash] : events.proposed) {
            if (slot == id) proposed.push_back(hash.GetHex());
        }
        obj.pushKV("proposed", std::move(proposed));

        // What is pending after the block, as the block left it: a sidechain that saw a block
        // propose or close a bundle knows from this, at any height, what it may still have to pay.
        UniValue pending(UniValue::VARR);
        for (const auto& [slot, bundles] : events.pending) {
            if (slot != id) continue;
            for (const auto& [hash, score] : bundles) {
                UniValue entry(UniValue::VOBJ);
                entry.pushKV("hash", hash.GetHex());
                entry.pushKV("score", score);
                pending.push_back(std::move(entry));
            }
        }
        obj.pushKV("pending", std::move(pending));
        result.push_back(std::move(obj));
    }
    return result;
},
    };
}

RPCMethod verifybmm()
{
    return RPCMethod{
        "verifybmm",
        "Check whether a block of the active chain commits to a sidechain block, that is, whether its coinbase accepts it for blind merged mining.",
        {
            {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash of the mainchain block"},
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sidechain slot number"},
            {"sideblockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash of the sidechain block"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::BOOL, "verified", "Whether the block commits to the sidechain block"},
            {RPCResult::Type::NUM, "height", /*optional=*/true, "The height of the mainchain block"},
            {RPCResult::Type::NUM, "confirmations", /*optional=*/true, "The number of confirmations of the mainchain block"},
            {RPCResult::Type::NUM_TIME, "time", /*optional=*/true, "The time of the mainchain block"},
        }},
        RPCExamples{HelpExampleCli("verifybmm", "\"blockhash\" 0 \"sideblockhash\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const uint256 block_hash{ParseHashV(request.params[0], "blockhash")};
    const SidechainId id{ParseSlot(request.params[1], chainman)};
    const uint256 side_hash{ParseHashV(request.params[2], "sideblockhash")};

    const CBlockIndex* pindex;
    int tip_height;
    {
        LOCK(::cs_main);
        pindex = chainman.m_blockman.LookupBlockIndex(block_hash);
        if (!pindex || !chainman.ActiveChain().Contains(*pindex)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found in the active chain");
        tip_height = chainman.ActiveHeight();
    }
    CBlock block;
    if (!chainman.m_blockman.ReadBlock(block, *pindex)) throw JSONRPCError(RPC_MISC_ERROR, "Block not available");

    bool verified{false};
    for (const CTxOut& out : block.vtx[0]->vout) {
        const auto accept{drivechain::ParseBmmAcceptScript(out.scriptPubKey)};
        if (accept && accept->first == id && accept->second == side_hash) verified = true;
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("verified", verified);
    if (verified) {
        result.pushKV("height", pindex->nHeight);
        result.pushKV("confirmations", tip_height - pindex->nHeight + 1);
        result.pushKV("time", pindex->GetBlockTime());
    }
    return result;
},
    };
}

} // namespace

void RegisterDrivechainRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"drivechain", &getdrivechaininfo},
        {"drivechain", &listactivesidechains},
        {"drivechain", &getsidechain},
        {"drivechain", &listsidechainproposals},
        {"drivechain", &createsidechainproposal},
        {"drivechain", &removesidechainproposal},
        {"drivechain", &acksidechain},
        {"drivechain", &receivewithdrawalbundle},
        {"drivechain", &vouchwithdrawalbundle},
        {"drivechain", &listwithdrawalbundles},
        {"drivechain", &getwithdrawalbundle},
        {"drivechain", &getaveragefee},
        {"drivechain", &sendwithdrawalbundle},
        {"drivechain", &setwithdrawalvote},
        {"drivechain", &setdefaultwithdrawalvote},
        {"drivechain", &listsidechaindeposits},
        {"drivechain", &getsidechainevents},
        {"drivechain", &verifybmm},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
