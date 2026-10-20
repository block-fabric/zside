// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/bridge.h>

#include <algorithm>

#include <logging.h>
#include <support/cleanse.h>
#include <tinyformat.h>

#include <exception>
#include <utility>

namespace webui {

std::string PercentEncode(const std::string& text)
{
    static constexpr char HEX[]{"0123456789ABCDEF"};
    std::string out;
    out.reserve(text.size());
    for (const unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 15];
        }
    }
    return out;
}

std::string WalletUri(const std::string& wallet_name)
{
    return "/wallet/" + PercentEncode(wallet_name);
}

std::string ValidUtf8(std::string_view text)
{
    static constexpr std::string_view REPLACEMENT{"\xEF\xBF\xBD"};
    std::string out;
    out.reserve(text.size());
    const auto byte{[&](size_t i) { return static_cast<unsigned char>(text[i]); }};
    size_t i{0};
    while (i < text.size()) {
        const unsigned char c{byte(i)};
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        // The length of the sequence, and the range of its second byte (Table 3-7 of the Unicode standard).
        size_t length{0};
        unsigned char low{0x80}, high{0xBF};
        if (c >= 0xC2 && c <= 0xDF) {
            length = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            length = 3;
            if (c == 0xE0) low = 0xA0;
            if (c == 0xED) high = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            length = 4;
            if (c == 0xF0) low = 0x90;
            if (c == 0xF4) high = 0x8F;
        }
        if (length == 0) {
            // Not a lead byte at all.
            out += REPLACEMENT;
            ++i;
            continue;
        }
        size_t valid{1};
        while (valid < length && i + valid < text.size()) {
            const unsigned char next{byte(i + valid)};
            if (valid == 1 ? (next < low || next > high) : (next < 0x80 || next > 0xBF)) break;
            ++valid;
        }
        if (valid == length) {
            out.append(text.substr(i, length));
        } else {
            // The maximal subpart is one replacement; the byte that broke it starts over.
            out += REPLACEMENT;
        }
        i += valid;
    }
    return out;
}

namespace {
bool IsSecretKey(const std::string& key)
{
    return key == "passphrase" || key == "old" || key == "new";
}

void Wipe(const UniValue& value)
{
    // The string belongs to a UniValue we own and may change: only its bytes are overwritten.
    std::string& text{const_cast<std::string&>(value.get_str())};
    if (!text.empty()) memory_cleanse(text.data(), text.size());
}
} // namespace

void CleanseSecrets(UniValue& value)
{
    if (value.isObject()) {
        const std::vector<std::string>& keys{value.getKeys()};
        const std::vector<UniValue>& values{value.getValues()};
        for (size_t i{0}; i < values.size(); ++i) {
            if (values[i].isStr()) {
                if (IsSecretKey(keys[i])) Wipe(values[i]);
            } else {
                CleanseSecrets(const_cast<UniValue&>(values[i]));
            }
        }
    } else if (value.isArray()) {
        for (const UniValue& item : value.getValues()) CleanseSecrets(const_cast<UniValue&>(item));
    }
}

UniValue ErrorObject(int code, const std::string& message)
{
    UniValue error{UniValue::VOBJ};
    error.pushKV("code", code);
    error.pushKV("message", message);
    return error;
}

Bridge::Bridge(ReplyFn reply, UiRunner ui, int workers, int long_workers) : m_reply(std::move(reply)), m_ui(std::move(ui))
{
    m_pool.Start(workers);
    m_long_pool.Start(long_workers);
}

Bridge::~Bridge()
{
    Stop();
}

void Bridge::SetRpc(RpcFn rpc)
{
    LOCK(m_mutex);
    m_rpc = std::move(rpc);
}

void Bridge::SetInternalError(InternalErrorFn fn)
{
    LOCK(m_mutex);
    m_internal_error = std::move(fn);
}

void Bridge::SetRpcPolicy(RpcPolicy policy)
{
    LOCK(m_mutex);
    m_rpc_policy = std::move(policy);
}

void Bridge::OpenTestRpc()
{
    LOCK(m_mutex);
    m_test_rpc = true;
}

std::vector<std::string> Bridge::Refused() const
{
    LOCK(m_mutex);
    return m_refused;
}

void Bridge::RegisterHost(const std::string& name, HostFn fn, Thread thread)
{
    LOCK(m_mutex);
    m_host[name] = Host{std::move(fn), thread};
}

Bridge::HostFn Bridge::FindHost(const std::string& name) const
{
    LOCK(m_mutex);
    const auto it{m_host.find(name)};
    return it == m_host.end() ? HostFn{} : it->second.fn;
}

size_t Bridge::Pending() const
{
    LOCK(m_mutex);
    return m_requests.size();
}

