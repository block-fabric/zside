// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/nodeevents.h>

#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <logging.h>
#include <net.h>
#include <node/context.h>
#include <util/threadnames.h>
#include <util/time.h>
#include <util/ui_change_type.h>
#include <util/translation.h>
#include <validation.h>
#include <wallet/types.h>
#include <webui/events.h>

#include <chrono>
#include <utility>

namespace webui {

std::string SyncStateName(SynchronizationState state)
{
    switch (state) {
    case SynchronizationState::INIT_REINDEX: return "reindex";
    case SynchronizationState::INIT_DOWNLOAD: return "download";
    case SynchronizationState::POST_INIT: return "synced";
    }
    return "synced";
}

namespace {
std::string ChangeName(ChangeType change)
{
    switch (change) {
    case CT_NEW: return "new";
    case CT_UPDATED: return "updated";
    case CT_DELETED: return "deleted";
    }
    return "updated";
}

std::string PurposeName(wallet::AddressPurpose purpose)
{
    switch (purpose) {
    case wallet::AddressPurpose::RECEIVE: return "receive";
    case wallet::AddressPurpose::SEND: return "send";
    case wallet::AddressPurpose::REFUND: return "refund";
    }
    return "unknown";
}

UniValue Balances(const std::string& name, const interfaces::WalletBalances& b)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("name", name);
    // Satoshis, as integers: the page formats them in the unit chosen.
    out.pushKV("trusted", b.balance);
    out.pushKV("untrusted_pending", b.unconfirmed_balance);
    out.pushKV("immature", b.immature_balance);
    out.pushKV("used", b.used_balance);
    out.pushKV("nonmempool", b.nonmempool_balance);
    return out;
}

UniValue WarningsArray(const bilingual_str& warnings)
{
    UniValue out{UniValue::VARR};
    if (!warnings.original.empty()) out.push_back(warnings.original);
    return out;
}
} // namespace

struct NodeEvents::WalletEntry {
    std::string name;
    std::unique_ptr<interfaces::Wallet> wallet;
    std::vector<std::unique_ptr<interfaces::Handler>> handlers;
    std::atomic<bool> dirty{true};
    std::atomic<bool> unloaded{false};
    bool have_balances{false};
    interfaces::WalletBalances balances;
};

NodeEvents::NodeEvents(interfaces::Node& node, EventHub& hub) : m_node(node), m_hub(hub) {}

NodeEvents::~NodeEvents()
{
    Stop();
}

UniValue NodeEvents::TipEvent(SynchronizationState state, const interfaces::BlockTip& tip, double progress)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("height", tip.block_height);
    out.pushKV("time", tip.block_time);
    out.pushKV("hash", tip.block_hash.GetHex());
    out.pushKV("progress", progress);
    out.pushKV("state", SyncStateName(state));
    out.pushKV("ibd", state != SynchronizationState::POST_INIT);
    out.pushKV("loadingBlocks", m_node.isLoadingBlocks());
    return out;
}

void NodeEvents::EmitAlerts()
{
    UniValue data{UniValue::VOBJ};
    data.pushKV("warnings", WarningsArray(m_node.getWarnings()));
    m_hub.Emit("alert", std::move(data), "alert");
}

void NodeEvents::SubscribeInit()
{
    m_init_handlers.push_back(m_node.handleInitMessage([this](const std::string& message) {
        UniValue data{UniValue::VOBJ};
        data.pushKV("message", message);
        m_hub.Emit("init.message", std::move(data), "init.message");
    }));
    m_init_handlers.push_back(m_node.handleShowProgress([this](const std::string& title, int progress, bool resume_possible) {
        UniValue data{UniValue::VOBJ};
        data.pushKV("title", title);
        data.pushKV("progress", progress);
        data.pushKV("resumePossible", resume_possible);
        // 0 and 100 start and end a progress: never lost to coalescing.
        m_hub.Emit("progress", std::move(data), progress > 0 && progress < 100 ? "progress" : "");
    }));
}

