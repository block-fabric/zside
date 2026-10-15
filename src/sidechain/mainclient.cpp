// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/mainclient.h>

#include <netbase.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <support/cleanse.h>
#include <util/sock.h>
#include <util/strencodings.h>
#include <util/threadinterrupt.h>
#include <util/time.h>
#include <util/translation.h>

#include <fstream>

#ifndef WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sidechain {
namespace {
constexpr auto TIMEOUT{60s};
//! Largest reply accepted, to bound what a misbehaving server can make this node hold.
constexpr size_t MAX_REPLY_SIZE{256 * 1024 * 1024};

std::string PercentEncode(const std::string& text)
{
    std::string encoded;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += static_cast<char>(c);
        } else {
            encoded += strprintf("%%%02X", c);
        }
    }
    return encoded;
}
} // namespace

util::Result<std::string> ReadPasswordFile(const fs::path& path)
{
    //! A password is one short line; a larger file is not one.
    constexpr size_t MAX_SIZE{4096};
    const std::string name{fs::PathToString(path)};
    std::string content;
#ifndef WIN32
    const int fd{open(path.c_str(), O_RDONLY | O_NOCTTY | O_CLOEXEC)};
    if (fd < 0) return util::Error{Untranslated(strprintf("Cannot open the password file %s", name))};
    struct stat st;
    char buffer[MAX_SIZE + 1];
    const auto fail{[&](const std::string& why) {
        close(fd);
        memory_cleanse(buffer, sizeof(buffer));
        return util::Error{Untranslated(strprintf("The password file %s %s", name, why))};
    }};
    if (fstat(fd, &st) != 0) return fail("cannot be read");
    if (!S_ISREG(st.st_mode)) return fail("is not a regular file");
    if (st.st_uid != geteuid()) return fail("is not owned by this user");
    if (st.st_mode & (S_IRWXG | S_IRWXO)) return fail("can be read or written by other users: make it readable by its owner only (chmod 600)");
    ssize_t total{0};
    while (total <= static_cast<ssize_t>(MAX_SIZE)) {
        const ssize_t n{read(fd, buffer + total, sizeof(buffer) - total)};
        if (n < 0) return fail("cannot be read");
        if (n == 0) break;
        total += n;
    }
    close(fd);
    content.assign(buffer, total);
    // Do not leave a copy of the password on the stack.
    memory_cleanse(buffer, sizeof(buffer));
#else
    std::ifstream file{path.std_path(), std::ios::binary};
    if (!file.is_open()) return util::Error{Untranslated(strprintf("Cannot open the password file %s", name))};
    content.resize(MAX_SIZE + 1);
    file.read(content.data(), content.size());
    content.resize(file.gcount());
#endif
    if (content.size() > MAX_SIZE) return util::Error{Untranslated(strprintf("The password file %s is too large to hold a password", name))};
    const size_t end{content.find_first_of("\r\n")};
    std::string password{content.substr(0, end)};
    memory_cleanse(content.data(), content.size());
    if (password.empty()) return util::Error{Untranslated(strprintf("The password file %s holds no password on its first line", name))};
    return password;
}

UniValue MainClient::Call(const std::string& method, const UniValue& params, bool wallet) const
{
    const auto unreachable{[&](const std::string& why) {
        return MainClientError{strprintf("Cannot reach the mainchain node at %s:%u: %s", m_options.host, m_options.port, why), /*rpc_error_in=*/false};
    }};

    std::string credentials{m_options.credentials};
    if (credentials.empty()) {
        std::ifstream file{m_options.cookie_file.std_path()};
        if (!file.is_open() || !std::getline(file, credentials)) {
            throw unreachable(strprintf("no credentials: set -mainchainrpcuser and -mainchainrpcpassword, or check that the cookie file %s can be read", fs::PathToString(m_options.cookie_file)));
        }
    }

    const std::string body{JSONRPCRequestObj(method, params, 1).write() + "\n"};
    std::string uri{"/"};
    if (wallet && m_options.wallet_set) uri = "/wallet/" + PercentEncode(m_options.wallet);
    const std::string request{strprintf("POST %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAuthorization: Basic %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\n\r\n%s",
                                        uri, m_options.host, EncodeBase64(credentials), body.size(), body)};

    const auto address{Lookup(m_options.host, m_options.port, /*fAllowLookup=*/true)};
    if (!address) throw unreachable("unknown host");
    const std::unique_ptr<Sock> sock{ConnectDirectly(*address, /*manual_connection=*/true)};
    if (!sock) throw unreachable("connection refused");

    std::string reply;
    try {
        CThreadInterrupt interrupt;
        sock->SendComplete(request, TIMEOUT, interrupt);
        const auto deadline{SteadyClock::now() + TIMEOUT};
        char buffer[65536];
        while (true) {
            Sock::Event occurred{0};
            const auto remaining{std::chrono::duration_cast<std::chrono::milliseconds>(deadline - SteadyClock::now())};
            if (remaining <= 0ms || !sock->Wait(remaining, Sock::RecvEvent, &occurred)) throw std::runtime_error("timeout");
            if (!(occurred & Sock::RecvEvent)) continue;
            const ssize_t received{sock->Recv(buffer, sizeof(buffer), 0)};
            if (received < 0) {
                const int error{WSAGetLastError()};
                if (error == WSAEWOULDBLOCK || error == WSAEINTR || error == WSAEINPROGRESS) continue;
                throw std::runtime_error(NetworkErrorString(error));
            }
            if (received == 0) break;
            reply.append(buffer, received);
            if (reply.size() > MAX_REPLY_SIZE) throw std::runtime_error("reply too large");
        }
    } catch (const std::runtime_error& e) {
        throw unreachable(e.what());
    }

    const size_t header_end{reply.find("\r\n\r\n")};
    if (!reply.starts_with("HTTP/1.") || header_end == std::string::npos || reply.size() < 12) throw unreachable("not an HTTP reply");
    const std::string status{reply.substr(9, 3)};
    if (status == "401") throw MainClientError{"The mainchain node refused the credentials", /*rpc_error_in=*/false};
    if (status == "403") throw MainClientError{"The mainchain node does not allow connections from this address (rpcallowip)", /*rpc_error_in=*/false};

    UniValue answer;
    if (!answer.read(reply.substr(header_end + 4)) || !answer.isObject()) {
        throw unreachable(strprintf("unexpected reply with HTTP status %s", status));
    }
    const UniValue& error{answer.find_value("error")};
    if (!error.isNull()) {
        const UniValue& message{error.find_value("message")};
        const std::string text{message.isStr() ? message.get_str() : error.write()};
        throw MainClientError{strprintf("The mainchain node answered %s with an error: %s", method, text), /*rpc_error_in=*/true, text};
    }
    return answer.find_value("result");
}

} // namespace sidechain
