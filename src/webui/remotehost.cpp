// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <webui/remotehost.h>

#include <chainparams.h>
#include <clientversion.h>
#include <common/args.h>
#include <common/url.h>
#include <crypto/sha256.h>
#include <httprpc.h>
#include <interfaces/node.h>
#include <logging.h>
#include <node/context.h>
#include <random.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <scheduler.h>
#include <support/cleanse.h>
#include <tinyformat.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <webui/bridge.h>
#include <webui/chainconfig.h>
#include <webui/confirm.h>
#include <webui/dialog.h>
#include <webui/events.h>
#include <webui/guard.h>
#include <webui/local.h>
#include <webui/nodeapi.h>
#include <webui/nodeevents.h>
#include <webui/rpcpolicy.h>
#if WEBUI_SIDECHAIN
#include <webui/sidechainapi.h>
#endif
#ifdef ENABLE_WALLET
#include <webui/walletapi.h>
#endif

#ifdef WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

namespace webui::remote {

UniValue DialogJson(const Window::ConfirmDialog& dialog)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("title", dialog.title);
    out.pushKV("message", dialog.message);
    out.pushKV("accept", dialog.accept);
    out.pushKV("cancel", dialog.cancel);
    out.pushKV("details", dialog.details);
    out.pushKV("level", std::string{dialog::LevelName(dialog.level)});
    out.pushKV("kind", dialog.kind);
    UniValue rows{UniValue::VARR};
    for (const dialog::Row& row : dialog.rows) {
        UniValue r{UniValue::VOBJ};
        r.pushKV("label", row.label);
        r.pushKV("value", row.value);
        r.pushKV("kind", std::string{dialog::RowKindName(row.kind)});
        rows.push_back(std::move(r));
    }
    out.pushKV("rows", std::move(rows));
    out.pushKV("passphrase", dialog.passphrase);
    out.pushKV("passphraseLabel", dialog.passphrase_label);
    out.pushKV("error", dialog.error);
    return out;
}

std::string Digest(const std::string& request, const Window::ConfirmDialog& dialog)
{
    const std::string question{DialogJson(dialog).write()};
    uint256 hash;
    // Each part with its length first: no two pairs of texts make the same bytes.
    const std::string bytes{strprintf("%u:%s%u:%s", request.size(), request, question.size(), question)};
    CSHA256().Write(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()).Finalize(hash.data());
    return HexStr(hash);
}

bool HasDuplicateKeys(const UniValue& value)
{
    if (value.isObject()) {
        std::set<std::string> seen;
        for (size_t i{0}; i < value.size(); ++i) {
            if (!seen.insert(value.getKeys()[i]).second) return true;
            if (HasDuplicateKeys(value.getValues()[i])) return true;
        }
    } else if (value.isArray()) {
        for (const UniValue& v : value.getValues()) {
            if (HasDuplicateKeys(v)) return true;
        }
    }
    return false;
}

std::string Bindings::CheckText(Check check)
{
    switch (check) {
    case Check::OK: return "ok";
    case Check::UNKNOWN: return "The confirmation is not known (used already, or never given): nothing was run.";
    case Check::EXPIRED: return "The confirmation came too late: nothing was run. Ask again.";
    case Check::REQUEST_CHANGED: return "The call is not the one that was confirmed: nothing was run.";
    case Check::QUESTION_CHANGED: return "What the call would do changed since it was confirmed: nothing was run. Ask again.";
    case Check::WALLET_CHANGED: return "The wallet is not the one the call was confirmed for: nothing was run.";
    }
    return "?";
}

std::string Bindings::Issue(const std::string& request, const std::string& digest, const std::string& wallet_id, std::chrono::steady_clock::time_point now)
{
    std::array<unsigned char, 32> bytes;
    GetStrongRandBytes(bytes);
    const std::string token{HexStr(bytes)};
    LOCK(m_mutex);
    std::erase_if(m_entries, [&](const auto& e) { return e.second.expires <= now; });
    while (m_entries.size() >= MAX_TOKENS) {
        const auto oldest{std::ranges::min_element(m_entries, {}, [](const auto& e) { return e.second.order; })};
        m_entries.erase(oldest);
    }
    m_entries.emplace(token, Entry{request, digest, wallet_id, now + TOKEN_EXPIRY, m_next++});
    return token;
}

Bindings::Check Bindings::Take(const std::string& token, const std::string& request, const std::string& digest, const std::string& wallet_id, std::chrono::steady_clock::time_point now)
{
    LOCK(m_mutex);
    const auto it{m_entries.find(token)};
    if (it == m_entries.end()) return Check::UNKNOWN;
    const Entry entry{it->second};
    m_entries.erase(it);
    if (entry.expires <= now) return Check::EXPIRED;
    if (entry.request != request) return Check::REQUEST_CHANGED;
    if (entry.wallet_id != wallet_id) return Check::WALLET_CHANGED;
    if (entry.digest != digest) return Check::QUESTION_CHANGED;
    return Check::OK;
}

size_t Bindings::Size() const
{
    LOCK(m_mutex);
    return m_entries.size();
}

std::string ChosenFiles::Problem(const std::string& path, const std::string& folder)
{
    if (path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos) return "not a path";
    const fs::path p{fs::PathFromString(path)};
    if (!p.is_absolute()) return "not an absolute path";
    std::error_code ec;
    const fs::path canonical{std::filesystem::weakly_canonical(p, ec)};
    if (ec) return "the path cannot be resolved";
    // As given, nothing resolved away: no "..", no ".", no symbolic link on the way, no doubled separator.
    if (fs::PathToString(canonical) != path) return "not a canonical path (a link, \"..\" or \".\" in it)";
    if (std::filesystem::is_symlink(p, ec)) return "a symbolic link";
    if (std::filesystem::exists(p, ec) && !std::filesystem::is_regular_file(p, ec)) return "not a file";
    if (!folder.empty()) {
        const fs::path base{std::filesystem::weakly_canonical(fs::PathFromString(folder), ec)};
        if (ec || p.parent_path() != base) return strprintf("not a file of the folder for the files of the app (-webuifiledir=%s)", folder);
    }
    return {};
}

#ifdef WIN32
struct ChosenFiles::Held {
    //! The file, opened as itself and without sharing its deletion (it cannot be renamed or replaced while held).
    HANDLE file{INVALID_HANDLE_VALUE};
    //! For a file to be made: its folder, held the same way.
    HANDLE folder{INVALID_HANDLE_VALUE};
    ~Held()
    {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (folder != INVALID_HANDLE_VALUE) CloseHandle(folder);
    }
};

namespace {
std::string WinError(DWORD error) { return strprintf("error %u", error); }

//! Opened as itself (a reparse point is opened, not followed); nullptr when there is nothing at the path.
HANDLE OpenAsItself(const fs::path& path, DWORD access, bool folder)
{
    const HANDLE handle{CreateFileW(path.wstring().c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | (folder ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr)};
    if (handle != INVALID_HANDLE_VALUE) return handle;
    const DWORD error{GetLastError()};
    if (error == ERROR_FILE_NOT_FOUND) return nullptr;
    if (error == ERROR_SHARING_VIOLATION) throw std::runtime_error("it is in use");
    throw std::runtime_error(strprintf("it cannot be opened (%s)", WinError(error)));
}

BY_HANDLE_FILE_INFORMATION InfoOf(HANDLE handle)
{
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info)) throw std::runtime_error("what it is cannot be read");
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("a symbolic link");
    return info;
}