void Bridge::Reply(Request& request, int status, const std::string& json)
{
    // A request is answered once: a cancelled one is answered when it is cancelled, and what it
    // returns afterwards is dropped.
    if (request.answered.exchange(true)) return;
    // The request leaves the list before its answer goes out: whoever has the answer sees it gone
    // (Pending() is 0 once every answer is in, and the rid can be used again at once).
    {
        LOCK(m_mutex);
        const auto it{m_requests.find(request.rid)};
        if (it != m_requests.end() && it->second.get() == &request) m_requests.erase(it);
    }
    if (status == 0 && request.exact) {
        // The result as text: the page reads its numbers exactly (lib/exactjson.ts), not as the doubles of JSON.parse.
        UniValue wrapped{UniValue::VOBJ};
        wrapped.pushKV(std::string{EXACT_KEY}, ValidUtf8(json));
        m_reply(request.call_id, status, wrapped.write());
        return;
    }
    m_reply(request.call_id, status, ValidUtf8(json));
}

void Bridge::ReplyError(Request& request, int code, const std::string& message)
{
    Reply(request, 1, ErrorObject(code, message).write());
}

void Bridge::Finish(const std::shared_ptr<Request>& request)
{
    LOCK(m_mutex);
    const auto it{m_requests.find(request->rid)};
    if (it != m_requests.end() && it->second == request) m_requests.erase(it);
}

void Bridge::Handle(const std::string& call_id, const std::string& args_json)
{
    auto request{std::make_shared<Request>()};
    request->call_id = call_id;

    UniValue args;
    if (!args.read(args_json) || !args.isArray() || args.size() != 1 || !args[0].isObject()) {
        ReplyError(*request, BRIDGE_INVALID_REQUEST, "A bridge call takes one request object");
        return;
    }
    const UniValue& body{args[0]};
    const UniValue& kind{body.find_value("kind")};
    if (!kind.isStr()) {
        ReplyError(*request, BRIDGE_INVALID_REQUEST, "The request has no kind");
        return;
    }
    const UniValue& rid{body.find_value("rid")};
    if (!rid.isNull() && !rid.isStr()) {
        ReplyError(*request, BRIDGE_INVALID_REQUEST, "The rid of a request is a string");
        return;
    }

    if (kind.get_str() == "cancel") {
        if (!rid.isStr()) {
            ReplyError(*request, BRIDGE_INVALID_REQUEST, "Cancel needs the rid of the request to cancel");
            return;
        }
        std::shared_ptr<Request> cancelled;
        {
            LOCK(m_mutex);
            const auto it{m_requests.find(rid.get_str())};
            if (it != m_requests.end()) {
                cancelled = it->second;
                m_requests.erase(it);
            }
        }
        if (cancelled) ReplyError(*cancelled, BRIDGE_CANCELLED, "Cancelled");
        Reply(*request, 0, UniValue{bool{cancelled}}.write());
        return;
    }

    if (kind.get_str() == "testrpc" && !WITH_LOCK(m_mutex, return m_test_rpc)) {
        ReplyError(*request, BRIDGE_INVALID_REQUEST, "Unknown kind of request: testrpc");
        return;
    }
    if (kind.get_str() != "rpc" && kind.get_str() != "host" && kind.get_str() != "testrpc") {
        ReplyError(*request, BRIDGE_INVALID_REQUEST, strprintf("Unknown kind of request: %s", kind.get_str()));
        return;
    }
    const UniValue& method{body.find_value("method")};
    if (!method.isStr() || method.get_str().empty()) {
        ReplyError(*request, BRIDGE_INVALID_REQUEST, "The request has no method");
        return;
    }
    const UniValue& params{body.find_value("params")};
    if (!params.isNull() && !params.isArray() && !params.isObject()) {
        ReplyError(*request, BRIDGE_INVALID_REQUEST, "The params of a request are an array or an object");
        return;
    }
    const UniValue& wallet{body.find_value("wallet")};
    if (!wallet.isNull() && !wallet.isStr()) {
        ReplyError(*request, BRIDGE_INVALID_REQUEST, "The wallet of a request is its name");
        return;
    }

    request->exact = body.find_value("exact").isTrue();
    // Requests without an id of their own cannot be cancelled; they are tracked under the call id.
    request->rid = rid.isStr() ? rid.get_str() : "\x01" + call_id;
    bool stopped{false}, duplicate{false};
    {
        LOCK(m_mutex);
        if (m_stopped) {
            stopped = true;
        } else if (!m_requests.emplace(request->rid, request).second) {
            duplicate = true;
        }
    }
    // Answered outside the lock: Reply takes it to remove the request.
    if (stopped) {
        ReplyError(*request, BRIDGE_STOPPED, "The app is shutting down");
        return;
    }
    if (duplicate) {
        ReplyError(*request, BRIDGE_INVALID_REQUEST, strprintf("A request with rid %s is under way already", request->rid));
        return;
    }

    Run(request, body);
    // Run took copies of what it needs.
    CleanseSecrets(args);
}

namespace {
//! The request the thread runs now (Bridge::CurrentRequestDone): set while its work runs.
thread_local const std::atomic<bool>* g_running_answered{nullptr};

class RunningRequest
{
public:
    explicit RunningRequest(const std::atomic<bool>& answered) : m_previous{g_running_answered} { g_running_answered = &answered; }
    ~RunningRequest() { g_running_answered = m_previous; }
    RunningRequest(const RunningRequest&) = delete;
    RunningRequest& operator=(const RunningRequest&) = delete;

private:
    const std::atomic<bool>* m_previous;
};
} // namespace

