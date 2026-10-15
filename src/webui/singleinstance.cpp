// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/singleinstance.h>
#include <webui/chainconfig.h>

#include <crypto/sha256.h>
#include <logging.h>
#include <util/strencodings.h>
#include <tinyformat.h>
#include <util/threadnames.h>

#include <array>
#include <cstdlib>
#include <cstring>

#ifdef WIN32
#include <windows.h>
#include <sddl.h>
#include <vector>
#else
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace webui {

#ifndef WIN32
bool SingleInstance::PrivateDir(const fs::path& dir)
{
    // Not a link, a directory of this user that nobody else may enter: no other user can put a
    // socket in it, nor take one's place.
    struct stat st;
    if (::lstat(fs::PathToString(dir).c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode) && st.st_uid == ::geteuid() && (st.st_mode & 077) == 0;
}
#endif

fs::path SingleInstance::SocketPath(const fs::path& datadir_net)
{
    std::array<unsigned char, CSHA256::OUTPUT_SIZE> hash;
    const std::string dir{fs::PathToString(fs::absolute(datadir_net))};
    CSHA256().Write(reinterpret_cast<const unsigned char*>(dir.data()), dir.size()).Finalize(hash.data());
    const std::string name{std::string{chain::APP} + "-" + HexStr(std::span{hash}.first(10)) + ".sock"};
#ifdef WIN32
    return fs::PathFromString(name);
#else
    // A socket path is short (108 bytes on Linux): in the runtime directory of the user if it is
    // private, or else in a directory of this user's own in the temporary one, made private. Never
    // in a directory others can write to (the temporary one itself): another user could take the
    // socket's place, and get the URIs handed over, or send some. Nothing if neither is private: every
    // start then runs on its own (a second one for the data directory stops at its lock).
    if (const char* runtime{std::getenv("XDG_RUNTIME_DIR")}; runtime && *runtime && PrivateDir(fs::PathFromString(runtime))) {
        return fs::PathFromString(runtime) / fs::PathFromString(name);
    }
    std::error_code ec;
    const fs::path own{fs::temp_directory_path(ec) / fs::PathFromString(strprintf("%s-%u", std::string{chain::APP}, static_cast<unsigned>(::geteuid())))};
    if (ec) return {};
    ::mkdir(fs::PathToString(own).c_str(), 0700);
    if (!PrivateDir(own)) {
        LogWarning("webui: %s is not a private directory of this user: no single app per data directory", fs::PathToString(own));
        return {};
    }
    return own / fs::PathFromString(name);
#endif
}

#ifdef WIN32
// A named pipe of this user (its name from the socket path and the user, its access for the user
// alone), in place of the socket of Unix.
namespace {
//! The SID of the user of a process, as text ("S-1-5-21-...").
std::wstring UserSid(HANDLE process)
{
    HANDLE token{nullptr};
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return {};
    std::wstring out;
    DWORD size{0};
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        LPWSTR text{nullptr};
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
            out = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return out;
}

//! Whether the process at the other end of a pipe runs as this user.
bool SameUser(ULONG pid)
{
    HANDLE process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process) return false;
    const std::wstring sid{UserSid(process)};
    CloseHandle(process);
    return !sid.empty() && sid == UserSid(GetCurrentProcess());
}

std::wstring PipeName(const fs::path& socket)
{
    if (socket.empty()) return {};
    const std::wstring sid{UserSid(GetCurrentProcess())};
    const std::string key{fs::PathToString(socket) + "|" + std::string(sid.begin(), sid.end())};
    std::array<unsigned char, CSHA256::OUTPUT_SIZE> hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(key.data()), key.size()).Finalize(hash.data());
    // A SID is ASCII, and so is the name.
    const std::string name{"\\\\.\\pipe\\" + std::string{chain::APP} + "-" + HexStr(std::span{hash}.first(12))};
    return std::wstring(name.begin(), name.end());
}

HANDLE CreatePipe(const std::wstring& name, bool first)
{
    // Only this user may open it (and nobody may take its name while the app runs: the first instance).
    PSECURITY_DESCRIPTOR descriptor{nullptr};
    const std::wstring sddl{L"D:P(A;;GA;;;" + UserSid(GetCurrentProcess()) + L")"};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    HANDLE pipe{CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                 PIPE_UNLIMITED_INSTANCES, 0, static_cast<DWORD>(SingleInstance::MAX_MESSAGE), 0, &attributes)};
    LocalFree(descriptor);
    return pipe;
}