ChosenFiles::Identity IdentityOf(const BY_HANDLE_FILE_INFORMATION& info, bool exists)
{
    return {exists, info.dwVolumeSerialNumber, (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
}

//! Opened and checked: a regular file (exists), or none in a folder that is one. The handle(s) in `held`.
ChosenFiles::Identity Open(const fs::path& path, bool write, ChosenFiles::Held* held)
{
    if (const HANDLE file{OpenAsItself(path, write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ, false)}) {
        auto close{[&] { if (!held) CloseHandle(file); }};
        try {
            const auto info{InfoOf(file)};
            if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) throw std::runtime_error("not a file");
            close();
            if (held) held->file = file;
            return IdentityOf(info, true);
        } catch (...) {
            CloseHandle(file);
            throw;
        }
    }
    const HANDLE folder{OpenAsItself(path.parent_path(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, true)};
    if (!folder) throw std::runtime_error("its folder is not there");
    try {
        const auto info{InfoOf(folder)};
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("its folder is not one");
        if (held) {
            held->folder = folder;
        } else {
            CloseHandle(folder);
        }
        return IdentityOf(info, false);
    } catch (...) {
        CloseHandle(folder);
        throw;
    }
}
} // namespace
#else
struct ChosenFiles::Held {
    //! The file, opened without following a link.
    int fd{-1};
    //! For a file to be made: its folder (the file is made in it by name, openat).
    int folder{-1};
    std::string name;
    ~Held()
    {
        if (fd >= 0) close(fd);
        if (folder >= 0) close(folder);
    }
};

namespace {
//! Opened as itself (O_NOFOLLOW: a link is refused, never followed); -1 when there is nothing at the path.
int OpenAsItself(const fs::path& path, int flags)
{
    const int fd{open(path.c_str(), flags | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
    if (fd >= 0) return fd;
    const int error{errno};
    if (error == ENOENT) return -1;
    if (error == ELOOP) throw std::runtime_error("a symbolic link");
    throw std::runtime_error(strprintf("it cannot be opened (%s)", std::strerror(error)));
}

struct stat StatOf(int fd)
{
    struct stat info;
    if (fstat(fd, &info) != 0) throw std::runtime_error("what it is cannot be read");
    return info;
}

ChosenFiles::Identity Open(const fs::path& path, bool write, ChosenFiles::Held* held)
{
    if (const int fd{OpenAsItself(path, write ? O_RDWR : O_RDONLY)}; fd >= 0) {
        struct stat info;
        try {
            info = StatOf(fd);
            if (!S_ISREG(info.st_mode)) throw std::runtime_error("not a file");
        } catch (...) {
            close(fd);
            throw;
        }
        if (held) {
            held->fd = fd;
        } else {
            close(fd);
        }
        return {true, static_cast<uint64_t>(info.st_dev), static_cast<uint64_t>(info.st_ino)};
    }
    const int folder{OpenAsItself(path.parent_path(), O_RDONLY | O_DIRECTORY)};
    if (folder < 0) throw std::runtime_error("its folder is not there");
    struct stat info;
    try {
        info = StatOf(folder);
        if (!S_ISDIR(info.st_mode)) throw std::runtime_error("its folder is not one");
    } catch (...) {
        close(folder);
        throw;
    }
    if (held) {
        held->folder = folder;
        held->name = fs::PathToString(path.filename());
    } else {
        close(folder);
    }
    return {false, static_cast<uint64_t>(info.st_dev), static_cast<uint64_t>(info.st_ino)};
}

void WriteAll(int fd, const char* data, size_t size, off_t offset)
{
    while (size > 0) {
        const ssize_t n{pwrite(fd, data, size, offset)};
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(strprintf("it cannot be written (%s)", std::strerror(errno)));
        }
        data += n;
        size -= static_cast<size_t>(n);
        offset += n;
    }
}
} // namespace
#endif

ChosenFiles::Identity ChosenFiles::Identify(const std::string& path)
{
    return Open(fs::PathFromString(path), /*write=*/false, nullptr);
}

std::string ChosenFiles::Changed(const File& file)
{
    if (!file.held) return "it was not opened when chosen";
    // The path as it was taken (no link on the way, nothing resolved away), then what it names, opened as itself.
    if (std::string problem{Problem(file.path)}; !problem.empty()) return problem;
    try {
#ifndef WIN32
        // What is held is still a name of the file (not unlinked since).
        if (file.held->fd >= 0 && StatOf(file.held->fd).st_nlink == 0) return "it is not there any more";
#endif
        const Identity now{Identify(file.path)};
        if (now == file.identity) return {};
        if (file.identity.exists && !now.exists) return "it is not there any more";
        if (!file.identity.exists && now.exists) return "a file was put there since";
        return "another file is there now";
    } catch (const std::runtime_error& e) {
        return e.what();
    }
}

void ChosenFiles::CopyOut(const File& file, const fs::path& copy)
{
    if (!file.held) throw std::runtime_error("it was not opened when chosen");
    std::ofstream out{static_cast<const std::filesystem::path&>(copy), std::ios::binary | std::ios::trunc};
    if (!out) throw std::runtime_error("a private copy cannot be made");
    std::vector<char> chunk(1 << 20);
#ifdef WIN32
    if (file.held->file == INVALID_HANDLE_VALUE) throw std::runtime_error("no such file");
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file.held->file, zero, nullptr, FILE_BEGIN)) throw std::runtime_error("it cannot be read");
    while (true) {
        DWORD n{0};
        if (!ReadFile(file.held->file, chunk.data(), static_cast<DWORD>(chunk.size()), &n, nullptr)) throw std::runtime_error("it cannot be read");
        if (n == 0) break;
        out.write(chunk.data(), n);
    }
#else
    if (file.held->fd < 0) throw std::runtime_error("no such file");
    off_t offset{0};
    while (true) {
        const ssize_t n{pread(file.held->fd, chunk.data(), chunk.size(), offset)};
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(strprintf("it cannot be read (%s)", std::strerror(errno)));
        }
        if (n == 0) break;
        out.write(chunk.data(), n);
        offset += n;
    }
#endif
    out.close();
    if (!out) throw std::runtime_error("a private copy cannot be made");
}

