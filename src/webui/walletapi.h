// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_WALLETAPI_H
#define BITCOIN_WEBUI_WALLETAPI_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <univalue.h>
#include <webui/guard.h>
#include <webui/window.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace interfaces {
class Node;
class Wallet;
class WalletLoader;
} // namespace interfaces

namespace webui {

class Bridge;

/**
 * The wallet functions of chains-app, as host methods of the bridge: what the
 * wallet pages of chains-qt do through WalletModel, WalletController and their
 * dialogs, with the same interfaces::Wallet and interfaces::WalletLoader calls
 * and the same rules and messages. The page calls them as
 *
 *   {"kind": "host", "method": "wallet.send.prepare", "params": {"wallet": "w1", ...}}
 *
 * Amounts are integers of satoshis. What pays, signs or unlocks is confirmed in the dialog window of the host first
 * (guard::AskFn, App::AskHost: outside the page, which can neither draw nor answer it), with the facts the host decoded
 * from the transaction itself: each destination, whole, before its amount, the fee, the total, the wallet, the network
 * (audit 7 #1). The passphrase of an encrypted wallet that is locked is typed in that dialog, never in the page: the
 * wallet is unlocked for the call and locked again after it (WalletModel::UnlockContext). A declined question answers
 * BRIDGE_CANCELLED.
 *
 * In a node without wallets (-disablewallet) every method that needs one answers WALLETS_DISABLED.
 *
 * Methods (params; result):
 *   wallets.list         {}; [{name, loaded, format, needsMigration}]
 *   wallets.signers      {}; {signers: [{name, fingerprint}], error?}
 *   wallets.create       {name, passphrase?, disablePrivateKeys, blank, externalSigner}; {name, warnings}
 *   wallets.open         {name}; {name, warnings}
 *   wallets.close        {name}; true
 *   wallets.restore      {file, name}; {name, warnings}
 *   wallets.migrate      {name, passphrase?, load}; {name, watchonlyName?, solvablesName?, backup, loaded}
 *   wallets.restoreMigrate {file, name, passphrase?}; as wallets.migrate
 *   wallets.isEncrypted  {name}; bool
 *   wallet.info          {wallet}; {name, encrypted, locked, privateKeysDisabled, externalSigner, hd, ...}
 *   wallet.encrypt / wallet.changePassphrase
 *   wallet.unlock        {wallet, seconds}; unlocked for so long (walletpassphrase), the passphrase typed in the host's dialog
 *   wallet.lock
 *   wallet.backup / wallet.exportWatchOnly {wallet, file}
 *   wallet.transactions  {wallet, txids? | latest?}; {height, hash, maturity, records: [...], gone?}
 *                        (all records; those of some transactions, and the ids no longer in the wallet; the newest)
 *   wallet.balanceHistory {wallet, points?}; [[time, balance]...]
 *   wallet.transaction   {wallet, txid, idx?, address?}; the details of TransactionDesc
 *   wallet.txActions     {wallet, txid}; {canAbandon, canBump, hex}
 *   wallet.abandon       {wallet, txid}
 *   wallet.bump.prepare / wallet.bump.commit / wallet.bump.psbt
 *   wallet.addresses     {wallet}; [{address, label, purpose, mine}]
 *   wallet.owned         {wallet, addresses (500 at most)}; [[address, label]] of those that are the wallet's (the explorer's "yours")
 *   wallet.address       {wallet, address, skip?, count?, coinSkip?, coinCount?}; what the wallet knows of an address (no scan of the chain)
 *   wallet.addressBook.add / .edit / .delete
 *   wallet.newAddress    {wallet, label, type}; address
 *   wallet.displayAddress {wallet, address}
 *   wallet.requests / wallet.requests.add / wallet.requests.remove
 *   wallet.coins / wallet.coins.lock / wallet.coinControl.summary
 *   wallet.fee           {wallet, confTarget}; the smart fee of the send page
 *   wallet.available     {wallet, selected?}; the balance that can be sent
 *   wallet.checkChange   {wallet, address}; {valid, mine, label}
 *   wallet.send.prepare  {wallet, recipients: [{address, amount, label?, message?, subtractFee?} or {data}], ...}; the
 *                        transaction made (not signed): {token, fee, recipients, total, ...}
 *   wallet.send.commit   {wallet, token}; confirmed on the desktop, signed, sent: {txid} (or a PSBT for a multisig device)
 *   wallet.send.psbt / wallet.send.discard
 *   send.isDust          {address, amount}; whether the output would be dust
 *   wallet.signMessage   {wallet, address, message}; {signature} or an error (confirmed on the desktop)
 *   (wallet RPC methods that spend, sign or need the keys: rpc.confirmed, confirm.h, through WithKeys)
 *   message.verify       {address, signature, message}; {ok, result, message}
 *   psbt.decode          {base64} or {fileBase64}; {psbt}
 *   wallet.psbt.open / wallet.psbt.sign {wallet?, psbt, finalize?}; the state of the PSBT (signing confirmed on the desktop)
 *   psbt.broadcast       {psbt}; {txid} (confirmed on the desktop)
 */
class WalletApi
{
public:
    using NodeFn = std::function<interfaces::Node&()>;
    /**
     * Whether the page may use a file: only one the user chose in a file chooser (to read, or with
     * `write`, to save). Without it, no method takes a file.
     */
    using FileFn = std::function<bool(const std::string& path, bool write)>;

