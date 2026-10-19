// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_CONFIRM_H
#define BITCOIN_WEBUI_CONFIRM_H

#include <consensus/amount.h>
#include <univalue.h>
#include <webui/bridge.h>
#include <webui/guard.h>
#include <webui/window.h>

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace interfaces {
class Node;
} // namespace interfaces

/**
 * What the page can make the node or the wallet do that moves coins, signs, or changes what the node votes for: never on
 * the page's word alone (audit 7 #1). The page asks the host (rpc.confirmed, or a wallet method of walletapi.cpp); the host
 * decodes what the call would do itself (from the parameters, or from the transaction), asks the user in the dialog window
 * of the host (App::AskHost: outside the page, which can neither draw nor answer it), with the passphrase of an encrypted
 * wallet typed there (the page never sees it), and only then runs the call, with the keys unlocked for it alone.
 *
 * The facts are rows (dialog::Row): the wallet and the network first, then each destination, whole, before its amount, then
 * the fee and the total; for a vote, what changes. Text of the page or the chain in them is shown as text, with what is not
 * shown as itself written out.
 */
namespace webui::confirm {

//! An amount as the dialogs say it: "1.50000000 CHN".
std::string Coins(CAmount amount);
//! The network of the node, as the dialogs name it ("Main network", "Testnet", "Signet", "Regtest").
std::string NetworkName();

//! A row (dialog::Row) of the dialog.
dialog::Row Text(std::string label, std::string value);
dialog::Row Destination(std::string label, std::string value);
dialog::Row Amount(std::string label, CAmount amount);
dialog::Row Total(std::string label, CAmount amount);
dialog::Row Heading(std::string label, std::string value = {});
dialog::Row Note(std::string text);
//! A picture (dialog::Row::Kind::IMAGE): `data_url` a data: URL of PNG or JPEG bytes the host checked; else the dialog says it cannot show it.
dialog::Row Image(std::string label, std::string data_url);

/** The rows every confirmation of a wallet starts with: the wallet, the network. */
std::vector<dialog::Row> WalletRows(const std::string& wallet);

/** What a call of the page is, for its describer: the node, the wallet named (or none), the parameters, as given. */
struct Call {
    interfaces::Node& node;
    std::string wallet;
    std::string method;
    UniValue params;
    //! What the wallet would pay as a network fee for a transaction of so many virtual bytes, at its fee rate (null without a wallet).
    std::function<std::optional<CAmount>(int64_t vsize)> fee_for{};