void ChosenFiles::CopyIn(const File& file, const fs::path& from)
{
    if (!file.held) throw std::runtime_error("it was not opened when chosen");
    if (!file.write) throw std::runtime_error("it was not chosen to be written");
    std::ifstream in{static_cast<const std::filesystem::path&>(from), std::ios::binary};
    if (!in) throw std::runtime_error("nothing was made to write");
    std::vector<char> chunk(1 << 20);
#ifdef WIN32
    HANDLE target{file.held->file};
    bool made{false};
    if (target == INVALID_HANDLE_VALUE) {
        // Made new, only where nothing is (not over a link put there, nor a file): the folder is held, not renamed.
        target = CreateFileW(fs::PathFromString(file.path).wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (target == INVALID_HANDLE_VALUE) throw std::runtime_error(strprintf("it cannot be made (%s)", WinError(GetLastError())));
        made = true;
    }
    auto done{[&] { if (made) CloseHandle(target); }};
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(target, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(target)) {
        done();
        throw std::runtime_error("it cannot be written");
    }
    while (in) {
        in.read(chunk.data(), chunk.size());
        const DWORD n{static_cast<DWORD>(in.gcount())};
        DWORD written{0};
        if (n > 0 && (!WriteFile(target, chunk.data(), n, &written, nullptr) || written != n)) {
            done();
            throw std::runtime_error("it cannot be written");
        }
    }
    FlushFileBuffers(target);
    done();
#else
    int target{file.held->fd};
    bool made{false};
    if (target < 0) {
        // Made in the folder held, by its name, only where nothing is: no link followed, no file put there replaced.
        target = openat(file.held->folder, file.held->name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (target < 0) throw std::runtime_error(strprintf("it cannot be made (%s)", std::strerror(errno)));
        made = true;
    }
    try {
        if (ftruncate(target, 0) != 0) throw std::runtime_error(strprintf("it cannot be written (%s)", std::strerror(errno)));
        off_t offset{0};
        while (in) {
            in.read(chunk.data(), chunk.size());
            const auto n{in.gcount()};
            if (n > 0) WriteAll(target, chunk.data(), static_cast<size_t>(n), offset);
            offset += n;
        }
        if (fsync(target) != 0) throw std::runtime_error(strprintf("it cannot be written (%s)", std::strerror(errno)));
    } catch (...) {
        if (made) close(target);
        throw;
    }
    if (made) close(target);
#endif
    if (in.bad()) throw std::runtime_error("what was made cannot be read");
}

std::string ChosenFiles::Grant(const std::string& path, bool write, bool overwrite, const std::string& user, std::chrono::steady_clock::time_point now, const std::string& folder)
{
    if (const std::string problem{Problem(path, folder)}; !problem.empty()) throw std::runtime_error(problem);
    // Opened now and kept: what is used later is that very file (or a new one made in that very folder).
    auto held{std::make_shared<Held>()};
    const Identity identity{Open(fs::PathFromString(path), write, held.get())};
    if (!write && !identity.exists) throw std::runtime_error("no such file");
    if (write && identity.exists && !overwrite) throw std::runtime_error("a file is there already (the chooser asks the user before one is replaced: overwrite)");
    std::array<unsigned char, 32> bytes;
    GetStrongRandBytes(bytes);
    const std::string token{HexStr(bytes)};
    LOCK(m_mutex);
    std::erase_if(m_entries, [&](const auto& e) { return e.second.expires <= now; });
    if (m_entries.size() >= MAX) {
        m_entries.erase(std::ranges::min_element(m_entries, {}, [](const auto& e) { return e.second.expires; }));
    }
    m_entries.emplace(token, Entry{File{path, write, write && identity.exists && overwrite, user, identity, std::move(held)}, now + EXPIRY});
    return token;
}

std::optional<ChosenFiles::File> ChosenFiles::Take(const std::string& token, const std::string& user, std::chrono::steady_clock::time_point now)
{
    LOCK(m_mutex);
    const auto it{m_entries.find(token)};
    if (it == m_entries.end()) return std::nullopt;
    Entry entry{std::move(it->second)};
    m_entries.erase(it);
    if (entry.expires <= now || entry.file.user != user) return std::nullopt;
    return std::move(entry.file);
}

void EventLog::Add(std::string batch)
{
    {
        LOCK(m_mutex);
        m_batches.emplace_back(++m_seq, std::move(batch));
        while (m_batches.size() > MAX_BATCHES) m_batches.pop_front();
    }
    m_cv.notify_all();
}

UniValue EventLog::After(uint64_t since, std::chrono::milliseconds timeout)
{
    WAIT_LOCK(m_mutex, lock);
    const auto deadline{std::chrono::steady_clock::now() + timeout};
    while (!m_stopped && m_seq <= since && std::chrono::steady_clock::now() < deadline) {
        m_cv.wait_until(lock, deadline);
    }
    UniValue out{UniValue::VOBJ};
    // Asked for after more than is kept (or a number from another run of the daemon): start again.
    const bool reset{since > m_seq || (!m_batches.empty() && since + 1 < m_batches.front().first)};
    UniValue batches{UniValue::VARR};
    for (const auto& [seq, batch] : m_batches) {
        if (seq > since || reset) batches.push_back(batch);
    }
    out.pushKV("seq", m_seq);
    out.pushKV("reset", reset);
    out.pushKV("batches", std::move(batches));
    return out;
}

void EventLog::Stop()
{
    {
        LOCK(m_mutex);
        m_stopped = true;
    }
    m_cv.notify_all();
}

Request ParseRequest(const UniValue& request)
{
    if (!request.isObject()) throw JSONRPCError(RPC_INVALID_PARAMETER, "request: {kind, method, params, wallet?}");
    if (HasDuplicateKeys(request)) throw JSONRPCError(RPC_INVALID_PARAMETER, "request: a key is given twice");
    for (const std::string& key : request.getKeys()) {
        if (key != "kind" && key != "method" && key != "params" && key != "wallet") throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("request: no field %s", key));
    }
    const UniValue& kind{request.find_value("kind")};
    const UniValue& method{request.find_value("method")};
    const UniValue& params{request.find_value("params")};
    const UniValue& wallet{request.find_value("wallet")};
    if (!kind.isStr() || (kind.get_str() != "rpc" && kind.get_str() != "host")) throw JSONRPCError(RPC_INVALID_PARAMETER, "request.kind: rpc or host");
    if (!method.isStr() || method.get_str().empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "request.method: a name");
    if (!params.isNull() && !params.isArray() && !params.isObject()) throw JSONRPCError(RPC_INVALID_PARAMETER, "request.params: an array or an object");
    if (!wallet.isNull() && !wallet.isStr()) throw JSONRPCError(RPC_INVALID_PARAMETER, "request.wallet: a name");
    Request out{kind.get_str(), method.get_str(), params.isNull() ? UniValue{kind.get_str() == "rpc" ? UniValue::VARR : UniValue::VOBJ} : params, wallet.isStr() ? wallet.get_str() : std::string{}};
    if (out.kind == "host") {
        // One wallet per request: the one the host method acts on, which it reads from its parameters (rpc.confirmed
        // too: its calls run in params.wallet). A wallet beside them is that one, or the request is refused.
        const UniValue& inner{out.params.isObject() ? out.params.find_value("wallet") : NullUniValue};
        if (!inner.isNull() && !inner.isStr()) throw JSONRPCError(RPC_INVALID_PARAMETER, "request.params.wallet: a name");
        const std::string acts_on{inner.isStr() ? inner.get_str() : std::string{}};
        if (wallet.isStr() && (inner.isNull() || out.wallet != acts_on)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "request.wallet: a host method acts on the wallet of its parameters (params.wallet); a request naming another is refused");
        }
        out.wallet = acts_on;
    }
    return out;
}

std::string DescribedWallet(const std::string& uri, const UniValue& arg)
{
    std::string of_uri;
    constexpr std::string_view prefix{"/wallet/"};
    if (uri.starts_with(prefix)) of_uri = UrlDecode(std::string_view{uri}.substr(prefix.size()));
    if (!arg.isStr()) return of_uri;
    if (!of_uri.empty() && of_uri != arg.get_str()) throw JSONRPCError(RPC_INVALID_PARAMETER, "wallet: the wallet named differs from the wallet of the URI");
    return arg.get_str();
}

