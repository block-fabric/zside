// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_WALLETAPI_H
#define BITCOIN_WEBUI_WALLETAPI_H

#include <primitives/transaction.h>
#include <sync.h>
#include <univalue.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
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
 * Amounts are integers of satoshis. Methods that need the keys of an
 * encrypted wallet take an optional "passphrase": the wallet is unlocked for
 * the call and locked again after it (WalletModel::UnlockContext); without
 * one, a locked wallet answers with the code WALLET_UNLOCK_NEEDED (-13), and
 * the page asks for the passphrase and calls again.
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
 *   wallet.encrypt / wallet.changePassphrase / wallet.unlock / wallet.lock
 *   wallet.backup / wallet.exportWatchOnly {wallet, file}
 *   wallet.transactions  {wallet, txids? | latest?}; {height, hash, maturity, records: [...], gone?}
 *                        (all records; those of some transactions, and the ids no longer in the wallet; the newest)
 *   wallet.balanceHistory {wallet, points?}; [[time, balance]...]
 *   wallet.transaction   {wallet, txid, idx?, address?}; the details of TransactionDesc
 *   wallet.txActions     {wallet, txid}; {canAbandon, canBump, hex}
 *   wallet.abandon       {wallet, txid}
 *   wallet.bump.prepare / wallet.bump.commit / wallet.bump.psbt
 *   wallet.addresses     {wallet}; [{address, label, purpose, mine}]
 *   wallet.addressBook.add / .edit / .delete
 *   wallet.newAddress    {wallet, label, type, passphrase?}; address
 *   wallet.displayAddress {wallet, address}
 *   wallet.requests / wallet.requests.add / wallet.requests.remove
 *   wallet.coins / wallet.coins.lock / wallet.coinControl.summary
 *   wallet.fee           {wallet, confTarget}; the smart fee of the send page
 *   wallet.available     {wallet, selected?}; the balance that can be sent
 *   wallet.checkChange   {wallet, address}; {valid, mine, label}
 *   wallet.send.prepare / wallet.send.commit / wallet.send.psbt / wallet.send.discard
 *   send.isDust          {address, amount}; whether the output would be dust
 *   wallet.signMessage   {wallet, address, message, passphrase?}; {signature} or an error
 *   wallet.withKeys      {wallet, calls: [{method, params}], passphrase?}; [{result} or {error}]
 *                        (wallet RPC methods that need the keys, unlocked for the call alone)
 *   message.verify       {address, signature, message}; {ok, result, message}
 *   psbt.decode          {base64} or {fileBase64}; {psbt}
 *   wallet.psbt.open / wallet.psbt.sign {wallet?, psbt}; the state of the PSBT
 *   psbt.broadcast       {psbt}; {txid}
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

    explicit WalletApi(NodeFn node, FileFn may_use_file = {});
    ~WalletApi();

    WalletApi(const WalletApi&) = delete;
    WalletApi& operator=(const WalletApi&) = delete;

    /** Make the methods available to the page. */
    void Register(Bridge& bridge);

    /** Run a method directly (the tests; Register uses the same functions). Throws HostError. */
    UniValue Call(const std::string& method, const UniValue& params);

    /** The methods, by name. */
    std::vector<std::string> Methods() const;

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
    //! The wallet loader of the node; throws WALLETS_DISABLED in a node without one.
    interfaces::WalletLoader& Loader() const;
    //! The file of a call, if the user chose it (FileFn); throws otherwise.
    std::string ChosenFile(const UniValue& params, bool write) const;
    std::map<std::string, Method> m_methods;

    Mutex m_mutex;
    int64_t m_next_token GUARDED_BY(m_mutex){1};
    std::map<std::string, std::shared_ptr<Prepared>> m_prepared GUARDED_BY(m_mutex);
    std::map<std::string, std::shared_ptr<Bump>> m_bumps GUARDED_BY(m_mutex);

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