    explicit WalletApi(NodeFn node, FileFn may_use_file = {}, guard::AskFn ask = {});
    ~WalletApi();

    WalletApi(const WalletApi&) = delete;
    WalletApi& operator=(const WalletApi&) = delete;

    /** Make the methods available to the page. */
    void Register(Bridge& bridge);

    /** Run a method directly (the tests; Register uses the same functions). Throws HostError. */
    UniValue Call(const std::string& method, const UniValue& params);

    /** The methods, by name. */
    std::vector<std::string> Methods() const;

    /**
     * Run `run` with the keys of a wallet unlocked for it alone when they are needed (confirm::KeysFn): `dialog` asked
     * first when `always`, and whenever the wallet is locked, with the passphrase typed in it (asked again when wrong,
     * a few times). A note of the network fee the wallet sets is added for `fee_unknown`. Throws HostError: BRIDGE_CANCELLED
     * on a no, PASSPHRASE_INCORRECT after the last wrong passphrase.
     */
    UniValue WithKeys(const std::string& wallet, Window::ConfirmDialog dialog, bool always, bool fee_unknown, const std::function<UniValue()>& run);

    /** The network fee a wallet would pay for a transaction of so many virtual bytes, at its fee rate (confirm::FeeFn). */
    std::optional<CAmount> FeeFor(const std::string& wallet, int64_t vsize);

    /** How many times a wrong passphrase is asked again before the call fails. */
    static constexpr int PASSPHRASE_TRIES{3};

    /** Error code of a wallet that must be unlocked for the call (RPC_WALLET_UNLOCK_NEEDED). */
    static constexpr int UNLOCK_NEEDED{-13};
    /** Error code of a wrong passphrase (RPC_WALLET_PASSPHRASE_INCORRECT). */
    static constexpr int PASSPHRASE_INCORRECT{-14};
    /** Error code of an amount or recipient the wallet refuses (as SendCoinsReturn). */
    static constexpr int SEND_REFUSED{-6};
    /** Error code of every wallet method in a node without wallets (-disablewallet). */
    static constexpr int WALLETS_DISABLED{-18};

private:
    struct Prepared;
    struct Bump;
    using Method = std::function<UniValue(const UniValue&)>;

    void Add(const std::string& name, Method method);

    NodeFn m_node;
    FileFn m_may_use_file;
    guard::AskFn m_ask;
    //! Asks the user (m_ask); declines without one.
    dialog::Answer Ask(const Window::ConfirmDialog& dialog) const;
    //! A loaded wallet by its name; throws -18.
    std::unique_ptr<interfaces::Wallet> WalletNamed(const std::string& name) const;
    //! The wallet loader of the node; throws WALLETS_DISABLED in a node without one.
    interfaces::WalletLoader& Loader() const;
    //! The file of a call, if the user chose it (FileFn); throws otherwise.
    std::string ChosenFile(const UniValue& params, bool write) const;
    std::map<std::string, Method> m_methods;

    Mutex m_mutex;
    int64_t m_next_token GUARDED_BY(m_mutex){1};
    std::map<std::string, std::shared_ptr<Prepared>> m_prepared GUARDED_BY(m_mutex);
    std::map<std::string, std::shared_ptr<Bump>> m_bumps GUARDED_BY(m_mutex);
    //! The tokens of sends and bumps whose question is open (one question per token at a time).
    std::set<std::string> m_asking GUARDED_BY(m_mutex);

    std::string NewToken() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
};

//! Encoding of the receive requests of chains-qt (RecentRequestEntry with its SendCoinsRecipient), so that both apps share them.
struct ReceiveRequest {
    int nVersion{1};
    int64_t id{0};
    int64_t time{0};
    int recipientVersion{1};
    std::string address;
    std::string label;
    int64_t amount{0};
    std::string message;
    std::string payment_request;
    std::string authenticated_merchant;
};
std::string EncodeReceiveRequest(const ReceiveRequest& request);
std::optional<ReceiveRequest> DecodeReceiveRequest(const std::string& data);

} // namespace webui

#endif // BITCOIN_WEBUI_WALLETAPI_H