const std::map<std::string, std::vector<std::string>>& HostPrivileges()
{
    static const std::map<std::string, std::vector<std::string>> table{
        // The host of the daemon: what the app reads first.
        {"app.hello", {"getnetworkinfo"}},
        {"app.info", {"getnetworkinfo"}},
        {"node.state", {"getblockchaininfo"}},
        // rpc.confirmed and console.run: each method they run is checked by name (CheckRpcCaller).
        {"rpc.confirmed", {}},
        {"rpc.confirmedMethods", {}},
        {"console.check", {}},
        {"console.run", {}},
        // NodeApi.
        {"peers.disconnect", {"disconnectnode"}},
        {"peers.ban", {"setban"}},
        {"bans.unban", {"setban"}},
        {"drivechain.depositAddress", {"getdepositaddress"}},
        {"tools.hashes", {}},
        {"tools.merkle", {}},
        {"tools.decode", {}},
        // A file is read only by a token the app's chooser gave (webuichosenfile: restorewallet).
        {"timestamp.hashFile", {}},
        {"timestamp.search", {"getblock"}},
        {"explorer.blocks", {"getblockheader"}},
        {"explorer.peg", {"getblock"}},
        // SidechainApi.
        {"peg.connection", {"getmainchaininfo"}},
        {"peg.call", {"getmainchaininfo"}},
        {"peg.deposit", {"createsidechaindeposit"}},
        {"bmm.set", {"setbmm"}},
        {"bmm.request", {"requestbmmblock"}},
        // WalletApi.
        {"wallets.list", {"listwallets"}},
        {"wallets.signers", {"enumeratesigners"}},
        {"wallets.create", {"createwallet"}},
        {"wallets.open", {"loadwallet"}},
        {"wallets.close", {"unloadwallet"}},
        {"wallets.restore", {"restorewallet"}},
        {"wallets.migrate", {"migratewallet"}},
        {"wallets.restoreMigrate", {"restorewallet", "migratewallet"}},
        {"wallets.isEncrypted", {"getwalletinfo"}},
        {"wallet.info", {"getwalletinfo"}},
        {"wallet.encrypt", {"encryptwallet"}},
        {"wallet.changePassphrase", {"walletpassphrasechange"}},
        {"wallet.unlock", {"walletpassphrase"}},
        {"wallet.lock", {"walletlock"}},
        {"wallet.backup", {"backupwallet"}},
        {"wallet.exportWatchOnly", {"exportwatchonlywallet"}},
        {"wallet.transactions", {"listtransactions"}},
        {"wallet.balanceHistory", {"listtransactions"}},
        {"wallet.transaction", {"gettransaction"}},
        {"wallet.txActions", {"gettransaction"}},
        {"wallet.abandon", {"abandontransaction"}},
        {"wallet.bump.prepare", {"bumpfee"}},
        {"wallet.bump.commit", {"bumpfee"}},
        {"wallet.bump.psbt", {"psbtbumpfee"}},
        {"wallet.addresses", {"listreceivedbyaddress"}},
        {"wallet.owned", {"listunspent"}},
        {"wallet.address", {"getaddressinfo"}},
        {"wallet.addressBook.add", {"setlabel"}},
        {"wallet.addressBook.edit", {"setlabel"}},
        {"wallet.addressBook.delete", {"setlabel"}},
        {"wallet.newAddress", {"getnewaddress"}},
        {"wallet.displayAddress", {"walletdisplayaddress"}},
        {"wallet.requests", {"listreceivedbyaddress"}},
        {"wallet.requests.add", {"getnewaddress"}},
        {"wallet.requests.remove", {"setlabel"}},
        {"wallet.coins", {"listunspent"}},
        {"wallet.coins.lock", {"lockunspent"}},
        {"wallet.coinControl.summary", {"listunspent"}},
        {"wallet.fee", {"estimatesmartfee"}},
        {"wallet.available", {"getbalances"}},
        {"wallet.checkChange", {"getaddressinfo"}},
        {"send.isDust", {}},
        {"wallet.send.prepare", {"send"}},
        {"wallet.send.commit", {"send"}},
        {"wallet.send.psbt", {"walletcreatefundedpsbt"}},
        {"wallet.send.discard", {"send"}},
        {"wallet.signMessage", {"signmessage"}},
        {"message.verify", {"verifymessage"}},
        {"psbt.decode", {"decodepsbt"}},
        {"wallet.psbt.open", {"decodepsbt"}},
        {"wallet.psbt.sign", {"walletprocesspsbt"}},
        {"psbt.broadcast", {"sendrawtransaction"}},
    };
    return table;
}

bool HostMethodAllowed(const std::string& method, const std::function<bool(const std::string&)>& allowed)
{
    if (allowed(method)) return true;
    const auto& table{HostPrivileges()};
    const auto it{table.find(method)};
    return it != table.end() && std::ranges::all_of(it->second, allowed);
}

namespace {

std::string PrivilegeText(const std::string& method)
{
    const auto& table{HostPrivileges()};
    const auto it{table.find(method)};
    if (it == table.end()) return "a method of the chain's own";
    std::string out;
    for (const std::string& m : it->second) out += (out.empty() ? "" : ", ") + m;
    return out.empty() ? "nothing" : out;
}

/** What a NeedConfirmation carries out of a host method, past its own handlers: not a std::exception, so none takes it. */
struct NeedConfirmation {
    UniValue description;
};

/** The request a thread runs for the app now: what its questions are bound to. */
struct Context {
    //! The request as text (kind, method, parameters, wallet).
    std::string request;
    std::string wallet;
    std::string token;
    SecureString passphrase;
    //! The files the user chose in the app's own chooser for this request (a backup, a file to hash): by path, whether
    //! to write and what the path named when chosen. Only from tokens of ChosenFiles, never a path given with the request.
    std::map<std::string, ChosenFiles::File> files;
    //! The host method it runs ("" for an RPC method).
    std::string method;
    //! The private files the host method reads (copies of chosen files) or writes (to be written into chosen files,
    //! by path of the chosen file), gone when the request is done.
    std::vector<fs::path> scratch;
    std::map<std::string, fs::path> to_write;
    bool describe_only{false};
    //! A question was answered yes in this request already: a second one is not asked (it would run the first again).
    bool accepted{false};
    ~Context()
    {
        for (const fs::path& p : scratch) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    }
};
thread_local Context* g_context{nullptr};

class ContextScope
{
public:
    explicit ContextScope(Context& context) : m_previous{g_context} { g_context = &context; }
    ~ContextScope() { g_context = m_previous; }
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;

private:
    Context* m_previous;
};

std::string NetworkLabel(const std::string& network)
{
    if (network == "main") return "";
    if (network == "test") return "testnet";
    return network;
}

class RemoteHost
{
public:
    explicit RemoteHost(node::NodeContext& context)
        : m_node{interfaces::MakeNode(context)},
          m_scheduler{context.scheduler.get()},
          m_bridge{[](const std::string&, int, const std::string&) {}, [](std::function<void()> fn) { fn(); }, 1, 1}
    {
        // Events: kept in order for the app, which asks for them (webuievents); delayed flushes on the node's scheduler.
        m_hub = std::make_unique<EventHub>(
            [this](const std::string& batch) { m_events.Add(batch); },
            [this](std::chrono::milliseconds delay, std::function<void()> fn) {
                if (m_scheduler) {
                    m_scheduler->scheduleFromNow(std::move(fn), std::max(delay, std::chrono::milliseconds{1}));
                } else {
                    fn();
                }
            });
        m_node_events = std::make_unique<NodeEvents>(*m_node, *m_hub);
        m_node_events->SubscribeNode();
        m_hub->SetReady(true);
        // The host's own folder for the private copies of chosen files: the daemon's alone, emptied at each start.
        if (context.args) {
            std::error_code ec;
            m_private = context.args->GetDataDirNet() / "webui-files";
            std::filesystem::remove_all(m_private, ec);
            std::filesystem::create_directories(m_private, ec);
            std::filesystem::permissions(m_private, std::filesystem::perms::owner_all, ec);
            if (ec) m_private.clear();
        }
        Register();
    }

