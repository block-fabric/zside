// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_REMOTEHOST_H
#define BITCOIN_WEBUI_REMOTEHOST_H

#include <support/allocators/secure.h>
#include <sync.h>
#include <univalue.h>
#include <util/fs.h>
#include <webui/window.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ArgsManager;
class CRPCTable;

/**
 * The host of this chain's pages, over RPC, in the daemon (<stem>d): what the Chains app needs to show the pages of
 * this sidechain in its own window (doc/unified-app.md of Chains, phase 2). The app runs the daemon; its pages call
 * the host methods of this chain (wallet.*, rpc.confirmed, the chain's own of local.cpp...) and its RPC methods (under
 * the page's policy, rpcpolicy.cpp) through three RPC methods:
 *
 *   webuihost {kind, method, params, wallet?} ( "token" "passphrase" )
 *       Runs one request of the page, as the app's own bridge would. What needs the user's word (a spend, a signature,
 *       the keys of a locked wallet: the questions of confirm.cpp and walletapi.cpp) is not asked here (a daemon has no
 *       desktop): the answer is {"confirm": <description>} with the question as the describer made it and a one-time
 *       token. The app shows it in its own trusted dialog; accepted, it sends the very same request again with the
 *       token (and the passphrase typed in that dialog, for a locked wallet): the question is made again, and runs only
 *       if the token is that of exactly this request, this question, this wallet, not used, not expired (Bindings).
 *   describeconfirmation "method" [params] ( "wallet" )
 *       What a confirmed RPC method (confirm::Methods) would do, described, never run: the same description and token,
 *       and the request (rpc.confirmed, the parameters canonical) to send to webuihost with it.
 *   webuievents ( since timeout )
 *       The events of the node for the pages (tip, wallets, balances), in batches, waited for at most `timeout` ms.
 *
 * Only a daemon has them (bitcoind.cpp, ENABLE_WEBUI_REMOTE); the app of a chain has its own host. The object is made
 * at the first call and goes when the node is interrupted.
 */
namespace webui::remote {

/** The version of what this host answers: app/scripts/pack.mjs HOST_API says the same. */
inline constexpr int HOST_API{1};
/** How long a token lasts: the user reads the question in that time (asked again after). */
inline constexpr std::chrono::seconds TOKEN_EXPIRY{300};
/** At most so many tokens are kept (the oldest goes first). */
inline constexpr size_t MAX_TOKENS{64};
/** The error of a token that does not fit (unknown, used, expired, another request, another question, another wallet). */
inline constexpr int CONFIRMATION_REFUSED{-32802};

/** The question as JSON: {title, message, accept, cancel, details, level, kind, rows: [{label, value, kind}], passphrase, passphraseLabel, error}. */
UniValue DialogJson(const Window::ConfirmDialog& dialog);

/** What a token binds, as text: the request (its kind, method, parameters, wallet as JSON) and the question (DialogJson). SHA-256, hex. */
std::string Digest(const std::string& request, const Window::ConfirmDialog& dialog);

/** Whether an object or array, at any depth, names a key twice (refused: two readers could take two values). */
bool HasDuplicateKeys(const UniValue& value);

/**
 * The tokens of the questions described and not answered yet: each bound to one request (as text), one question (its
 * digest) and one wallet (its identity: what its public descriptors are, so that another wallet loaded under the same
 * name does not fit), for TOKEN_EXPIRY, used once.
 */
class Bindings
{
public:
    enum class Check { OK, UNKNOWN, EXPIRED, REQUEST_CHANGED, QUESTION_CHANGED, WALLET_CHANGED };
    static std::string CheckText(Check check);