//! Wait for an overlapped operation; false when it did not end in time or the app stops.
bool WaitIo(HANDLE pipe, OVERLAPPED& ov, DWORD& bytes, const std::atomic<bool>& stop, int timeout_ms)
{
    for (int waited{0}; !stop; waited += 200) {
        if (timeout_ms >= 0 && waited >= timeout_ms) break;
        if (WaitForSingleObject(ov.hEvent, 200) == WAIT_OBJECT_0) return GetOverlappedResult(pipe, &ov, &bytes, FALSE);
    }
    CancelIo(pipe);
    GetOverlappedResult(pipe, &ov, &bytes, TRUE);
    return false;
}
} // namespace

bool SingleInstance::Send(const fs::path& socket, const std::vector<std::string>& lines)
{
    const std::wstring name{PipeName(socket)};
    if (name.empty()) return false;
    HANDLE pipe{INVALID_HANDLE_VALUE};
    for (int attempt{0}; attempt < 3; ++attempt) {
        pipe = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) break;
        WaitNamedPipeW(name.c_str(), 1000);
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;
    // Only to an app of this user; it may then come to the front.
    ULONG pid{0};
    bool sent{GetNamedPipeServerProcessId(pipe, &pid) && SameUser(pid)};
    if (sent) AllowSetForegroundWindow(pid);
    for (const std::string& line : lines) {
        if (!sent) break;
        // One message per line: a line break inside one would split it.
        if (line.find_first_of("\r\n") != std::string::npos || line.size() > MAX_MESSAGE) continue;
        const std::string data{line + "\n"};
        DWORD written{0};
        sent = WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    }
    if (sent) FlushFileBuffers(pipe);
    CloseHandle(pipe);
    return sent;
}

bool SingleInstance::Listen(const fs::path& socket, MessageFn on_message)
{
    const std::wstring name{PipeName(socket)};
    if (name.empty()) return false;
    HANDLE pipe{CreatePipe(name, /*first=*/true)};
    if (pipe == INVALID_HANDLE_VALUE) {
        // Another app listens there (or the name is taken).
        LogDebug(BCLog::RPC, "webui: cannot listen on the pipe of %s (%u)\n", fs::PathToString(socket), static_cast<unsigned>(GetLastError()));
        return false;
    }
    m_socket = socket;
    m_stop = false;
    m_thread = std::thread([this, pipe, on_message = std::move(on_message)] {
        util::ThreadRename("webui-instance");
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (!m_stop) {
            ResetEvent(ov.hEvent);
            DWORD bytes{0};
            bool connected{ConnectNamedPipe(pipe, &ov) != FALSE};
            if (!connected) {
                const DWORD error{GetLastError()};
                if (error == ERROR_PIPE_CONNECTED) connected = true;
                else if (error == ERROR_IO_PENDING) connected = WaitIo(pipe, ov, bytes, m_stop, -1);
            }
            if (!connected) {
                DisconnectNamedPipe(pipe);
                continue;
            }
            // Only from an app of this user: another one could otherwise hand payment URIs over.
            ULONG pid{0};
            std::string data;
            if (GetNamedPipeClientProcessId(pipe, &pid) && SameUser(pid)) {
                // A client has a second to say what it wants.
                const auto deadline{GetTickCount64() + 1000};
                char buffer[4096];
                while (!m_stop && data.size() <= MAX_MESSAGE * 4 && GetTickCount64() < deadline) {
                    ResetEvent(ov.hEvent);
                    DWORD n{0};
                    bool ok{ReadFile(pipe, buffer, sizeof(buffer), &n, &ov) != FALSE};
                    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WaitIo(pipe, ov, n, m_stop, static_cast<int>(deadline - GetTickCount64()));
                    if (!ok || n == 0) break;
                    data.append(buffer, n);
                }
            }
            DisconnectNamedPipe(pipe);
            size_t start{0};
            for (size_t end{data.find('\n')}; end != std::string::npos; start = end + 1, end = data.find('\n', start)) {
                const std::string line{data.substr(start, end - start)};
                if (!line.empty() && line.size() <= MAX_MESSAGE) on_message(line);
            }
        }
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
    });
    return true;
}