    ~RemoteHost() { Stop(); }

    void Stop()
    {
        if (m_stopped.exchange(true)) return;
        m_events.Stop();
        if (m_node_events) m_node_events->Stop();
    }

    bool Stopped() const { return m_stopped; }

    UniValue Info() const
    {
        UniValue info{UniValue::VOBJ};
        const std::string network{ChainTypeToString(Params().GetChainType())};
        info.pushKV("name", CLIENT_NAME);
        info.pushKV("version", FormatFullVersion());
        info.pushKV("bugreport", CLIENT_BUGREPORT);
        info.pushKV("network", network);
        info.pushKV("networkLabel", NetworkLabel(network));
        info.pushKV("datadir", "");
        info.pushKV("blockSpacing", Params().GetConsensus().nPowTargetSpacing);
        info.pushKV("blockWeight", Params().GetConsensus().max_block_weight);
#ifdef ENABLE_WALLET
        const node::NodeContext* context{m_node->context()};
        info.pushKV("walletEnabled", !(context && context->args && context->args->GetBoolArg("-disablewallet", false)));
#else
        info.pushKV("walletEnabled", false);
#endif
        info.pushKV("devtools", false);
        info.pushKV("language", "");
        info.pushKV("test", false);
        info.pushKV("platform", "other");
        // Shown in the window of another app (the Chains app): no title bar, no window of its own.
        info.pushKV("embedded", true);
        info.pushKV("remote", true);
        // What this host answers (webuihost, describeconfirmation, webuievents, and the host methods it runs): the pack
        // of the pages the app shows must speak the same (app/scripts/pack.mjs HOST_API).
        info.pushKV("hostApi", HOST_API);
        return info;
    }

    /** One request of the app (webuihost). */
    UniValue Run(const UniValue& request, const std::string& token, SecureString passphrase, const UniValue& files, const std::string& auth_user)
    {
        if (m_stopped) throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
        Context context;
        const auto [kind, method, params, wallet] = ParseRequest(request);
        context.request = Canonical(kind, method, params, wallet);
        context.wallet = wallet;
        context.token = token;
        context.passphrase = std::move(passphrase);
        if (files.isArray()) {
            for (const UniValue& f : files.getValues()) {
                const auto file{f.isStr() ? m_files.Take(f.get_str(), auth_user, std::chrono::steady_clock::now()) : std::nullopt};
                if (!file) throw JSONRPCError(RPC_INVALID_PARAMETER, "files: a token of a file chosen in the app is needed (unknown, used or expired)");
                const auto [it, added]{context.files.emplace(file->path, *file)};
                if (!added) throw JSONRPCError(RPC_INVALID_PARAMETER, "files: one token for a file");
            }
        }
        if (kind == "host") context.method = method;
        const ContextScope scope{context};
        // What it runs by name, only what the caller may call itself (its -rpcwhitelist).
        const RpcCallerScope caller{CallerCheck(auth_user)};
        // The files chosen: read and written through what was opened when they were chosen.
        const ChosenFileScope chosen_files{[this](const std::string& path, bool write) { return UseFile(path, write); }};
        UniValue out{UniValue::VOBJ};
        try {
            UniValue result{kind == "rpc" ? CallRpc(method, params, wallet) : CallHost(method, params)};
            // What the host method wrote, now written into the files chosen.
            for (const auto& [path, from] : context.to_write) {
                try {
                    ChosenFiles::CopyIn(context.files.at(path), from);
                } catch (const std::runtime_error& e) {
                    throw HostError(BRIDGE_HOST_ERROR, strprintf("The file chosen cannot be written: %s", e.what()));
                }
            }
            out.pushKV("result", std::move(result));
        } catch (NeedConfirmation& need) {
            out.pushKV("confirm", std::move(need.description));
        } catch (const HostError& e) {
            throw JSONRPCError(static_cast<RPCErrorCode>(e.code), e.what());
        }
        return out;
    }

    /** describeconfirmation: the question of a confirmed RPC method, never run. */
    UniValue Describe(const std::string& method, const UniValue& params, const std::string& wallet, const std::string& auth_user)
    {
        if (m_stopped) throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
        const RpcCallerScope caller{CallerCheck(auth_user)};
        try {
            CheckRpcCaller(method);
        } catch (const HostError& e) {
            throw JSONRPCError(static_cast<RPCErrorCode>(e.code), e.what());
        }
        const auto& methods{confirm::Methods()};
        if (!methods.contains(method) && !KeyedWalletRpc(method)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s is not a method that is confirmed (confirm.cpp): it reads, or is not offered to the pages", method));
        }
        UniValue inner{UniValue::VOBJ};
        inner.pushKV("method", method);
        try {
            inner.pushKV("params", confirm::CanonicalParams(*m_node, method, params));
        } catch (const HostError& e) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, e.what());
        }
        if (!wallet.empty()) inner.pushKV("wallet", wallet);
        UniValue request{UniValue::VOBJ};
        request.pushKV("kind", "host");
        request.pushKV("method", "rpc.confirmed");
        request.pushKV("params", inner);

        Context context;
        context.request = Canonical("host", "rpc.confirmed", inner, wallet);
        context.wallet = wallet;
        context.describe_only = true;
        const ContextScope scope{context};
        const confirm::DescribeOnly describe_only;
        UniValue out{UniValue::VOBJ};
        try {
            CallHost("rpc.confirmed", inner);
            // Nothing was asked, and nothing ran (DescribeOnly): it needs no question.
            out.pushKV("needed", false);
        } catch (NeedConfirmation& need) {
            out = std::move(need.description);
            out.pushKV("needed", true);
        } catch (const HostError& e) {
            if (e.code != BRIDGE_AUTH_NEEDED) throw JSONRPCError(static_cast<RPCErrorCode>(e.code), e.what());
            out.pushKV("needed", false);
        }
        out.pushKV("request", std::move(request));
        return out;
    }

    UniValue Events(uint64_t since, std::chrono::milliseconds timeout) { return m_events.After(since, timeout); }

    Bindings& bindings() { return m_bindings; }

    std::string ChooseFile(const std::string& path, bool write, bool overwrite, const std::string& auth_user)
    {
        // Only the app's own chooser: its RPC user (-webuichooser), or one that may call everything; and only what that
        // user may do itself with a file.
        const ArgsManager* args{m_node->context() ? m_node->context()->args : nullptr};
        const std::string chooser{args ? args->GetArg("-webuichooser", "") : std::string{}};
        if (auth_user != chooser || chooser.empty()) {
            if (!RpcUserMayCallAll(auth_user)) {
                throw JSONRPCError(static_cast<RPCErrorCode>(BRIDGE_NOT_ALLOWED), "Only the app's own chooser gives files (-webuichooser): this RPC user may not");
            }
        }
        const std::string privilege{write ? "backupwallet" : "restorewallet"};
        if (!RpcUserMayCall(auth_user, privilege)) {
            throw JSONRPCError(static_cast<RPCErrorCode>(BRIDGE_NOT_ALLOWED), strprintf("This RPC user may not call %s (-rpcwhitelist): no file chosen to be %s", privilege, write ? "written" : "read"));
        }
        try {
            return m_files.Grant(path, write, overwrite, auth_user, std::chrono::steady_clock::now(), args ? args->GetArg("-webuifiledir", "") : std::string{});
        } catch (const std::runtime_error& e) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("This file cannot be used: %s", e.what()));
        }
    }