    /** A new token for this request and question. */
    std::string Issue(const std::string& request, const std::string& digest, const std::string& wallet_id, std::chrono::steady_clock::time_point now) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Take a token: gone whatever the answer (a token that does not fit is not tried again). */
    Check Take(const std::string& token, const std::string& request, const std::string& digest, const std::string& wallet_id, std::chrono::steady_clock::time_point now) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    size_t Size() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    struct Entry {
        std::string request;
        std::string digest;
        std::string wallet_id;
        std::chrono::steady_clock::time_point expires;
        uint64_t order;
    };
    mutable Mutex m_mutex;
    std::map<std::string, Entry> m_entries GUARDED_BY(m_mutex);
    uint64_t m_next GUARDED_BY(m_mutex){0};
};

/**
 * The events for the pages, kept in order with a number each (the app asks for those after the last it got): at most
 * MAX_BATCHES batches; one who asks for older ones than are kept is told to start again ("reset").
 */
class EventLog
{
public:
    static constexpr size_t MAX_BATCHES{512};
    void Add(std::string batch) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** The batches after `since`, waited for at most `timeout` when there are none: {seq, batches: [json text...], reset}. */
    UniValue After(uint64_t since, std::chrono::milliseconds timeout) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    Mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::pair<uint64_t, std::string>> m_batches GUARDED_BY(m_mutex);
    uint64_t m_seq GUARDED_BY(m_mutex){0};
    bool m_stopped GUARDED_BY(m_mutex){false};
};

/**
 * The files the user chose in the app's own chooser (a wallet backup to write, a file to hash): the app, right after its
 * native chooser answered, gives the path here (webuichosenfile) and gets a one-time token for it; a request of the pages
 * then names the token (webuihost files), never a path. A path is taken only absolute and already canonical (no "..",
 * no symbolic link on the way, no relative part): what the user saw chosen is what is opened.
 *
 * Only the app's own chooser grants: the RPC user named by -webuichooser (the app's own, an -rpcauth of its run), or
 * one that may call everything; and only what that user may do itself (backupwallet to write, restorewallet to read).
 * A token is the granting user's alone. A file there already is written only when the grant says so (overwrite: the
 * chooser asked the user) and only by a wallet backup. With -webuifiledir, only files in that folder.
 *
 * The file is opened when chosen and kept open (O_NOFOLLOW; on Windows, the reparse point itself, without sharing its
 * deletion: it cannot be renamed or replaced meanwhile); for a file to be made, its folder. Used, it is read through
 * that descriptor (into a private copy the host method reads), or written through it (from a private file the host
 * method wrote): made in that very folder (openat, O_CREAT|O_EXCL|O_NOFOLLOW), never by opening the path again. Before,
 * the path must still name what is held (else the user would not find it there): a link or another file put there
 * in between is refused.
 */
class ChosenFiles
{
public:
    /** What a path named when it was chosen: a regular file (exists) or, when there was none yet, its folder. */
    struct Identity {
        bool exists{false};
        uint64_t device{0};
        uint64_t inode{0};
        bool operator==(const Identity&) const = default;
    };
    /** What is kept open of a chosen file (remotehost.cpp). */
    struct Held;
    struct File {
        std::string path;
        bool write{false};
        //! The user agreed to replace the file there (a backup over an older one).
        bool overwrite{false};
        //! The RPC user who granted it: only that user's requests use it.
        std::string user;
        Identity identity;
        std::shared_ptr<Held> held;
    };
    static constexpr std::chrono::seconds EXPIRY{120};
    static constexpr size_t MAX{32};
    /** Why a path cannot be taken ("" when it can); with a folder (-webuifiledir), only a path in it. */
    static std::string Problem(const std::string& path, const std::string& folder = {});
    /** What the path names now, opened without following a link. Throws std::runtime_error with what it is else. */
    static Identity Identify(const std::string& path);
    /** Why the path no longer names what is held of a chosen file ("" when it does). */
    static std::string Changed(const File& file);
    /** A private copy of a chosen file, read through what was opened when chosen. Throws std::runtime_error. */
    static void CopyOut(const File& file, const fs::path& copy);
    /** Write a chosen file from a private one, through what was opened when chosen. Throws std::runtime_error. */
    static void CopyIn(const File& file, const fs::path& from);
    /**
     * A token for a chosen file, of the RPC user `user`. Throws std::runtime_error with the Problem, or when there is
     * a file there to write and `overwrite` is not given.
     */
    std::string Grant(const std::string& path, bool write, bool overwrite, const std::string& user, std::chrono::steady_clock::time_point now,
                      const std::string& folder = {}) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** The file of a token for `user`, once (gone after); nullopt for a token unknown, used, expired or of another user. */
    std::optional<File> Take(const std::string& token, const std::string& user, std::chrono::steady_clock::time_point now) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    struct Entry {
        File file;
        std::chrono::steady_clock::time_point expires;
    };
    Mutex m_mutex;
    std::map<std::string, Entry> m_entries GUARDED_BY(m_mutex);
};

/**
 * The RPC methods whose privilege each host method of the kit exercises (what a user must be allowed to call for it to
 * run through webuihost): a host method runs for a caller with a -rpcwhitelist only when the whitelist names the
 * host method itself, or every method of its entry here. One without an entry (a chain's own, local.cpp) only when
 * named. An empty entry: nothing beyond webuihost (it reads nothing of the node, or what it runs is checked by name).
 */
const std::map<std::string, std::vector<std::string>>& HostPrivileges();
/** Whether `allowed` (what a user may call) lets that user run a host method (HostPrivileges). */
bool HostMethodAllowed(const std::string& method, const std::function<bool(const std::string&)>& allowed);

/** A request of the pages (webuihost), read: one wallet for it, the one its method uses (ParseRequest). */
struct Request {
    std::string kind;
    std::string method;
    UniValue params;
    std::string wallet;
};
/**
 * Read a request {kind, method, params, wallet?}. Throws a JSON-RPC error when it is not one. Its wallet is the one the
 * method acts on: for an RPC method, `wallet` (the URI it runs at); for a host method, the wallet of its parameters
 * (params.wallet, as the host methods and rpc.confirmed read it), and a `wallet` beside it must be that same one
 * (refused when it differs, or names one where the parameters name none): a token is never bound to one wallet for
 * a call that acts on another.
 */
Request ParseRequest(const UniValue& request);

/** The wallet of describeconfirmation: its argument, or the wallet of the URI (/wallet/<name>); both must agree. */
std::string DescribedWallet(const std::string& uri, const UniValue& arg);

/** The options of the host over RPC (-webuichooser, -webuifiledir). */
void AddArgs(ArgsManager& args);
/** Add webuihost, describeconfirmation, webuievents and webuichosenfile to the table (rpc_extra::g_register of the daemon). */
void RegisterRpcCommands(CRPCTable& table);
/** The node is shutting down: the host stops (the waits return, its handlers go). */
void Interrupt();
/** The node's scheduler stopped: the host goes (rpc_extra::g_shutdown), before the parts of the node it used. */
void Shutdown();

} // namespace webui::remote

#endif // BITCOIN_WEBUI_REMOTEHOST_H
