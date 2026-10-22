// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_BRIDGE_H
#define BITCOIN_WEBUI_BRIDGE_H

#include <sync.h>
#include <univalue.h>
#include <util/threadpool.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace webui {

//! Error codes the bridge answers with, besides the codes of the RPC methods themselves.
enum BridgeError : int {
    //! The request was cancelled by the page.
    BRIDGE_CANCELLED = -32800,
    //! What was asked needs the user's word first, and the page asked for nothing to be asked (rpc.confirmed authorizedOnly).
    BRIDGE_AUTH_NEEDED = -32801,
    //! The request is not a bridge request (bad JSON, missing fields, unknown kind).
    BRIDGE_INVALID_REQUEST = -32600,
    //! No host method of that name.
    BRIDGE_METHOD_NOT_FOUND = -32601,
    //! The node is not ready for RPC calls yet (still starting).
    BRIDGE_NOT_READY = -28,
    //! The app is shutting down: no more calls.
    BRIDGE_STOPPED = -9,
    //! A host method failed.
    BRIDGE_HOST_ERROR = -1,
    //! The caller may not run that RPC method: the RPC user's -rpcwhitelist does not list it (the host over RPC, remotehost.h).
    BRIDGE_NOT_ALLOWED = -32803,
};

//! The key of the result of a request asked {exact: true}: {"$rawjson": "<the result as JSON text>"} (Bridge::Request::exact).
inline constexpr std::string_view EXACT_KEY{"$rawjson"};

//! What a host method throws to answer with an error of its own.
struct HostError : std::runtime_error {
    int code;
    HostError(int code_in, const std::string& message) : std::runtime_error(message), code(code_in) {}
    explicit HostError(const std::string& message) : HostError(BRIDGE_HOST_ERROR, message) {}
};

/**
 * Who runs the request of this thread may name these RPC methods: in the daemon's host over RPC (remotehost.h), what the
 * RPC user who called webuihost or describeconfirmation may call itself (its -rpcwhitelist), so that a method run
 * inside them by name (an RPC method of the pages, the calls of rpc.confirmed, the commands of console.run) is never
 * one that user could not call directly. Outside a scope (the app's own window): any.
 */
class RpcCallerScope
{
public:
    using AllowedFn = std::function<bool(const std::string& method)>;
    explicit RpcCallerScope(AllowedFn allowed);
    ~RpcCallerScope();
    RpcCallerScope(const RpcCallerScope&) = delete;
    RpcCallerScope& operator=(const RpcCallerScope&) = delete;

private:
    AllowedFn m_allowed;
    const AllowedFn* m_previous;
};
/** Throws HostError(BRIDGE_NOT_ALLOWED) when the caller of this thread's request may not run `method` (RpcCallerScope). */
void CheckRpcCaller(const std::string& method);

/**
 * Where a host method reads or writes a file the user chose (after its own check that it was chosen): in the app's own
 * window, the path itself; in the daemon's host over RPC (remotehost.h), a private copy made through the file opened
 * when it was chosen, or a private file written back through what was opened when it was chosen once the request is
 * done, never the path opened again.
 */
class ChosenFileScope
{
public:
    using UseFn = std::function<std::string(const std::string& path, bool write)>;
    explicit ChosenFileScope(UseFn use);
    ~ChosenFileScope();
    ChosenFileScope(const ChosenFileScope&) = delete;
    ChosenFileScope& operator=(const ChosenFileScope&) = delete;

private:
    UseFn m_use;
    const UseFn* m_previous;
};
/** The path to read (write: to write) for a file the user chose at `path` (ChosenFileScope); the path itself outside one. */
std::string UseChosenFile(const std::string& path, bool write);

/** The URI path of a wallet for the RPC dispatcher: "/wallet/<percent-encoded name>". */
std::string WalletUri(const std::string& wallet_name);

/** Percent-encode everything but the unreserved characters of RFC 3986. */
std::string PercentEncode(const std::string& text);

/**
 * The text as valid UTF-8: every ill-formed sequence becomes U+FFFD (the maximal subparts of the
 * Unicode standard, as browsers decode). Everything the page gets goes through it: text from the
 * chain (a sidechain title, a deposit destination) is arbitrary bytes, and the web view drops a script
 * that is not valid UTF-8, with the answer or the events in it.
 */
std::string ValidUtf8(std::string_view text);

/**
 * Wipe the secrets of a request (the strings under "passphrase", "old", "new", at any depth) once
 * it is done with, so that they do not stay in freed memory.
 */
void CleanseSecrets(UniValue& value);

/**
 * The bridge between the page and the node. The page calls one function,
 * with requests like
 *
 *   {"rid": "17", "kind": "rpc",  "method": "getbalances", "params": [], "wallet": "w1"}
 *   {"rid": "18", "kind": "host", "method": "prefs.set", "params": {"theme": "dark"}}
 *   {"kind": "cancel", "rid": "17"}
 *
 * RPC calls go to the in-process RPC dispatcher (as the RPC console of the Qt
 * GUI does: no HTTP, no credentials), host calls to the functions the app
 * registers. Both run on worker threads, never on the UI thread, except the
 * host methods registered to run on the UI thread (window, clipboard). The
 * answer comes back through the reply function: status 0 and the result as
 * JSON, or status 1 and {"code": ..., "message": ...}.
 *
 * A request can be cancelled while it waits or runs: its answer is then the
 * BRIDGE_CANCELLED error at once, and what it returns later is dropped.
 */