    /**
     * The parameter at `index` (positional) or named `name` (an object): null when not given. The names are the RPC's
     * own (ArgNames).
     */
    UniValue Arg(size_t index) const;
    /** A method of the node that reads, for what the confirmation shows (the title of a sidechain): its result, or null on an error. */
    UniValue Read(const std::string& method, const UniValue& args, bool wallet = false) const;
};

/** The names of the parameters of an RPC method, in order, as the node's own help says them (cached); empty when unknown. */
std::vector<std::string> ArgNames(interfaces::Node& node, const std::string& method);

/**
 * The parameters of a call as the RPC server would take them, positional: an array as it is; an object only of the
 * parameters' own names (ArgNames: no alias, no "args", no field of an options object), put at their positions with null in
 * the gaps. Throws HostError(BRIDGE_INVALID_REQUEST) for anything else, or an object when the names are unknown. The host
 * describes this array and runs this array, nothing else (audit 7 review: no parser differential).
 */
UniValue CanonicalParams(interfaces::Node& node, const std::string& method, const UniValue& params);

/** The value of a parameter as a dialog shows it: a string as itself, anything else as JSON. */
std::string ValueText(const UniValue& value);

/**
 * The generic rows of the parameters of a call: each one given, by its name, in order, those named in `destinations`
 * (addresses, owners, recipients) first and whole, then those in `amounts` (as given, with the unit), then the others.
 */
std::vector<dialog::Row> ParamRows(const Call& call, const std::vector<std::string>& destinations = {}, const std::vector<std::string>& amounts = {});

/**
 * The second step of an operation in two (a commitment, then its reveal once the commitment is deep enough: reserve a
 * name, then register it), confirmed with the first: the question of the first step shows both, with their fees, and
 * once the first ran the host keeps a one-time authorization of the second, bound to exactly what was shown (see
 * Authorizations). The method of the first step declares it (Method::reveal).
 */
struct Reveal {
    //! The method of the second step, of the same wallet (it must be in Methods() itself, with its describer).
    std::string method;
    //! The title and the button of the one question for both, and what each step is called in it.
    std::string title;
    std::string accept;
    std::string first_label;
    std::string second_label;
    //! What the two steps do, said first in the question.
    std::string what;
    //! Whether the parameters of the second step go with those of the first (both canonical arrays): the same name.
    std::function<bool(const UniValue& first, const UniValue& second)> matches;
    //! About how large each transaction is (virtual bytes), for the fees the question says.
    int64_t first_vsize{250};
    std::function<int64_t(const UniValue& second)> second_vsize;
    //! When the second step runs, as the question says it ("once the reservation is 3 blocks deep: about 45 s").
    std::function<std::string(const Call& first)> when;
};

/**
 * When a second step runs, from the chain's own info (`info_method`, its field `depth_field`: how deep the first must be):
 * "once the reservation is 3 blocks deep (about 3 min)", with the chain's block time.
 */
std::function<std::string(const Call&)> AfterDepth(std::string info_method, std::string depth_field);

/** The parameters of two calls name the same thing: their first parameters are the same string. */
bool SameFirstParam(const UniValue& first, const UniValue& second);

/**
 * How a call is confirmed. A describer gives the question (nothing for a call that needs none: turning something off).
 *  - NODE: a method of the node (a vote, an ack, mining): asked always.
 *  - SPEND: a method of a wallet that pays or signs: asked always, with the passphrase in the same question when the
 *    wallet is locked; the network fee the wallet sets is said when the describer cannot know it (fee_unknown).
 *  - KEYS: a method of a wallet that only needs its keys (it reads with them, or makes a key): asked only when the wallet is
 *    locked, for the passphrase.
 */
struct Method {
    enum class Kind { NODE, SPEND, KEYS };
    Kind kind{Kind::SPEND};
    Bridge::Thread thread{Bridge::Thread::LONG};
    std::function<std::optional<Window::ConfirmDialog>(const Call&)> describe;
    bool fee_unknown{false};
    //! How many of the first parameters must be given (not null): a call without one is refused before it is described.
    size_t required{0};
    //! Whether several calls may be confirmed in one question (rpc.confirmed {calls}): never for what moves coins.
    bool batch{false};
    //! The second step of this one, confirmed with it (rpc.confirmed {then}).
    std::optional<Reveal> reveal{};
};

/** A describer of the generic kind: the title and the button, and which parameters are destinations and amounts. */
std::function<std::optional<Window::ConfirmDialog>(const Call&)> Generic(std::string title, std::string accept, std::string what,
                                                                         std::vector<std::string> destinations = {}, std::vector<std::string> amounts = {},
                                                                         dialog::Level level = dialog::Level::WARNING);

/** The methods confirmed this way, by name: the kit's own (confirm.cpp) and the chain's own (local.cpp). */
const std::map<std::string, Method>& Methods();

/**
 * Runs `run` with the keys of the wallet unlocked for it alone when they are needed: `dialog` asked first (with the
 * passphrase in it when the wallet is locked, asked again when wrong) when `always`, or when the wallet is locked. Throws
 * HostError(BRIDGE_CANCELLED) on a no. Given by the wallet functions (WalletApi::WithKeys); absent without wallets.
 */
using KeysFn = std::function<UniValue(const std::string& wallet, Window::ConfirmDialog dialog, bool always, bool fee_unknown, const std::function<UniValue()>& run)>;
/** The network fee the wallet would pay for a transaction of so many virtual bytes (WalletApi::FeeFor). */
using FeeFn = std::function<std::optional<CAmount>(const std::string& wallet, int64_t vsize)>;

/**
 * The one-time authorizations of second steps (Reveal), kept by the host alone, in memory (an app that starts again asks
 * again). One is made only when the user accepted the question of both steps and the first ran; it is bound to the wallet,
 * the method and the parameters of the second step exactly as shown (canonical, compared as JSON), to the transaction of
 * the first step (still in the wallet, not replaced, when the second comes), to the network fee the question said at most
 * (the wallet's fee for it then must not be more) and to a time (EXPIRY). It is taken by the call that matches it, one
 * at a time; given back when that call failed without making a transaction. Once the second step made its transaction,
 * the method never runs again under it: the same call then only broadcasts that very transaction again if it left the
 * mempool without a block (what the pages resend), at most MAX_REBROADCASTS times; while it waits in the mempool nothing
 * happens; in a block, conflicted, abandoned, gone, or not taken back by the mempool, the authorization is used up and the
 * call is asked as any other. One transaction, however often the page asks. The page never sees nor names one: it can neither make, nor widen, nor reuse it.
 */
class Authorizations
{
public:
    struct Entry {
        std::string wallet;
        std::string method;
        //! The canonical parameters of the second step, as JSON.
        std::string params;
        //! The transaction of the first step.
        std::string commit_txid;
        //! The most the second step's network fee may be (at the wallet's fee rate when it runs), and its size for that.
        CAmount fee_cap{0};
        int64_t vsize{0};
        std::chrono::steady_clock::time_point expires;
        //! The transaction the second step made, once it ran: that very transaction is broadcast again (never a new one)
        //! if it left the mempool without a block, at most MAX_REBROADCASTS times.
        std::string reveal_txid{};
        int rebroadcasts{0};
    };
    static constexpr int MAX_REBROADCASTS{3};
    //! How long one lasts: an hour (a reservation is deep enough in a few blocks).
    static constexpr std::chrono::seconds EXPIRY{3600};
    //! At most so many kept (the oldest goes).
    static constexpr size_t MAX{32};

