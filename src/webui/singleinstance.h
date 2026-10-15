// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_SINGLEINSTANCE_H
#define BITCOIN_WEBUI_SINGLEINSTANCE_H

#include <util/fs.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace webui {

/**
 * One app per data directory and network, as the payment server of the Qt GUI
 * does: the app that runs listens on a local socket named after its data
 * directory; a second start for the same data directory hands its payment URIs
 * (or, without any, a request to show the window) to it and exits.
 *
 * Messages are lines: "uri <uri>" or "activate".
 */
class SingleInstance
{
public:
    using MessageFn = std::function<void(const std::string& line)>;

    /**
     * The socket of the app of a data directory (the network directory, as GetDataDirNet()), in a
     * directory private to this user; empty if there is none (no single app per data directory then).
     */
    static fs::path SocketPath(const fs::path& datadir_net);

    /**
     * Hand the messages to the app that runs for this socket.
     * @return false if no app listens there
     */
    static bool Send(const fs::path& socket, const std::vector<std::string>& lines);

    /** Hand the URIs of the command line to the app that runs, or ask it to show itself. */
    static bool SendToRunning(const fs::path& socket, const std::vector<std::string>& uris);

    SingleInstance() = default;
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    /**
     * Listen on the socket; `on_message` is called on a thread of the listener for each line received.
     * @return false if it cannot listen (another app listens there, or the system refused)
     */
    bool Listen(const fs::path& socket, MessageFn on_message);

    /** Stop listening and remove the socket. */
    void Stop();

#ifndef WIN32
    /** Whether a directory is this user's and private (not a link; no access for others). */
    static bool PrivateDir(const fs::path& dir);
#endif

    /** Most bytes of one message. */
    static constexpr size_t MAX_MESSAGE{64 * 1024};

private:
    fs::path m_socket;
    int m_fd{-1};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

} // namespace webui

#endif // BITCOIN_WEBUI_SINGLEINSTANCE_H