void NodeEvents::SubscribeNode()
{
    m_handlers.push_back(m_node.handleNotifyNumConnectionsChanged([this](int count) {
        UniValue data{UniValue::VOBJ};
        data.pushKV("count", count);
        m_hub.Emit("connections", std::move(data), "connections");
    }));
    m_handlers.push_back(m_node.handleNotifyNetworkActiveChanged([this](bool active) {
        UniValue data{UniValue::VOBJ};
        data.pushKV("active", active);
        m_hub.Emit("networkActive", std::move(data));
    }));
    m_handlers.push_back(m_node.handleNotifyAlertChanged([this] { EmitAlerts(); }));
    m_handlers.push_back(m_node.handleBannedListChanged([this] { m_hub.Emit("bans", UniValue{UniValue::VOBJ}, "bans"); }));
    m_handlers.push_back(m_node.handleNotifyBlockTip([this](SynchronizationState state, interfaces::BlockTip tip, double progress) {
        m_hub.Emit("tip", TipEvent(state, tip, progress), "tip");
        m_tip_changed = true;
        m_wake.notify_all();
    }));
    m_handlers.push_back(m_node.handleNotifyHeaderTip([this](SynchronizationState state, interfaces::BlockTip tip, bool presync) {
        UniValue data{UniValue::VOBJ};
        data.pushKV("height", tip.block_height);
        data.pushKV("time", tip.block_time);
        data.pushKV("hash", tip.block_hash.GetHex());
        data.pushKV("presync", presync);
        data.pushKV("state", SyncStateName(state));
        m_hub.Emit("headerTip", std::move(data), presync ? "presync" : "headerTip");
    }));
    // The progress of the node goes on after init (rescans, verification).
    if (m_init_handlers.empty()) SubscribeInit();

    // Wallets: the ones that come and the ones there already.
    // A node without wallets (-disablewallet, or a build without them) has no wallet loader.
    if (const node::NodeContext* context{m_node.context()}; context && context->wallet_loader) {
        m_handlers.push_back(m_node.walletLoader().handleLoadWallet([this](std::unique_ptr<interfaces::Wallet> wallet) {
            AddWallet(std::move(wallet));
        }));
        for (auto& wallet : m_node.walletLoader().getWallets()) AddWallet(std::move(wallet));
    }

    m_stop = false;
    m_poller = std::thread([this] {
        util::ThreadRename("webui-poll");
        while (!m_stop) {
            {
                WAIT_LOCK(m_mutex, lock);
                m_wake.wait_for(lock, std::chrono::milliseconds{BALANCE_POLL_MS}, [&] { return m_stop.load(); });
            }
            if (m_stop) break;
            Poll();
        }
    });
}

void NodeEvents::AddWallet(std::unique_ptr<interfaces::Wallet> wallet)
{
    if (!wallet) return;
    auto entry{std::make_shared<WalletEntry>()};
    entry->name = wallet->getWalletName();
    entry->wallet = std::move(wallet);
    {
        LOCK(m_mutex);
        if (m_stop || m_wallets.contains(entry->name)) return;
    }
    const std::string name{entry->name};
    std::weak_ptr<WalletEntry> weak{entry};
    interfaces::Wallet& w{*entry->wallet};
    entry->handlers.push_back(w.handleUnload([this, weak] {
        if (auto e{weak.lock()}) {
            e->unloaded = true;
            m_wake.notify_all();
        }
    }));
    entry->handlers.push_back(w.handleStatusChanged([this, weak] {
        if (auto e{weak.lock()}) {
            UniValue data{UniValue::VOBJ};
            data.pushKV("name", e->name);
            data.pushKV("encrypted", e->wallet->isCrypted());
            data.pushKV("locked", e->wallet->isLocked());
            m_hub.Emit("wallet.status", std::move(data), "wallet.status:" + e->name);
        }
    }));
    entry->handlers.push_back(w.handleTransactionChanged([this, weak](const Txid& txid, ChangeType change) {
        if (auto e{weak.lock()}) {
            UniValue data{UniValue::VOBJ};
            data.pushKV("name", e->name);
            data.pushKV("txid", txid.GetHex());
            data.pushKV("change", ChangeName(change));
            m_hub.Emit("wallet.tx", std::move(data));
            e->dirty = true;
        }
    }));
    entry->handlers.push_back(w.handleShowProgress([this, weak](const std::string& title, int progress) {
        if (auto e{weak.lock()}) {
            UniValue data{UniValue::VOBJ};
            data.pushKV("name", e->name);
            data.pushKV("title", title);
            data.pushKV("progress", progress);
            m_hub.Emit("wallet.progress", std::move(data), progress > 0 && progress < 100 ? "wallet.progress:" + e->name : "");
            if (progress == 100) e->dirty = true;
        }
    }));
    entry->handlers.push_back(w.handleAddressBookChanged([this, weak](const CTxDestination& address, const std::string& label, bool is_mine, wallet::AddressPurpose purpose, ChangeType change) {
        if (auto e{weak.lock()}) {
            UniValue data{UniValue::VOBJ};
            data.pushKV("name", e->name);
            data.pushKV("address", EncodeDestination(address));
            data.pushKV("label", label);
            data.pushKV("mine", is_mine);
            data.pushKV("purpose", PurposeName(purpose));
            data.pushKV("change", ChangeName(change));
            m_hub.Emit("wallet.addressbook", std::move(data));
        }
    }));
    {
        LOCK(m_mutex);
        if (m_stop || m_wallets.contains(name)) return;
        m_wallets.emplace(name, entry);
    }
    UniValue data{UniValue::VOBJ};
    data.pushKV("name", name);
    data.pushKV("encrypted", entry->wallet->isCrypted());
    data.pushKV("locked", entry->wallet->isLocked());
    data.pushKV("privateKeysDisabled", entry->wallet->privateKeysDisabled());
    data.pushKV("hd", entry->wallet->hdEnabled());
    m_hub.Emit("wallet.loaded", std::move(data));
    m_wake.notify_all();
}