private:
    /** The RPC user's -rpcwhitelist, as the RPC server checks it (RpcUserMayCall). */
    static RpcCallerScope::AllowedFn CallerCheck(const std::string& auth_user)
    {
        return [auth_user](const std::string& method) { return RpcUserMayCall(auth_user, method); };
    }

    /** A private file of the host (in the data directory, the daemon's alone), new. */
    fs::path Scratch()
    {
        std::array<unsigned char, 16> bytes;
        GetStrongRandBytes(bytes);
        const fs::path p{m_private / fs::PathFromString(HexStr(bytes) + ".tmp")};
        if (g_context) g_context->scratch.push_back(p);
        return p;
    }

    /** Where a host method reads or writes a file chosen for this request (ChosenFileScope): never the path itself. */
    std::string UseFile(const std::string& path, bool write)
    {
        if (!g_context) throw HostError(BRIDGE_INVALID_REQUEST, "A file outside a request of the app");
        const auto it{g_context->files.find(path)};
        if (it == g_context->files.end() || (write && !it->second.write)) throw HostError(BRIDGE_INVALID_REQUEST, "Only a file chosen by the user can be used");
        const ChosenFiles::File& file{it->second};
        // The path still names what was opened when chosen: else the user would not find there what is written.
        if (const std::string changed{ChosenFiles::Changed(file)}; !changed.empty()) {
            LogInfo("webui remote: a chosen file was refused when used (%s)", changed);
            throw HostError(BRIDGE_INVALID_REQUEST, strprintf("The file chosen is not the one it was when chosen (%s): nothing was done. Choose it again.", changed));
        }
        if (m_private.empty()) throw HostError("No private folder for the files of the app");
        const fs::path scratch{Scratch()};
        if (write) {
            if (g_context->to_write.contains(path)) throw HostError(BRIDGE_INVALID_REQUEST, "A file chosen is written once per request");
            g_context->to_write.emplace(path, scratch);
            return fs::PathToString(scratch);
        }
        try {
            ChosenFiles::CopyOut(file, scratch);
        } catch (const std::runtime_error& e) {
            throw HostError(strprintf("The file chosen cannot be read: %s", e.what()));
        }
        return fs::PathToString(scratch);
    }

    static std::string Canonical(const std::string& kind, const std::string& method, const UniValue& params, const std::string& wallet)
    {
        UniValue c{UniValue::VARR};
        c.push_back(kind);
        c.push_back(method);
        c.push_back(params);
        c.push_back(wallet);
        return c.write();
    }

    /** Which wallet this is (its public descriptors), so that a token is not taken for another one of the same name. */
    std::string WalletId(const std::string& wallet)
    {
        if (wallet.empty()) return {};
        try {
            const UniValue descriptors{m_node->executeRpc("listdescriptors", UniValue{UniValue::VARR}, WalletUri(wallet))};
            const std::string text{descriptors.find_value("descriptors").write()};
            uint256 hash;
            CSHA256().Write(reinterpret_cast<const unsigned char*>(text.data()), text.size()).Finalize(hash.data());
            return HexStr(hash);
        } catch (...) {
            return "unknown:" + wallet;
        }
    }

    /** Every question of the host methods of this chain, asked of the app instead of a desktop. */
    dialog::Answer Ask(const Window::ConfirmDialog& dialog)
    {
        Context* context{g_context};
        if (!context) throw HostError(BRIDGE_AUTH_NEEDED, "A question outside a request of the app");
        // A passphrase that was wrong: said, not asked again in the same request (the app asks again).
        if (context->accepted && !dialog.error.empty()) throw HostError(-14, dialog.error);
        // A second question after a yes: never asked (answering it would run what the first allowed once more).
        if (context->accepted) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: one confirmation per call in the Chains app: run the commands one at a time", guard::OneLine(dialog.title, 80)));
        const std::string digest{Digest(context->request, dialog)};
        const auto now{std::chrono::steady_clock::now()};
        if (!context->token.empty() && !context->describe_only) {
            const Bindings::Check check{m_bindings.Take(context->token, context->request, digest, WalletId(context->wallet), now)};
            if (check != Bindings::Check::OK) {
                LogInfo("webui remote: a confirmation was refused (%s): %s", Bindings::CheckText(check), guard::OneLine(dialog.title, 80));
                throw HostError(CONFIRMATION_REFUSED, Bindings::CheckText(check));
            }
            context->accepted = true;
            dialog::Answer answer;
            answer.accepted = true;
            answer.passphrase = context->passphrase;
            LogInfo("webui remote: confirmed by the app: %s", guard::OneLine(dialog.title, 80));
            return answer;
        }
        if (dialog.rows.size() > dialog::MAX_ROWS) throw HostError(BRIDGE_INVALID_REQUEST, "Too much for one question");
        UniValue description{DialogJson(dialog)};
        description.pushKV("token", m_bindings.Issue(context->request, digest, WalletId(context->wallet), now));
        description.pushKV("digest", digest);
        description.pushKV("expiresIn", static_cast<int64_t>(TOKEN_EXPIRY.count()));
        description.pushKV("chain", CLIENT_NAME);
        description.pushKV("network", ChainTypeToString(Params().GetChainType()));
        throw NeedConfirmation{std::move(description)};
    }

    bool Confirm(const Window::ConfirmDialog& dialog) { return Ask(dialog).accepted; }

    UniValue CallRpc(const std::string& method, const UniValue& params, const std::string& wallet)
    {
        // The pages' own policy: what they read. What pays or signs is a host method (rpc.confirmed), asked.
        if (!PageRpcPolicy(method)) throw HostError(BRIDGE_METHOD_NOT_FOUND, strprintf("The interface does not call %s (use the console)", method));
        CheckRpcCaller(method);
        try {
            return m_node->executeRpc(method, params, wallet.empty() ? std::string{"/"} : WalletUri(wallet));
        } catch (const UniValue& error) {
            const UniValue& code{error.find_value("code")};
            const UniValue& message{error.find_value("message")};
            throw HostError(code.isNum() ? code.getInt<int>() : BRIDGE_HOST_ERROR, message.isStr() ? message.get_str() : error.write());
        }
    }

    UniValue CallHost(const std::string& method, const UniValue& params)
    {
        // Only what the caller may do itself: the host method named in its -rpcwhitelist, or what it exercises
        // (HostPrivileges); a host method not in that table only when named.
        if (!HostMethodAllowed(method, [](const std::string& m) {
                try {
                    CheckRpcCaller(m);
                    return true;
                } catch (const HostError&) {
                    return false;
                }
            })) {
            throw HostError(BRIDGE_NOT_ALLOWED, strprintf("This RPC user may not run %s (-rpcwhitelist: neither it nor what it does, %s, is listed)", method, PrivilegeText(method)));
        }
        // What a daemon can do for the pages: never the window, the app's settings, the desktop (the app does those).
        if (method == "app.hello") {
            UniValue out{UniValue::VOBJ};
            out.pushKV("phase", "ready");
            out.pushKV("phaseData", UniValue{UniValue::VOBJ});
            out.pushKV("dialogs", UniValue{UniValue::VARR});
            out.pushKV("info", Info());
            return out;
        }
        if (method == "app.info") return Info();
        if (method == "node.state") return m_node_events->Snapshot();
        const Bridge::HostFn fn{m_bridge.FindHost(method)};
        if (!fn) throw HostError(BRIDGE_METHOD_NOT_FOUND, strprintf("No host method %s in the daemon", method));
        try {
            return fn(params);
        } catch (const HostError&) {
            throw;
        } catch (const UniValue& error) {
            throw HostError(BRIDGE_HOST_ERROR, error.write());
        } catch (const std::exception& e) {
            throw HostError(BRIDGE_HOST_ERROR, e.what());
        }
    }

    void Register()
    {
        const auto node{[this]() -> interfaces::Node& {
            if (m_stopped) throw HostError(BRIDGE_STOPPED, "The node is shutting down");
            return *m_node;
        }};
        const guard::ConfirmFn confirm{[this](const Window::ConfirmDialog& dialog) { return Confirm(dialog); }};
        const guard::AskFn ask{[this](const Window::ConfirmDialog& dialog) { return Ask(dialog); }};
        // A file is the user's only when chosen in the app's own chooser for this request; one there already is replaced
        // only by a wallet backup the user agreed to replace it with (overwrite). Used through UseFile.
        const auto chosen{[](const std::string& path, bool write) {
            if (!g_context) return false;
            const auto it{g_context->files.find(path)};
            if (it == g_context->files.end() || (write && !it->second.write)) return false;
            if (write && it->second.identity.exists && !(it->second.overwrite && g_context->method == "wallet.backup")) return false;
            return true;
        }};
        m_node_api = std::make_unique<NodeApi>(node, [chosen](const std::string& path) { return chosen(path, false); }, confirm);
        m_node_api->Register(m_bridge);
#if WEBUI_SIDECHAIN
        m_sidechain_api = std::make_unique<SidechainApi>(node, confirm);
        m_sidechain_api->Register(m_bridge);
#endif
        confirm::KeysFn keys;
        confirm::FeeFn fee;
#ifdef ENABLE_WALLET
        m_wallet_api = std::make_unique<WalletApi>(node, chosen, ask);
        m_wallet_api->Register(m_bridge);
        keys = [this](const std::string& wallet, Window::ConfirmDialog dialog, bool always, bool fee_unknown, const std::function<UniValue()>& run) {
            return m_wallet_api->WithKeys(wallet, std::move(dialog), always, fee_unknown, run);
        };
        fee = [this](const std::string& wallet, int64_t vsize) { return m_wallet_api->FeeFor(wallet, vsize); };
#endif
        local::RegisterHost(m_bridge, node, confirm, [keys](const std::string& wallet, Window::ConfirmDialog dialog, bool always, bool fee_unknown, const std::function<UniValue()>& run) -> UniValue {
            if (keys) return keys(wallet, std::move(dialog), always, fee_unknown, run);
            throw HostError(-18, "Wallets are disabled in this node (-disablewallet).");
        });
        m_confirmed = std::make_unique<confirm::ConfirmedRpc>(node, ask, keys, fee);
        m_confirmed->Register(m_bridge);
    }

    std::unique_ptr<interfaces::Node> m_node;
    CScheduler* m_scheduler;
    Bridge m_bridge;
    EventLog m_events;
    std::unique_ptr<EventHub> m_hub;
    std::unique_ptr<NodeEvents> m_node_events;
    std::unique_ptr<NodeApi> m_node_api;
