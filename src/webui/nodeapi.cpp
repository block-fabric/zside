// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/nodeapi.h>

#include <base58.h>
#include <chain.h>
#include <bech32.h>
#include <crypto/ripemd160.h>
#include <crypto/sha1.h>
#include <crypto/sha256.h>
#include <crypto/sha512.h>
#include <drivechain/sidechain.h>
#include <hash.h>
#include <interfaces/node.h>
#include <net.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <net_processing.h>
#include <node/context.h>
#include <rpc/blockchain.h>
#include <validation.h>
#include <node/blockstorage.h>
#include <key_io.h>
#include <addresstype.h>
#if WEBUI_SIDECHAIN
#include <sidechain/state.h>
#endif
#include <netbase.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <webui/bridge.h>
#include <webui/chainconfig.h>
#include <webui/confirm.h>
#include <webui/console.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <utility>

namespace webui {
using util::SplitString;
using util::TrimString;
namespace {
const UniValue& Param(const UniValue& params, const std::string& key)
{
    return params.isObject() ? params.find_value(key) : NullUniValue;
}

std::string ParamStr(const UniValue& params, const std::string& key)
{
    const UniValue& value{Param(params, key)};
    if (!value.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a string is needed", key));
    return value.get_str();
}

int64_t ParamInt(const UniValue& params, const std::string& key)
{
    const UniValue& value{Param(params, key)};
    if (!value.isNum()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a number is needed", key));
    return value.getInt<int64_t>();
}

template <typename Hasher>
std::string Digest(std::span<const unsigned char> data)
{
    unsigned char out[Hasher::OUTPUT_SIZE];
    Hasher{}.Write(data.data(), data.size()).Finalize(out);
    return HexStr(out);
}

//! The most rows of the explorer one call gives (explorer.blocks).
constexpr int64_t MAX_EXPLORER_ROWS{10'000};

const std::vector<std::string> METHODS{
    "console.check", "console.run", "peers.disconnect", "peers.ban", "bans.unban", "drivechain.depositAddress",
    "tools.hashes", "tools.merkle", "tools.decode", "timestamp.hashFile", "timestamp.search", "explorer.blocks",
#if WEBUI_SIDECHAIN
    "explorer.peg",
#endif
};
//! The most rows of each kind explorer.peg gives (a block of a few thousand withdrawals is shown in part).
constexpr size_t MAX_PEG_ROWS{1000};
} // namespace

UniValue HashesOf(const std::vector<unsigned char>& data)
{
    const uint256 sha256d{Hash(data)};
    UniValue out{UniValue::VOBJ};
    out.pushKV("sha256", Digest<CSHA256>(data));
    out.pushKV("sha256d", HexStr(sha256d));
    out.pushKV("sha256dId", sha256d.GetHex());
    out.pushKV("ripemd160", Digest<CRIPEMD160>(data));
    out.pushKV("hash160", HexStr(Hash160(data)));
    out.pushKV("sha512", Digest<CSHA512>(data));
    out.pushKV("sha1", Digest<CSHA1>(data));
    out.pushKV("bytes", static_cast<int64_t>(data.size()));
    return out;
}

std::vector<std::vector<std::string>> MerkleLevels(const std::vector<std::string>& txids)
{
    std::vector<uint256> level;
    for (const std::string& line : txids) {
        const std::string id{TrimString(line)};
        if (id.empty()) continue;
        const auto hash{uint256::FromHex(id)};
        if (!hash) throw std::runtime_error(strprintf("\"%s\" is not a transaction id (64 hexadecimal characters).", id));
        level.push_back(*hash);
    }
    std::vector<std::vector<std::string>> levels;
    while (!level.empty()) {
        std::vector<std::string> shown;
        for (const uint256& h : level) shown.push_back(h.GetHex());
        levels.push_back(std::move(shown));
        if (level.size() == 1) break;
        // A level with an odd number of hashes has its last hash paired with itself.
        if (level.size() % 2 == 1) level.push_back(level.back());
        std::vector<uint256> next;
        for (size_t i{0}; i < level.size(); i += 2) next.push_back(Hash(level[i], level[i + 1]));
        level = std::move(next);
    }
    return levels;
}

NodeApi::NodeApi(NodeFn node, ReadableFn readable, guard::ConfirmFn confirm)
    : m_node(std::move(node)), m_readable(std::move(readable)), m_confirm(std::move(confirm)) {}

std::vector<std::string> NodeApi::Methods() const { return METHODS; }

void NodeApi::Register(Bridge& bridge)
{
    for (const std::string& method : METHODS) {
        // Commands of the console, the search of a stamp and the hash of a file can take long: they run
        // apart from the quick calls of the pages.
        const bool slow{method == "console.run" || method == "timestamp.search" || method == "timestamp.hashFile"};
        bridge.RegisterHost(method, [this, method](const UniValue& params) { return Call(method, params); }, slow ? Bridge::Thread::LONG : Bridge::Thread::WORKER);
    }
}

UniValue NodeApi::Call(const std::string& method, const UniValue& params)
{
    if (method == "console.check") {
        return console::Check(ParamStr(params, "line"));
    }
    if (method == "console.run") {
        interfaces::Node& node{m_node()};
        const UniValue& wallet{Param(params, "wallet")};
        const std::string wallet_name{wallet.isStr() ? wallet.get_str() : std::string{}};
        // The page can call this with any line: a command that does more than read waits for the user's word,
        // given to the desktop, for each call (the line is checked as it runs, nested commands too), with the parameters
        // it runs with shown (audit 7 #3).
        const console::ExecuteFn execute{[&](const std::string& m, const UniValue& p, const std::string& uri) {
            // Only what the caller may run itself (the RPC user's -rpcwhitelist, for the host over RPC), nested commands too.
            CheckRpcCaller(m);
            if (!guard::ConsoleRunsQuietly(m)) {
                const auto dialog{guard::ConsoleConfirmation(m, p, wallet_name, confirm::ArgNames(node, m))};
                if (!dialog) throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found");
                if (!m_confirm(*dialog)) throw JSONRPCError(RPC_MISC_ERROR, strprintf("%s was not run: it was declined", m));
            }
            return node.executeRpc(m, p, uri);
        }};
        return console::Run(execute, ParamStr(params, "line"), wallet_name);
    }
    if (method == "peers.disconnect") {
        return UniValue{m_node().disconnectById(ParamInt(params, "id"))};
    }
    if (method == "peers.ban") {
        interfaces::Node& node{m_node()};
        const int64_t id{ParamInt(params, "id")};
        const int64_t seconds{ParamInt(params, "seconds")};
        if (seconds <= 0) throw HostError(BRIDGE_INVALID_REQUEST, "seconds: a time to ban for is needed");
        interfaces::Node::NodesStats stats;
        if (!node.getNodesStats(stats)) throw HostError("The peers are not available");
        for (const auto& [node_stats, have_state, state] : stats) {
            if (node_stats.nodeid != id) continue;
            // As RPCConsole::banSelectedNode: the address of the peer, then every peer at it disconnected.
            node.ban(node_stats.addr, seconds);
            node.disconnectByAddress(node_stats.addr);
            return UniValue{true};
        }
        return UniValue{false};
    }
    if (method == "bans.unban") {
        const CSubNet subnet{LookupSubNet(ParamStr(params, "address"))};
        if (!subnet.IsValid()) throw HostError(BRIDGE_INVALID_REQUEST, "Not an address or a subnet");
        return UniValue{m_node().unban(subnet)};
    }
    if (method == "drivechain.depositAddress") {
        drivechain::DepositAddress deposit_address;
        const auto kind{drivechain::ParseDepositAddress(TrimString(ParamStr(params, "text")), deposit_address)};
        UniValue out{UniValue::VOBJ};
        out.pushKV("kind", kind == drivechain::DepositAddressKind::VALID ? "valid" : kind == drivechain::DepositAddressKind::INVALID ? "invalid" : "plain");
        if (kind == drivechain::DepositAddressKind::VALID) {
            out.pushKV("slot", static_cast<int64_t>(deposit_address.slot));
            out.pushKV("address", deposit_address.address);
        }
        return out;
    }
    if (method == "tools.hashes") {
        const std::string text{ParamStr(params, "text")};
        const UniValue& hex{Param(params, "hex")};
        std::vector<unsigned char> data;
        if (hex.isBool() && hex.get_bool()) {
            std::string compact;
            for (const char c : text) {
                if (!IsSpace(c)) compact += c;
            }
            const auto bytes{TryParseHex<unsigned char>(compact)};
            if (!bytes) {
                UniValue out{UniValue::VOBJ};
                out.pushKV("error", "This is not hexadecimal data.");
                return out;
            }
            data = *bytes;
        } else {
            data.assign(text.begin(), text.end());
        }
        return HashesOf(data);
    }
    if (method == "tools.merkle") {
        const UniValue& txids{Param(params, "txids")};
        if (!txids.isArray()) throw HostError(BRIDGE_INVALID_REQUEST, "txids: a list is needed");
        std::vector<std::string> ids;
        for (const UniValue& id : txids.getValues()) {
            if (!id.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "txids: strings are needed");
            ids.push_back(id.get_str());
        }
        UniValue out{UniValue::VOBJ};
        try {
            UniValue levels{UniValue::VARR};
            for (const auto& level : MerkleLevels(ids)) {
                UniValue l{UniValue::VARR};
                for (const auto& h : level) l.push_back(h);
                levels.push_back(std::move(l));
            }
            out.pushKV("levels", std::move(levels));
        } catch (const std::runtime_error& e) {
            out.pushKV("error", e.what());
        }
        return out;
    }
    if (method == "tools.decode") {
        const std::string input{TrimString(ParamStr(params, "text"))};
        UniValue out{UniValue::VOBJ};
        std::vector<unsigned char> payload;
        if (DecodeBase58Check(input, payload, 100) && !payload.empty()) {
            UniValue base58{UniValue::VOBJ};
            base58.pushKV("version", static_cast<int>(payload[0]));
            base58.pushKV("payload", HexStr(std::span{payload}.subspan(1)));
            out.pushKV("base58", std::move(base58));
        }
        const auto bech{bech32::Decode(input)};
        if (bech.encoding != bech32::Encoding::INVALID) {
            UniValue b{UniValue::VOBJ};
            b.pushKV("encoding", bech.encoding == bech32::Encoding::BECH32M ? "bech32m" : "bech32");
            b.pushKV("prefix", bech.hrp);
            if (!bech.data.empty()) {
                b.pushKV("witnessVersion", static_cast<int>(bech.data[0]));
                std::vector<unsigned char> program;
                if (ConvertBits<5, 8, false>([&](unsigned char c) { program.push_back(c); }, bech.data.begin() + 1, bech.data.end())) {
                    b.pushKV("witnessProgram", HexStr(program));
                }
            }
            out.pushKV("bech32", std::move(b));
        }
        return out;
    }
    if (method == "timestamp.hashFile") {
        const std::string path{ParamStr(params, "path")};
        if (!m_readable(path)) throw HostError(BRIDGE_INVALID_REQUEST, "Only a file chosen by the user can be read");
        std::ifstream file{static_cast<const std::filesystem::path&>(fs::PathFromString(UseChosenFile(path, /*write=*/false))), std::ios::binary};
        if (!file) throw HostError("The file cannot be read.");
        CSHA256 hasher;
        std::vector<char> chunk(1 << 20);
        int64_t size{0};
        while (file) {
            file.read(chunk.data(), chunk.size());
            if (file.gcount() > 0) {
                hasher.Write(reinterpret_cast<const unsigned char*>(chunk.data()), file.gcount());
                size += file.gcount();
            }
        }
        if (file.bad()) throw HostError("The file cannot be read.");
        unsigned char hash[CSHA256::OUTPUT_SIZE];
        hasher.Finalize(hash);
        UniValue out{UniValue::VOBJ};
        out.pushKV("sha256", HexStr(hash));
        out.pushKV("size", size);
        return out;
    }
    if (method == "explorer.blocks") {
        // Blocks of the active chain from the block index alone (no block read from disk), newest first: `count` of them
        // (at most MAX_EXPLORER_ROWS) from the height `from` down (the tip by default). The page asks for the rows it
        // lacks in parts (all of them: part after part; on a new tip: only the new ones), never the whole chain at once
        // (audit 7 #12). The index entries are taken under cs_main, the rows made without it (an entry of the block
        // index stays as long as the node runs). Each row: [height, hash, time, transactions, difficulty]; its size
        // and weight are read (getblock) for the rows a page shows.
        const node::NodeContext* context{m_node().context()};
        if (!context || !context->chainman) throw HostError("The chain is not loaded yet");
        const int64_t asked{ParamInt(params, "count")};
        if (asked < 1 || asked > MAX_EXPLORER_ROWS) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("count: 1 to %d", MAX_EXPLORER_ROWS));
        const UniValue& from_value{Param(params, "from")};
        if (!from_value.isNull() && !from_value.isNum()) throw HostError(BRIDGE_INVALID_REQUEST, "from: a height");
        std::vector<const CBlockIndex*> indexes;
        {
            LOCK(::cs_main);
            const CChain& chain{context->chainman->ActiveChain()};
            const int tip{chain.Height()};
            const int64_t from{from_value.isNull() ? tip : std::min<int64_t>(from_value.getInt<int64_t>(), tip)};
            const int64_t count{from < 0 ? 0 : std::min<int64_t>(asked, from + 1)};
            indexes.reserve(count);
            for (int64_t h{from}; h > from - count; --h) indexes.push_back(chain[static_cast<int>(h)]);
        }
        UniValue rows{UniValue::VARR};
        rows.reserve(indexes.size());
        for (const CBlockIndex* index : indexes) {
            UniValue row{UniValue::VARR};
            row.push_back(index->nHeight);
            row.push_back(index->GetBlockHash().GetHex());
            row.push_back(index->GetBlockTime());
            row.push_back(static_cast<int64_t>(index->nTx));
            row.push_back(GetDifficulty(*index));
            rows.push_back(std::move(row));
        }
        return rows;
    }
#if WEBUI_SIDECHAIN
    if (method == "explorer.peg") {
        // What a block of this sidechain does with the peg, read from the block (no index, no scan): the payouts of its
        // coinbase (outputs 1 and on: deposits from the mainchain, refunds, the chain's own payouts), the bundle it
        // commits to, the withdrawals its transactions make and the refunds they ask; MAX_PEG_ROWS of each at most
        // ("more" then). The explorer links each to its transaction and its address.
        const node::NodeContext* context{m_node().context()};
        if (!context || !context->chainman) throw HostError("The chain is not loaded yet");
        const auto hash{uint256::FromHex(ParamStr(params, "hash"))};
        if (!hash) throw HostError(BRIDGE_INVALID_REQUEST, "hash: a block hash is needed");
        const CBlockIndex* index;
        {
            LOCK(::cs_main);
            index = context->chainman->m_blockman.LookupBlockIndex(*hash);
            if (!index) throw HostError(BRIDGE_INVALID_REQUEST, "No block with this hash");
            if (!(index->nStatus & BLOCK_HAVE_DATA)) throw HostError("The block is not on disk (pruned)");
        }
        CBlock block;
        if (!context->chainman->m_blockman.ReadBlock(block, *index)) throw HostError("The block cannot be read");
        bool more{false};
        UniValue payouts{UniValue::VARR};
        UniValue bundle{UniValue::VNULL};
        if (!block.vtx.empty()) {
            const CTransaction& coinbase{*block.vtx[0]};
            for (uint32_t n{0}; n < coinbase.vout.size(); ++n) {
                const CTxOut& out{coinbase.vout[n]};
                if (const auto commit{sidechain::ParseBundleCommitScript(out.scriptPubKey)}) {
                    bundle = commit->GetHex();
                    continue;
                }
                if (n == 0 || out.scriptPubKey.IsUnspendable() || out.nValue == 0) continue;
                if (payouts.size() >= MAX_PEG_ROWS) {
                    more = true;
                    continue;
                }
                CTxDestination dest;
                UniValue row{UniValue::VARR};
                row.push_back(static_cast<int64_t>(n));
                row.push_back(out.nValue);
                row.push_back(ExtractDestination(out.scriptPubKey, dest) ? EncodeDestination(dest) : std::string{});
                payouts.push_back(std::move(row));
            }
        }
        UniValue withdrawals{UniValue::VARR};
        UniValue refunds{UniValue::VARR};
        for (size_t i{1}; i < block.vtx.size(); ++i) {
            const CTransaction& tx{*block.vtx[i]};
            for (uint32_t n{0}; n < tx.vout.size(); ++n) {
                const CTxOut& out{tx.vout[n]};
                if (const auto w{sidechain::IsWithdrawalScript(out.scriptPubKey) ? sidechain::ParseWithdrawalOutput(out) : std::nullopt}) {
                    if (withdrawals.size() >= MAX_PEG_ROWS) {
                        more = true;
                        continue;
                    }
                    UniValue row{UniValue::VARR};
                    row.push_back(tx.GetHash().GetHex());
                    row.push_back(static_cast<int64_t>(n));
                    row.push_back(w->amount);
                    row.push_back(w->main_fee);
                    row.push_back(HexStr(w->main_script));
                    row.push_back(EncodeDestination(WitnessV0KeyHash{w->refund_keyhash}));
                    withdrawals.push_back(std::move(row));
                } else if (const auto r{sidechain::ParseRefundScript(out.scriptPubKey)}) {
                    if (refunds.size() >= MAX_PEG_ROWS) {
                        more = true;
                        continue;
                    }
                    UniValue row{UniValue::VARR};
                    row.push_back(tx.GetHash().GetHex());
                    row.push_back(r->withdrawal.hash.GetHex());
                    row.push_back(static_cast<int64_t>(r->withdrawal.n));
                    refunds.push_back(std::move(row));
                }
            }
        }
        UniValue out{UniValue::VOBJ};
        out.pushKV("coinbase", block.vtx.empty() ? std::string{} : block.vtx[0]->GetHash().GetHex());
        out.pushKV("payouts", std::move(payouts));
        out.pushKV("bundle", std::move(bundle));
        out.pushKV("withdrawals", std::move(withdrawals));
        out.pushKV("refunds", std::move(refunds));
        out.pushKV("more", more);
        return out;
    }
#endif
    if (method == "timestamp.search") {
        interfaces::Node& node{m_node()};
        const std::string payload{ToLower(ParamStr(params, "payload"))};
        if (payload.empty() || !IsHex(payload)) throw HostError(BRIDGE_INVALID_REQUEST, "payload: hexadecimal data is needed");
        std::string hash{ParamStr(params, "from")};
        const int64_t count{std::clamp<int64_t>(ParamInt(params, "count"), 1, 1000)};
        UniValue found{UniValue::VARR};
        int64_t searched{0};
        for (; searched < count && !hash.empty(); ++searched) {
            UniValue args{UniValue::VARR};
            args.push_back(hash);
            args.push_back(2);
            UniValue block;
            try {
                block = node.executeRpc("getblock", args, "");
            } catch (const UniValue&) {
                hash.clear();
                break;
            }
            for (const UniValue& tx : block.find_value("tx").getValues()) {
                for (const UniValue& out : tx.find_value("vout").getValues()) {
                    const UniValue& script{out.find_value("scriptPubKey")};
                    if (!script.find_value("type").isStr() || script.find_value("type").get_str() != "nulldata") continue;
                    if (!script.find_value("hex").isStr() || !script.find_value("hex").get_str().ends_with(payload)) continue;
                    UniValue stamp{UniValue::VOBJ};
                    stamp.pushKV("height", block.find_value("height"));
                    stamp.pushKV("time", block.find_value("time"));
                    stamp.pushKV("txid", tx.find_value("txid"));
                    stamp.pushKV("block", hash);
                    found.push_back(std::move(stamp));
                }
            }
            const UniValue& previous{block.find_value("previousblockhash")};
            hash = previous.isStr() ? previous.get_str() : std::string{};
        }
        UniValue out{UniValue::VOBJ};
        out.pushKV("searched", searched);
        out.pushKV("next", hash.empty() ? UniValue{} : UniValue{hash});
        out.pushKV("found", std::move(found));
        return out;
    }
    throw HostError(BRIDGE_METHOD_NOT_FOUND, "No method " + method);
}

} // namespace webui
