// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/events.h>

#include <webui/bridge.h>

#include <algorithm>
#include <optional>
#include <utility>

namespace webui {

EventHub::EventHub(DeliverFn deliver, ScheduleFn schedule, std::chrono::milliseconds throttle)
    : m_deliver(std::move(deliver)), m_schedule(std::move(schedule)), m_throttle(throttle) {}

void EventHub::Emit(const std::string& type, UniValue data, const std::string& coalesce_key)
{
    UniValue event{UniValue::VOBJ};
    event.pushKV("type", type);
    event.pushKV("data", std::move(data));

    std::optional<std::chrono::milliseconds> flush;
    {
        LOCK(m_mutex);
        if (!coalesce_key.empty()) {
            const auto it{std::find_if(m_waiting.begin(), m_waiting.end(), [&](const Event& e) { return e.key == coalesce_key; })};
            if (it != m_waiting.end()) {
                // The newer state replaces the older one, at the place of the older one.
                it->value = std::move(event);
                return;
            }
        }
        m_waiting.push_back(Event{coalesce_key, std::move(event)});
        if (!m_ready && m_waiting.size() > MAX_KEPT) {
            m_waiting.erase(m_waiting.begin(), m_waiting.begin() + (m_waiting.size() - MAX_KEPT));
        }
        if (m_ready) flush = NeedFlush(coalesce_key.empty() ? std::chrono::milliseconds{0} : m_throttle);
    }
    // Scheduled without the lock: the scheduler may flush at once, on this thread.
    if (flush) m_schedule(*flush, [this] { Flush(); });
}

std::optional<std::chrono::milliseconds> EventHub::NeedFlush(std::chrono::milliseconds delay)
{
    const auto at{std::chrono::steady_clock::now() + delay};
    // One flush is enough, unless this event needs an earlier one.
    if (m_scheduled && m_scheduled_at <= at) return std::nullopt;
    m_scheduled = true;
    m_scheduled_at = at;
    return delay;
}

void EventHub::SetReady(bool ready)
{
    std::optional<std::chrono::milliseconds> flush;
    {
        LOCK(m_mutex);
        m_ready = ready;
        if (m_ready && !m_waiting.empty()) flush = NeedFlush(std::chrono::milliseconds{0});
    }
    if (flush) m_schedule(*flush, [this] { Flush(); });
}

void EventHub::Flush()
{
    std::vector<Event> batch;
    {
        LOCK(m_mutex);
        m_scheduled = false;
        if (!m_ready || m_waiting.empty()) return;
        batch.swap(m_waiting);
    }
    UniValue events{UniValue::VARR};
    for (Event& e : batch) events.push_back(std::move(e.value));
    // Text of the chain may be any bytes: the page gets valid UTF-8 (see ValidUtf8).
    m_deliver(ValidUtf8(events.write()));
}

size_t EventHub::Waiting() const
{
    LOCK(m_mutex);
    return m_waiting.size();
}

} // namespace webui
