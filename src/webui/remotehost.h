// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_REMOTEHOST_H
#define BITCOIN_WEBUI_REMOTEHOST_H

#include <support/allocators/secure.h>
#include <sync.h>
#include <univalue.h>
#include <webui/window.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>

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
 * What the path named when it was chosen is kept (Identity: the file, opened without following a link, by its device
 * and inode; or, for a file to be made, its folder): when a request uses it, the path is opened again the same way and
 * must name the very same file (or still none, in the same folder). A link or another file put there in between is
 * refused, never followed.
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
    struct File {
        std::string path;
        bool write{false};
        Identity identity;
    };
    static constexpr std::chrono::seconds EXPIRY{120};
    static constexpr size_t MAX{32};
    /** Why a path cannot be taken ("" when it can). */
    static std::string Problem(const std::string& path);
    /**
     * What the path names now, opened without following a link (O_NOFOLLOW; on Windows, the reparse point itself): a
     * regular file, or none in a folder that is one. Throws std::runtime_error with what it is else.
     */
    static Identity Identify(const std::string& path);
    /** Why a file chosen cannot be used now ("" when the path still names what it named when chosen). */
    static std::string Changed(const File& file);
    /** A token for a chosen file. Throws std::runtime_error with the Problem. */
    std::string Grant(const std::string& path, bool write, std::chrono::steady_clock::time_point now) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** The file of a token, once (gone after); nullopt for a token unknown, used or expired. */
    std::optional<File> Take(const std::string& token, std::chrono::steady_clock::time_point now) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    struct Entry {
        File file;
        std::chrono::steady_clock::time_point expires;
    };
    Mutex m_mutex;
    std::map<std::string, Entry> m_entries GUARDED_BY(m_mutex);
};

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

/** Add webuihost, describeconfirmation, webuievents and webuichosenfile to the table (rpc_extra::g_register of the daemon). */
void RegisterRpcCommands(CRPCTable& table);
/** The node is shutting down: the host stops (the waits return, its handlers go). */
void Interrupt();
/** The node's scheduler stopped: the host goes (rpc_extra::g_shutdown), before the parts of the node it used. */
void Shutdown();

} // namespace webui::remote

#endif // BITCOIN_WEBUI_REMOTEHOST_H
