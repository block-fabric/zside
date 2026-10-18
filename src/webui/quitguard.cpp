// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/quitguard.h>

#include <utility>

namespace webui {

void QuitGuard::Set(std::string message, int64_t now)
{
    if (message.size() > MAX_MESSAGE) {
        // Cut on a character, not inside one (UTF-8).
        size_t cut{MAX_MESSAGE};
        while (cut > 0 && (static_cast<unsigned char>(message[cut]) & 0xC0) == 0x80) --cut;
        message.resize(cut);
        message += "…";
    }
    LOCK(m_mutex);
    m_message = std::move(message);
    m_set_at = now;
}

std::optional<std::string> QuitGuard::Message(int64_t now) const
{
    LOCK(m_mutex);
    if (m_message.empty() || now - m_set_at > TTL_S || now < m_set_at) return std::nullopt;
    return m_message;
}

QuitGuard::Decision QuitGuard::OnQuit(int64_t now)
{
    const bool guarded{Message(now).has_value()};
    LOCK(m_mutex);
    // A second quit while the question shows: the user means it.
    if (m_asking || !guarded) return Decision::QUIT;
    m_asking = true;
    return Decision::ASK;
}

void QuitGuard::Answered()
{
    LOCK(m_mutex);
    m_asking = false;
}

bool QuitGuard::Asking() const
{
    LOCK(m_mutex);
    return m_asking;
}

} // namespace webui