#if WEBUI_SIDECHAIN
    std::unique_ptr<SidechainApi> m_sidechain_api;
#endif
#ifdef ENABLE_WALLET
    std::unique_ptr<WalletApi> m_wallet_api;
#endif
    std::unique_ptr<confirm::ConfirmedRpc> m_confirmed;
    Bindings m_bindings;
    ChosenFiles m_files;
    fs::path m_private;
    std::atomic<bool> m_stopped{false};
};

Mutex g_host_mutex;
std::shared_ptr<RemoteHost> g_host GUARDED_BY(g_host_mutex);
bool g_interrupted GUARDED_BY(g_host_mutex){false};

std::shared_ptr<RemoteHost> GetHost(const JSONRPCRequest& request)
{
    node::NodeContext& context{EnsureAnyNodeContext(request.context)};
    LOCK(g_host_mutex);
    if (g_interrupted) throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
    if (!g_host) g_host = std::make_shared<RemoteHost>(context);
    return g_host;
}

const RPCResult DESCRIPTION_RESULT{RPCResult::Type::OBJ, "", "",
    {
        {RPCResult::Type::BOOL, "needed", "whether the call needs the user's word (false: it would run without a question)"},
        {RPCResult::Type::STR, "title", /*optional=*/true, "the title of the question"},
        {RPCResult::Type::STR, "message", /*optional=*/true, "what it does, in words"},
        {RPCResult::Type::STR, "accept", /*optional=*/true, "the word of the button that accepts"},
        {RPCResult::Type::STR, "cancel", /*optional=*/true, "the word of the button that declines (empty: Cancel)"},
        {RPCResult::Type::STR, "details", /*optional=*/true, "data shown apart, in a monospaced block"},
        {RPCResult::Type::STR, "level", /*optional=*/true, "info, warning or danger"},
        {RPCResult::Type::STR, "kind", /*optional=*/true, "what kind of question it is"},
        {RPCResult::Type::ARR, "rows", /*optional=*/true, "the facts, in order: each destination whole before its amount, the fee, the total",
            {{RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::STR, "label", ""},
                {RPCResult::Type::STR, "value", ""},
                {RPCResult::Type::STR, "kind", "text, address, amount, total, heading, note or image"},
            }}}},
        {RPCResult::Type::BOOL, "passphrase", /*optional=*/true, "whether the passphrase of the wallet is needed (it is locked)"},
        {RPCResult::Type::STR, "passphraseLabel", /*optional=*/true, "the label of the passphrase field"},
        {RPCResult::Type::STR, "error", /*optional=*/true, "an error shown at the passphrase field"},
        {RPCResult::Type::STR_HEX, "token", /*optional=*/true, "the one-time token of exactly this call and this question"},
        {RPCResult::Type::STR_HEX, "digest", /*optional=*/true, "SHA-256 of the call and the question the token is bound to"},
        {RPCResult::Type::NUM, "expiresIn", /*optional=*/true, "seconds the token lasts"},
        {RPCResult::Type::STR, "chain", /*optional=*/true, "the name of this chain"},
        {RPCResult::Type::STR, "network", /*optional=*/true, "its network"},
        {RPCResult::Type::OBJ, "request", "the request to send to webuihost with the token, exactly", {}, RPCResultOptions{.skip_type_check = true}},
    }};