class Bridge
{
public:
    //! Sends an answer to the page. Called from any thread; must be thread-safe.
    using ReplyFn = std::function<void(const std::string& call_id, int status, const std::string& json)>;
    //! Runs an RPC method in-process. Throws a UniValue error object, or std::exception.
    using RpcFn = std::function<UniValue(const std::string& method, const UniValue& params, const std::string& uri)>;
    //! A host method: takes the params of the request, returns the result. Throws HostError.
    using HostFn = std::function<UniValue(const UniValue& params)>;
    //! Runs a function on the UI thread, later.
    using UiRunner = std::function<void(std::function<void()>)>;

    /**
     * Where a call runs: on a worker (WORKER), on a worker of the calls that may take long (LONG: a
     * download, a rescan, a command of the console; they never hold up the quick ones), or on the UI
     * thread (window, clipboard).
     */
    enum class Thread { WORKER, LONG, UI };

    /**
     * Which RPC methods the page may call, and where they run: nullopt for a method the page may not
     * call (it is answered with BRIDGE_METHOD_NOT_FOUND). Without a policy, every method runs on a worker.
     */
    using RpcPolicy = std::function<std::optional<Thread>(const std::string& method)>;

    Bridge(ReplyFn reply, UiRunner ui, int workers = 4, int long_workers = 6);
    ~Bridge();

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    /** Where RPC calls go. Until it is set, they are answered with BRIDGE_NOT_READY. */
    void SetRpc(RpcFn rpc) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Told of an exception a host method did not expect (not a HostError): the app shows the internal
     * error dialog of chains-qt (handleNonFatalException). The call is answered with the error anyway.
     */
    using InternalErrorFn = std::function<void(const std::string& method, const std::string& what)>;
    void SetInternalError(InternalErrorFn fn) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Set the RPC methods the page may call (RpcPolicy). */
    void SetRpcPolicy(RpcPolicy policy) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Take requests of the kind "testrpc": any RPC method, past the policy (on the lane the policy gives a
     * method of the pages, any other one on the long lane). Only for a test script (-webuitestscript, in a build
     * with WEBUI_TEST_HOOKS), which drives the node itself as the console would; the calls of the pages ("rpc")
     * keep to the policy all the same, so that the tests run the policy that ships. Refused unless opened.
     */
    void OpenTestRpc() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The RPC methods the policy refused to the page so far, in order, each once (the tests read them: test.refused). */
    std::vector<std::string> Refused() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Make a host method available to the page. */
    void RegisterHost(const std::string& name, HostFn fn, Thread thread = Thread::WORKER) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * The host method registered under this name, or an empty function: for a host that runs them itself, on its own
     * thread (the host of the pages over RPC of a daemon, webui/remotehost.h).
     */
    HostFn FindHost(const std::string& name) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Handle a call of the page. `call_id` identifies the call for the reply,
     * `args_json` is the JSON array of the arguments of the call: [request].
     */
    void Handle(const std::string& call_id, const std::string& args_json) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Stop taking calls: the ones waiting are answered with BRIDGE_STOPPED, the
     * ones running are waited for. Called before the node shuts down, after
     * the node was asked to (so that calls that wait for the node return).
     */
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Number of requests waiting or running. */
    size_t Pending() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Whether the request the calling thread runs (a host method, on the thread the bridge runs it on) was answered
     * already: the page cancelled it, or the app stops. A host method that waits for the user (App::Confirm) withdraws
     * its question then (audit 7 #8). False on a thread that runs no request.
     */
    static bool CurrentRequestDone();

private:
    struct Request {
        std::string call_id;
        std::string rid;
        //! {exact: true}: the result goes back as its JSON text, {"$rawjson": text} (EXACT_KEY), for the page to read its numbers
        //! itself (amounts past 2^53 units, which a JSON.parse of the web view would round).
        bool exact{false};
        std::atomic<bool> answered{false};
    };

    //! Answers a request once; the request leaves the list of pending ones before the answer is sent.
    void Reply(Request& request, int status, const std::string& json) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void ReplyError(Request& request, int code, const std::string& message) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Run(const std::shared_ptr<Request>& request, const UniValue& body) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Finish(const std::shared_ptr<Request>& request) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    const ReplyFn m_reply;
    const UiRunner m_ui;
    ThreadPool m_pool{"webui"};
    ThreadPool m_long_pool{"webuilong"};

    mutable Mutex m_mutex;
    RpcFn m_rpc GUARDED_BY(m_mutex);
    RpcPolicy m_rpc_policy GUARDED_BY(m_mutex);
    bool m_test_rpc GUARDED_BY(m_mutex){false};
    std::vector<std::string> m_refused GUARDED_BY(m_mutex);
    InternalErrorFn m_internal_error GUARDED_BY(m_mutex);
    struct Host {
        HostFn fn;
        Thread thread;
    };
    std::map<std::string, Host> m_host GUARDED_BY(m_mutex);
    //! Requests waiting or running, by the id the page gave them.
    std::map<std::string, std::shared_ptr<Request>> m_requests GUARDED_BY(m_mutex);
    bool m_stopped GUARDED_BY(m_mutex){false};
};

/** The error object of an exception thrown by the RPC dispatcher or a host method. */
UniValue ErrorObject(int code, const std::string& message);

} // namespace webui

#endif // BITCOIN_WEBUI_BRIDGE_H