void SingleInstance::Stop()
{
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
}
#else
namespace {
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

int Accept(int fd)
{
#ifdef __linux__
    return ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
#else
    return ::accept(fd, nullptr, nullptr);
#endif
}

bool Address(const fs::path& socket, sockaddr_un& addr)
{
    if (socket.empty()) return false;
    const std::string path{fs::PathToString(socket)};
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

#ifdef MSG_NOSIGNAL
constexpr int SEND_FLAGS{MSG_NOSIGNAL};
#else
constexpr int SEND_FLAGS{0};
#endif

bool WriteAll(int fd, std::string_view data)
{
    while (!data.empty()) {
        const ssize_t n{::send(fd, data.data(), data.size(), SEND_FLAGS)};
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data.remove_prefix(n);
    }
    return true;
}
//! Whether the other end of a local connection runs as this user.
bool SameUser(int fd)
{
#if defined(SO_PEERCRED)
    struct ucred cred;
    socklen_t size{sizeof(cred)};
    return ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) == 0 && cred.uid == ::geteuid();
#else
    uid_t uid;
    gid_t gid;
    return ::getpeereid(fd, &uid, &gid) == 0 && uid == ::geteuid();
#endif
}

//! Whether a socket file is this user's (not one another user put there).
bool OwnSocket(const fs::path& socket)
{
    struct stat st;
    return ::lstat(fs::PathToString(socket).c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == ::geteuid();
}
} // namespace

bool SingleInstance::Send(const fs::path& socket, const std::vector<std::string>& lines)
{
    sockaddr_un addr;
    if (!Address(socket, addr) || !OwnSocket(socket)) return false;
    const int fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (fd < 0) return false;
    // Only to an app of this user.
    bool sent{::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && SameUser(fd)};
    for (const std::string& line : lines) {
        if (!sent) break;
        // One message per line: a line break inside one would split it.
        if (line.find_first_of("\r\n") != std::string::npos || line.size() > MAX_MESSAGE) continue;
        sent = WriteAll(fd, line + "\n");
    }
    ::close(fd);
    return sent;
}

bool SingleInstance::Listen(const fs::path& socket, MessageFn on_message)
{
    sockaddr_un addr;
    if (!Address(socket, addr)) {
        LogWarning("webui: the path of the instance socket is too long: %s", fs::PathToString(socket));
        return false;
    }
    // A socket left by an app that crashed: nobody answers on it.
    if (fs::exists(socket)) {
        if (Send(socket, {})) return false;
        std::error_code ec;
        fs::remove(socket, ec);
    }
    m_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_fd < 0) return false;
    if (::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(m_fd, 8) != 0) {
        LogWarning("webui: cannot listen on %s: %s", fs::PathToString(socket), std::strerror(errno));
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    // For this user only (its directory is already private).
    ::chmod(addr.sun_path, 0600);
    m_socket = socket;
    m_stop = false;
    m_thread = std::thread([this, on_message = std::move(on_message)] {
        util::ThreadRename("webui-instance");
        while (!m_stop) {
            pollfd p{m_fd, POLLIN, 0};
            if (::poll(&p, 1, 200) <= 0) continue;
            const int client{Accept(m_fd)};
            if (client < 0) continue;
            // Only from an app of this user: another one could otherwise hand payment URIs over.
            if (!SameUser(client)) {
                ::close(client);
                continue;
            }
            // A client has a second to say what it wants.
            timeval timeout{1, 0};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            std::string data;
            char buffer[4096];
            for (;;) {
                const ssize_t n{::recv(client, buffer, sizeof(buffer), 0)};
                if (n <= 0) break;
                data.append(buffer, n);
                if (data.size() > MAX_MESSAGE * 4) break;
            }
            ::close(client);
            size_t start{0};
            for (size_t end{data.find('\n')}; end != std::string::npos; start = end + 1, end = data.find('\n', start)) {
                const std::string line{data.substr(start, end - start)};
                if (!line.empty() && line.size() <= MAX_MESSAGE) on_message(line);
            }
        }
    });
    return true;
}

void SingleInstance::Stop()
{
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
        std::error_code ec;
        fs::remove(m_socket, ec);
    }
}
#endif

bool SingleInstance::SendToRunning(const fs::path& socket, const std::vector<std::string>& uris)
{
    std::vector<std::string> lines;
    for (const std::string& uri : uris) lines.push_back("uri " + uri);
    if (lines.empty()) lines.emplace_back("activate");
    return Send(socket, lines);
}

SingleInstance::~SingleInstance()
{
    Stop();
}

} // namespace webui
