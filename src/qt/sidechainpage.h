// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SIDECHAINPAGE_H
#define BITCOIN_QT_SIDECHAINPAGE_H

#include <univalue.h>

#include <QPointer>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>

class ClientModel;
class PlatformStyle;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTimer;
QT_END_NAMESPACE

/**
 * Page for what ties this chain to its mainchain: how the node follows the
 * mainchain, deposits from it, withdrawals to it and the bundles that pay
 * them, and blind merged mining.
 *
 * The page works through the sidechain RPC commands of the node.
 */
class SidechainPage : public QWidget
{
    Q_OBJECT

public:
    explicit SidechainPage(const PlatformStyle* platform_style, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);
    void setWalletModel(WalletModel* wallet_model);

public Q_SLOTS:
    /** Reload everything shown from the node. */
    void refresh();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private Q_SLOTS:
    void newDepositAddress();
    void withdraw();
    void refund();
    void setMining(bool mine);
    void mineOnce();
    /** The address the fees of blocks go to: the one entered, or a new one of the wallet. Empty if there is none to be had. */
    QString miningAddress();

private:
    //! What refresh does; it may throw on a reply of an unexpected shape.
    void refreshPage();
    /**
     * Run an RPC command. Returns nothing after showing the error to the user
     * if the command fails.
     */
    std::optional<UniValue> call(const std::string& method, const UniValue& params, bool wallet = false, bool quiet = false);
    /**
     * Run an RPC command that can take long off the GUI thread. Then, on the GUI thread, `finally`,
     * and `success` with the result, or the error is shown to the user. Nothing runs if the page is
     * gone by then.
     */
    void callAsync(const std::string& method, const UniValue& params, std::function<void(const UniValue&)> success,
                   std::function<void()> finally, bool wallet = true);

    QWidget* createDepositTab();
    QWidget* createWithdrawTab();
    QWidget* createMiningTab();

    ClientModel* m_client_model{nullptr};
    //! Guarded: the wallet may be unloaded while a message box of this page, or a call off the GUI
    //! thread, is under way.
    QPointer<WalletModel> m_wallet_model;
    QTimer* m_timer{nullptr};

    QLabel* m_summary{nullptr};
    QTabWidget* m_tabs{nullptr};

    QLabel* m_deposit_instructions{nullptr};
    QLineEdit* m_deposit_address{nullptr};

    QLineEdit* m_withdraw_address{nullptr};
    QLineEdit* m_withdraw_amount{nullptr};
    QLineEdit* m_withdraw_fee{nullptr};
    QPushButton* m_withdraw_button{nullptr};
    QLabel* m_bundle{nullptr};
    QTableWidget* m_withdrawals{nullptr};
    QPushButton* m_refund_button{nullptr};

    QLineEdit* m_mining_address{nullptr};
    QLineEdit* m_mining_amount{nullptr};
    QPushButton* m_mine_once{nullptr};
    QPushButton* m_mining_start{nullptr};
    QPushButton* m_mining_stop{nullptr};
    QLabel* m_mining_status{nullptr};
    //! What became of the last block asked for by hand.
    QString m_mining_once;

    int m_slot{0};
};

#endif // BITCOIN_QT_SIDECHAINPAGE_H
