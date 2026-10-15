// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_NODEEVENTS_H
#define BITCOIN_WEBUI_NODEEVENTS_H

#include <sync.h>
#include <univalue.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace interfaces {
class Handler;
class Node;
class Wallet;
struct BlockTip;
} // namespace interfaces
enum class SynchronizationState;

namespace webui {

class EventHub;

/**
 * What happens in the node, as events for the page (what ClientModel and
 * WalletModel of the Qt GUI follow):
 *
 *   init.message   {message}
 *   progress       {title, progress, resumePossible}
 *   connections    {count}
 *   networkActive  {active}
 *   alert          {warnings}
 *   bans           {}
 *   tip            {height, time, hash, progress, state, ibd, loadingBlocks}
 *   headerTip      {height, time, hash, presync, state}
 *   wallet.loaded / wallet.unloaded   {name}
 *   wallet.status  {name, encrypted, locked}
 *   wallet.tx      {name, txid, change}
 *   wallet.balance {name, trusted, untrusted_pending, immature, used, nonmempool}
 *   wallet.progress {name, title, progress}
 *   wallet.addressbook {name, address, label, mine, purpose, change}
 *   traffic        {recv, sent, time}   (every second)
 *   mempool        {size, usage, max}   (when it changes)
 *
 * Text that comes from the node or the chain is passed as data (JSON
 * strings); the page escapes it when it shows it.
 */
class NodeEvents
{
public:
    NodeEvents(interfaces::Node& node, EventHub& hub);
    ~NodeEvents();
    NodeEvents(const NodeEvents&) = delete;
    NodeEvents& operator=(const NodeEvents&) = delete;

    /** Follow what happens during init (init messages, progress): before the node starts. */
    void SubscribeInit();

    /** Follow the node once it runs: tip, peers, alerts, wallets. */
    void SubscribeNode() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Stop following (before the node shuts down). Thread-safe, idempotent. */
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The state of the node now, for a page that (re)starts: the fields of the events. */
    UniValue Snapshot() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Names of the wallets followed. */
    std::vector<std::string> Wallets() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** How often balances are checked when something changed (as WalletModel's poll timer). */
    static constexpr int BALANCE_POLL_MS{250};

private:
    struct WalletEntry;

    void AddWallet(std::unique_ptr<interfaces::Wallet> wallet) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Poll() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    UniValue TipEvent(SynchronizationState state, const interfaces::BlockTip& tip, double progress);
    void EmitAlerts();

    interfaces::Node& m_node;
    EventHub& m_hub;

    std::vector<std::unique_ptr<interfaces::Handler>> m_init_handlers;
    std::vector<std::unique_ptr<interfaces::Handler>> m_handlers;

    Mutex m_mutex;
    std::map<std::string, std::shared_ptr<WalletEntry>> m_wallets GUARDED_BY(m_mutex);
    std::condition_variable m_wake;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_tip_changed{false};
    std::thread m_poller;
    //! Polls so far, and the mempool last seen (only the poller thread uses them).
    uint64_t m_polls{0};
    int64_t m_mempool_size{-1};
    int64_t m_mempool_usage{-1};
};

/** The name of a synchronization state for the page: "reindex", "download", "synced". */
std::string SyncStateName(SynchronizationState state);

} // namespace webui

#endif // BITCOIN_WEBUI_NODEEVENTS_H
