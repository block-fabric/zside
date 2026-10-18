// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_QUITGUARD_H
#define BITCOIN_WEBUI_QUITGUARD_H

#include <sync.h>

#include <cstdint>
#include <optional>
#include <string>

namespace webui {

/**
 * Whether quitting asks first: a page keeps work that a quit would interrupt (Hivemind: a market still being
 * created, audit 7 E-HM1) and says so to the host (app.quitGuard, with the text to show). A quit asked then (the
 * window closed, Quit of the menu or of the tray, Ctrl+Q) shows the dialog of the host with that text, "Keep … open"
 * the default; never forever:
 *  - the page says it again within TTL_S, or it is forgotten (a page that hangs or went away holds nothing);
 *  - a second quit while the question shows quits (as does an answer "Quit anyway");
 *  - the end of the session of the desktop quits without asking (App: session_ending).
 * Thread-safe.
 */
class QuitGuard
{
public:
    //! How long what the page said holds without being said again (seconds).
    static constexpr int64_t TTL_S{60};
    //! The longest text kept (bytes).
    static constexpr size_t MAX_MESSAGE{600};

    enum class Decision {
        //! Quit now.
        QUIT,
        //! Ask (the text: Message).
        ASK,
    };

    /** What the page says at `now` (seconds): the text to show before a quit, or empty for none. Cut to MAX_MESSAGE. */
    void Set(std::string message, int64_t now) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** The text to show before a quit at `now`, if the page said one in the last TTL_S. */
    std::optional<std::string> Message(int64_t now) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** A quit is asked at `now`: ask first (the question starts showing), or quit (no text, or a question shows already). */
    Decision OnQuit(int64_t now) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** The question was answered (or went away): a quit asked later asks again (while the page still says so). */
    void Answered() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Whether the question shows now. */
    bool Asking() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    mutable Mutex m_mutex;
    std::string m_message GUARDED_BY(m_mutex);
    int64_t m_set_at GUARDED_BY(m_mutex){0};
    bool m_asking GUARDED_BY(m_mutex){false};
};

} // namespace webui

#endif // BITCOIN_WEBUI_QUITGUARD_H