bool Bridge::CurrentRequestDone()
{
    return g_running_answered && g_running_answered->load();
}

void Bridge::Run(const std::shared_ptr<Request>& request, const UniValue& body)
{
    const bool test_rpc{body.find_value("kind").get_str() == "testrpc"};
    const bool rpc{test_rpc || body.find_value("kind").get_str() == "rpc"};
    const std::string method{body.find_value("method").get_str()};
    auto params{std::make_shared<UniValue>(body.find_value("params").isNull() ? UniValue{UniValue::VARR} : body.find_value("params"))};
    const std::string uri{body.find_value("wallet").isStr() ? WalletUri(body.find_value("wallet").get_str()) : "/"};

    // What the request does, on the thread it runs on.
    std::function<void()> work;
    Thread thread{Thread::WORKER};
    if (rpc) {
        RpcPolicy policy;
        {
            LOCK(m_mutex);
            policy = m_rpc_policy;
        }
        if (test_rpc) {
            // The lane the policy gives a method of the pages; any other one on the long lane.
            const std::optional<Thread> lane{policy ? policy(method) : std::nullopt};
            thread = lane.value_or(Thread::LONG) == Thread::WORKER ? Thread::WORKER : Thread::LONG;
        } else if (policy) {
            const std::optional<Thread> lane{policy(method)};
            if (!lane) {
                {
                    LOCK(m_mutex);
                    if (std::ranges::find(m_refused, method) == m_refused.end() && m_refused.size() < 100) m_refused.push_back(method);
                }
                ReplyError(*request, BRIDGE_METHOD_NOT_FOUND, strprintf("The interface does not call %s (use the console)", method));
                Finish(request);
                return;
            }
            thread = *lane == Thread::LONG ? Thread::LONG : Thread::WORKER;
        }
        work = [this, request, method, params, uri] {
            RpcFn rpc_fn;
            {
                LOCK(m_mutex);
                rpc_fn = m_rpc;
            }
            if (!rpc_fn) {
                ReplyError(*request, BRIDGE_NOT_READY, "The node is still starting");
                return;
            }
            try {
                const UniValue result{rpc_fn(method, *params, uri)};
                Reply(*request, 0, result.write());
            } catch (const UniValue& error) {
                if (error.isObject() && error.find_value("code").isNum() && error.find_value("message").isStr()) {
                    Reply(*request, 1, ErrorObject(error.find_value("code").getInt<int>(), error.find_value("message").get_str()).write());
                } else {
                    ReplyError(*request, BRIDGE_HOST_ERROR, error.write());
                }
            } catch (const std::exception& e) {
                ReplyError(*request, BRIDGE_HOST_ERROR, e.what());
            }
        };
    } else {
        Host host;
        {
            LOCK(m_mutex);
            const auto it{m_host.find(method)};
            if (it != m_host.end()) host = it->second;
        }
        if (!host.fn) {
            ReplyError(*request, BRIDGE_METHOD_NOT_FOUND, strprintf("No host method %s", method));
            Finish(request);
            return;
        }
        thread = host.thread;
        work = [this, request, host, params, method] {
            const RunningRequest running{request->answered};
            try {
                const UniValue result{host.fn(*params)};
                Reply(*request, 0, result.write());
            } catch (const HostError& e) {
                ReplyError(*request, e.code, e.what());
            } catch (const UniValue& error) {
                ReplyError(*request, BRIDGE_HOST_ERROR, error.write());
            } catch (const std::exception& e) {
                ReplyError(*request, BRIDGE_HOST_ERROR, e.what());
                InternalErrorFn internal;
                {
                    LOCK(m_mutex);
                    internal = m_internal_error;
                }
                if (internal) internal(method, e.what());
            }
        };
        if (host.thread == Thread::UI) {
            m_ui([this, request, work, params] {
                if (!request->answered) work();
                CleanseSecrets(*params);
                Finish(request);
            });
            return;
        }
    }

    auto task{[this, request, work, params] {
        bool stopped;
        {
            LOCK(m_mutex);
            stopped = m_stopped;
        }
        if (stopped) {
            ReplyError(*request, BRIDGE_STOPPED, "The app is shutting down");
        } else if (!request->answered) {
            // Not cancelled while it waited.
            work();
        }
        CleanseSecrets(*params);
        Finish(request);
    }};
    if (!(thread == Thread::LONG ? m_long_pool : m_pool).Submit(std::move(task))) {
        ReplyError(*request, BRIDGE_STOPPED, "The app is shutting down");
        Finish(request);
    }
}

void Bridge::Stop()
{
    {
        LOCK(m_mutex);
        if (m_stopped) return;
        m_stopped = true;
    }
    // Waits for the requests that run; the ones that wait are answered with BRIDGE_STOPPED.
    m_pool.Stop();
    m_long_pool.Stop();
    LogDebug(BCLog::RPC, "webui: bridge stopped\n");
}

} // namespace webui