void NodeEvents::Poll()
{
    std::vector<std::shared_ptr<WalletEntry>> wallets, gone;
    {
        LOCK(m_mutex);
        for (auto it{m_wallets.begin()}; it != m_wallets.end();) {
            if (it->second->unloaded) {
                gone.push_back(it->second);
                it = m_wallets.erase(it);
            } else {
                wallets.push_back(it->second);
                ++it;
            }
        }
    }
    for (auto& e : gone) {
        for (auto& h : e->handlers) h->disconnect();
        UniValue data{UniValue::VOBJ};
        data.pushKV("name", e->name);
        m_hub.Emit("wallet.unloaded", std::move(data));
    }
    // The traffic of the network once a second, and the size of the mempool when it changes (as the
    // timer of ClientModel: bytesChanged, mempoolSizeChanged).
    if (++m_polls % (1000 / BALANCE_POLL_MS) == 0) {
        UniValue traffic{UniValue::VOBJ};
        traffic.pushKV("recv", m_node.getTotalBytesRecv());
        traffic.pushKV("sent", m_node.getTotalBytesSent());
        traffic.pushKV("time", TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now()));
        m_hub.Emit("traffic", std::move(traffic), "traffic");
        const int64_t size{static_cast<int64_t>(m_node.getMempoolSize())};
        const int64_t usage{static_cast<int64_t>(m_node.getMempoolDynamicUsage())};
        if (size != m_mempool_size || usage != m_mempool_usage) {
            m_mempool_size = size;
            m_mempool_usage = usage;
            UniValue mempool{UniValue::VOBJ};
            mempool.pushKV("size", size);
            mempool.pushKV("usage", usage);
            mempool.pushKV("max", static_cast<int64_t>(m_node.getMempoolMaxUsage()));
            m_hub.Emit("mempool", std::move(mempool), "mempool");
        }
    }
    const bool tip_changed{m_tip_changed.exchange(false)};
    for (auto& e : wallets) {
        if (!tip_changed && !e->dirty) continue;
        interfaces::WalletBalances balances;
        uint256 block_hash;
        // As WalletModel::pollBalanceChanged: skip while the wallet is busy, try again at the next poll.
        if (!e->wallet->tryGetBalances(balances, block_hash)) {
            e->dirty = true;
            continue;
        }
        e->dirty = false;
        if (e->have_balances && !balances.balanceChanged(e->balances)) continue;
        e->have_balances = true;
        e->balances = balances;
        m_hub.Emit("wallet.balance", Balances(e->name, balances), "wallet.balance:" + e->name);
    }
}

void NodeEvents::Stop()
{
    m_stop = true;
    m_wake.notify_all();
    if (m_poller.joinable()) m_poller.join();
    for (auto& h : m_handlers) h->disconnect();
    for (auto& h : m_init_handlers) h->disconnect();
    std::map<std::string, std::shared_ptr<WalletEntry>> wallets;
    {
        LOCK(m_mutex);
        wallets.swap(m_wallets);
    }
    for (auto& [name, e] : wallets) {
        for (auto& h : e->handlers) h->disconnect();
    }
}

std::vector<std::string> NodeEvents::Wallets()
{
    LOCK(m_mutex);
    std::vector<std::string> names;
    for (const auto& [name, e] : m_wallets) {
        if (!e->unloaded) names.push_back(name);
    }
    return names;
}

UniValue NodeEvents::Snapshot()
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("connections", static_cast<int64_t>(m_node.getNodeCount(ConnectionDirection::Both)));
    out.pushKV("networkActive", m_node.getNetworkActive());
    out.pushKV("warnings", WarningsArray(m_node.getWarnings()));
    {
        UniValue tip{UniValue::VOBJ};
        const int height{m_node.getNumBlocks()};
        tip.pushKV("height", height);
        tip.pushKV("time", m_node.getLastBlockTime());
        tip.pushKV("hash", m_node.getBestBlockHash().GetHex());
        tip.pushKV("progress", m_node.getVerificationProgress());
        const bool ibd{m_node.isInitialBlockDownload()};
        tip.pushKV("state", ibd ? "download" : "synced");
        tip.pushKV("ibd", ibd);
        tip.pushKV("loadingBlocks", m_node.isLoadingBlocks());
        out.pushKV("tip", std::move(tip));
    }
    {
        UniValue header{UniValue::VOBJ};
        int height;
        int64_t time;
        if (m_node.getHeaderTip(height, time)) {
            header.pushKV("height", height);
            header.pushKV("time", time);
            header.pushKV("presync", false);
        }
        out.pushKV("headerTip", std::move(header));
    }
    UniValue wallets{UniValue::VARR};
    std::vector<std::shared_ptr<WalletEntry>> entries;
    {
        LOCK(m_mutex);
        for (const auto& [name, e] : m_wallets) {
            if (!e->unloaded) entries.push_back(e);
        }
    }
    for (const auto& e : entries) {
        UniValue w{UniValue::VOBJ};
        w.pushKV("name", e->name);
        w.pushKV("encrypted", e->wallet->isCrypted());
        w.pushKV("locked", e->wallet->isLocked());
        w.pushKV("privateKeysDisabled", e->wallet->privateKeysDisabled());
        w.pushKV("hd", e->wallet->hdEnabled());
        wallets.push_back(std::move(w));
    }
    out.pushKV("wallets", std::move(wallets));
    return out;
}

} // namespace webui
