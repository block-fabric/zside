// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_EVENTS_H
#define BITCOIN_WEBUI_EVENTS_H

#include <sync.h>
#include <univalue.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace webui {

/**
 * The events the node sends to the page: from any thread of the node to the
 * page, in order, in batches.
 *
 * An event with a coalescing key replaces the one with the same key that has
 * not been delivered yet (the block tip during a sync changes thousands of
 * times a second; only the latest one matters), and is delivered after at most
 * `throttle`. Other events are delivered at once, in the order they came.
 *
 * Until the page says it is ready, events are kept for it.
 */
class EventHub
{
public:
    //! Delivers a batch to the page, as the JSON array of the events. Called on the UI thread.
    using DeliverFn = std::function<void(const std::string& batch_json)>;
    //! Runs a function on the UI thread after a delay (zero: as soon as possible). Called without locks held.
    using ScheduleFn = std::function<void(std::chrono::milliseconds delay, std::function<void()>)>;

    EventHub(DeliverFn deliver, ScheduleFn schedule, std::chrono::milliseconds throttle = std::chrono::milliseconds{250});

    /** Send an event: {"type": type, "data": data}. Thread-safe. */
    void Emit(const std::string& type, UniValue data, const std::string& coalesce_key = {}) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The page is ready for events: deliver what was kept for it. Thread-safe. */
    void SetReady(bool ready) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Deliver what is waiting now. UI thread. */
    void Flush() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Events waiting to be delivered. */
    size_t Waiting() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Most events kept while the page is not ready; older ones are dropped. */
    static constexpr size_t MAX_KEPT{10000};

private:
    //! Whether a flush must be scheduled for an event that waits `delay` at most, and after how long.
    std::optional<std::chrono::milliseconds> NeedFlush(std::chrono::milliseconds delay) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    const DeliverFn m_deliver;
    const ScheduleFn m_schedule;
    const std::chrono::milliseconds m_throttle;

    mutable Mutex m_mutex;
    struct Event {
        std::string key;
        UniValue value;
    };
    std::vector<Event> m_waiting GUARDED_BY(m_mutex);
    bool m_ready GUARDED_BY(m_mutex){false};
    //! A flush is scheduled, and when it runs.
    bool m_scheduled GUARDED_BY(m_mutex){false};
    std::chrono::steady_clock::time_point m_scheduled_at GUARDED_BY(m_mutex);
};

} // namespace webui

#endif // BITCOIN_WEBUI_EVENTS_H