RPCMethod describeconfirmation()
{
    return RPCMethod{
        "describeconfirmation",
        "What a call that pays, signs or changes what the node does would do, as the user is to be asked about it, "
        "described by this chain's own describers (the questions of its app). Nothing is run. To run it, send the request "
        "of the answer to webuihost with its token: only exactly that call, from that wallet, once, within the time.\n",
        {
            {"method", RPCArg::Type::STR, RPCArg::Optional::NO, "the RPC method"},
            {"params", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "its parameters, in order", {{"param", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""}}},
            {"wallet", RPCArg::Type::STR, RPCArg::DefaultHint{"the wallet of the URI"}, "the wallet it runs in"},
        },
        DESCRIPTION_RESULT,
        RPCExamples{HelpExampleCli("-rpcwallet=main describeconfirmation", "signmessage '[\"<address>\", \"hello\"]'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            const auto host{GetHost(request)};
            const UniValue params{request.params[1].isNull() ? UniValue{UniValue::VARR} : request.params[1]};
            if (HasDuplicateKeys(params)) throw JSONRPCError(RPC_INVALID_PARAMETER, "params: a key is given twice");
            return host->Describe(request.params[0].get_str(), params, DescribedWallet(request.URI, request.params[2]), request.authUser);
        }};
}

RPCMethod webuihost()
{
    return RPCMethod{
        "webuihost",
        "One request of the pages of this chain, from the app that shows them (the Chains app): a host method of the pages "
        "(wallet.*, rpc.confirmed, the chain's own) or an RPC method they read (the pages' own policy). A call that needs the "
        "user's word answers {confirm: <question>} with a one-time token; sent again, exactly, with the token (and the "
        "passphrase the user typed for a locked wallet), it runs.\n",
        {
            {"request", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::NO, "{kind: \"host\"|\"rpc\", method, params, wallet?}", {{"kind", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""}}},
            {"token", RPCArg::Type::STR, RPCArg::Default{""}, "the token of the question the user accepted"},
            {"passphrase", RPCArg::Type::STR, RPCArg::Default{""}, "the passphrase of the locked wallet, typed in the question"},
            {"files", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "tokens (webuichosenfile) of the files the user chose for this request in the app's own chooser: never paths", {{"token", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""}}},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::ANY, "result", /*optional=*/true, "what the request answered"},
            {RPCResult::Type::OBJ, "confirm", /*optional=*/true, "the question to ask the user (as describeconfirmation, without needed and request)", {}, RPCResultOptions{.skip_type_check = true}},
        }},
        RPCExamples{""},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            const auto host{GetHost(request)};
            SecureString passphrase;
            if (request.params[2].isStr()) {
                passphrase.assign(request.params[2].get_str().begin(), request.params[2].get_str().end());
            }
            return host->Run(request.params[0], request.params[1].isStr() ? request.params[1].get_str() : std::string{}, std::move(passphrase), request.params[3], request.authUser);
        }};
}

RPCMethod webuievents()
{
    return RPCMethod{
        "webuievents",
        "The events of the node for the pages of this chain (its tip, its wallets, their balances), after the number `since`: "
        "batches of events as JSON text, waited for at most `timeout` milliseconds when there are none.\n",
        {
            {"since", RPCArg::Type::NUM, RPCArg::Default{0}, "the number of the last batch received"},
            {"timeout", RPCArg::Type::NUM, RPCArg::Default{0}, "milliseconds to wait for one (at most 30000)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::NUM, "seq", "the number of the last batch"},
            {RPCResult::Type::BOOL, "reset", "whether batches were missed (older than are kept): all those kept are given"},
            {RPCResult::Type::ARR, "batches", "", {{RPCResult::Type::STR, "", "a JSON array of events {type, data}"}}},
        }},
        RPCExamples{HelpExampleCli("webuievents", "0 10000")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            const auto host{GetHost(request)};
            const int64_t since{request.params[0].isNull() ? 0 : request.params[0].getInt<int64_t>()};
            const int64_t timeout{request.params[1].isNull() ? 0 : std::clamp<int64_t>(request.params[1].getInt<int64_t>(), 0, 30000)};
            return host->Events(static_cast<uint64_t>(std::max<int64_t>(0, since)), std::chrono::milliseconds{timeout});
        }};
}

RPCMethod webuichosenfile()
{
    return RPCMethod{
        "webuichosenfile",
        "A file the user just chose in the native chooser of the app that shows this chain's pages: a one-time token for it, "
        "valid 2 minutes, that a request of the pages names (webuihost files) to read or write it. Only an absolute, "
        "canonical path (no link, no \"..\") is taken, and when a request uses it, it must still name the same file "
        "(or still none, in the same folder): it is opened now, without following a link, and read or written through what was "
        "opened. Only the app's own RPC user (-webuichooser) or one that may call everything gives files, and only one that may "
        "call backupwallet (to write) or restorewallet (to read); the token is that user's. A file there already is replaced only "
        "with overwrite, by a wallet backup. The app calls it with what its own chooser answered, never with a path of the pages.\n",
        {
            {"path", RPCArg::Type::STR, RPCArg::Optional::NO, "the file chosen"},
            {"write", RPCArg::Type::BOOL, RPCArg::Default{false}, "chosen to be written (a backup)"},
            {"overwrite", RPCArg::Type::BOOL, RPCArg::Default{false}, "the user agreed to replace the file there (a wallet backup only)"},
        },
        RPCResult{RPCResult::Type::STR_HEX, "", "the token"},
        RPCExamples{""},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            return GetHost(request)->ChooseFile(request.params[0].get_str(), !request.params[1].isNull() && request.params[1].get_bool(),
                                                !request.params[2].isNull() && request.params[2].get_bool(), request.authUser);
        }};
}

} // namespace

void AddArgs(ArgsManager& args)
{
    args.AddArg("-webuichooser=<user>", "The RPC user of the app that shows this chain's pages (its -rpcauth user): the only one, besides those that may call everything, that gives files the user chose (webuichosenfile)", ArgsManager::ALLOW_ANY | ArgsManager::SENSITIVE, OptionsCategory::RPC);
    args.AddArg("-webuifiledir=<dir>", "Only files of this folder are given (webuichosenfile)", ArgsManager::ALLOW_ANY, OptionsCategory::RPC);
}

void RegisterRpcCommands(CRPCTable& table)
{
    static const CRPCCommand commands[]{
        {"webui", &webuichosenfile},
        {"webui", &describeconfirmation},
        {"webui", &webuihost},
        {"webui", &webuievents},
    };
    for (const auto& c : commands) table.appendCommand(c.name, &c);
}

void Interrupt()
{
    std::shared_ptr<RemoteHost> host;
    {
        LOCK(g_host_mutex);
        g_interrupted = true;
        host = g_host;
    }
    // Stopped, not destroyed: what it scheduled on the node's scheduler may still run until the node stops that (in
    // Shutdown), and finds it stopped. It goes in Shutdown, once the scheduler stopped.
    if (host) host->Stop();
}

void Shutdown()
{
    std::shared_ptr<RemoteHost> host;
    {
        LOCK(g_host_mutex);
        g_interrupted = true;
        host = std::move(g_host);
    }
    if (host) host->Stop();
    // The RPC server stopped before (its calls are done): this was the last of it.
}

} // namespace webui::remote