    void Add(Entry entry);
    /** The one of these exactly, taken (not there any more), if it is there and not expired; expired ones go. */
    std::optional<Entry> Take(const std::string& wallet, const std::string& method, const std::string& params, std::chrono::steady_clock::time_point now);
    /** One taken back (its call failed before it made anything), unless expired. */
    void GiveBack(Entry entry, std::chrono::steady_clock::time_point now);
    size_t Size() const;

private:
    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;
};

/**
 * The host methods:
 *   rpc.confirmed {method, params, wallet?}       one call, confirmed as Methods() says; its result (its error as the RPC's)
 *   rpc.confirmed {calls: [{method, params}], wallet}  several calls of one method of a wallet, confirmed once (proofs of
 *                                                 funds sign one message with each address): [{result} or {error}]
 *   rpc.confirmed {method, params, wallet, then: {method, params}}  a first step and its second (Method::reveal), in one
 *                                                 question; accepted, the first runs, and the second is authorized once
 *                                                 (Authorizations): its result, with "authorized" (seconds the second may
 *                                                 come in) when it is
 *   rpc.confirmed {method, params, wallet, authorizedOnly: true}  only what needs no question (a second step authorized,
 *                                                 the wallet not locked): else BRIDGE_AUTH_NEEDED, nothing asked or run
 *   rpc.confirmedMethods {}                       the names of Methods(), and of the wallet methods of the page's policy that
 *                                                 need the keys (rpcpolicy.h KeyedWalletRpc), confirmed as KEYS
 */
class ConfirmedRpc
{
public:
    using NodeFn = std::function<interfaces::Node&()>;
    ConfirmedRpc(NodeFn node, guard::AskFn ask, KeysFn keys, FeeFn fee = {});

    void Register(Bridge& bridge);

    /** Run a method directly (the tests; Register uses the same function). Throws HostError. */
    UniValue Call(const UniValue& params);

    /** At most this many calls in one rpc.confirmed. */
    static constexpr size_t MAX_CALLS{100};
    /** The cap of a second step's network fee: its fee when confirmed, times this (fees may rise meanwhile, not more). */
    static constexpr int FEE_CAP_FACTOR{2};

    /** The tests: other methods than Methods(), another clock. */
    void SetMethodsForTest(std::map<std::string, Method> methods) { m_test_methods = std::move(methods); }
    void SetClockForTest(std::function<std::chrono::steady_clock::time_point()> now) { m_now = std::move(now); }
    const Authorizations& authorizations() const { return m_authorizations; }

private:
    const std::map<std::string, Method>& AllMethods() const { return m_test_methods ? *m_test_methods : Methods(); }
    UniValue TwoSteps(interfaces::Node& node, const std::string& wallet, const std::string& name, const Method& method, UniValue params, const UniValue& then);
    std::optional<UniValue> Authorized(interfaces::Node& node, const std::string& wallet, const std::string& name, const Method& method, const UniValue& params, bool only);

    NodeFn m_node;
    guard::AskFn m_ask;
    KeysFn m_keys;
    FeeFn m_fee;
    Authorizations m_authorizations;
    std::optional<std::map<std::string, Method>> m_test_methods;
    std::function<std::chrono::steady_clock::time_point()> m_now{[] { return std::chrono::steady_clock::now(); }};
};

} // namespace webui::confirm

#endif // BITCOIN_WEBUI_CONFIRM_H
