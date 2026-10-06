// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SHIELDEDPAGE_H
#define BITCOIN_QT_SHIELDEDPAGE_H

#include <qt/noderpc.h>

#include <QWidget>

class ClientModel;
class QRImageWidget;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;
QT_END_NAMESPACE

/**
 * zSide: the shielded account of the wallet, a page of its own. Its balance; its address to be
 * paid at (with a QR code, and new ones on demand); paying privately, with a memo, out of it;
 * shielding transparent coins into it; and what it received, with the memos, and paid.
 * Only the lists update by themselves: what is being typed is never touched.
 */
class ShieldedPage : public QWidget
{
    Q_OBJECT

public:
    explicit ShieldedPage(NodeRpc::WalletNameFn wallet_name, QWidget* parent = nullptr);
    void setClientModel(ClientModel* client_model);

protected:
    void showEvent(QShowEvent* event) override;

private:
    std::optional<UniValue> call(const std::string& method, const UniValue& params, bool quiet = false);
    void say(const QString& text, bool error = false);
    bool confirm(const QString& title, const QString& text);
    void refresh();
    void showAddress(const QString& address);
    void updateSendHint();
    void updateCastBills();

    NodeRpc::WalletNameFn m_wallet_name;
    ClientModel* m_client_model{nullptr};
    bool m_busy{false};

    QLabel* m_available{nullptr};
    QLabel* m_pending{nullptr};
    QLabel* m_transparent{nullptr};
    QLabel* m_pool{nullptr};
    QLabel* m_status{nullptr};

    QLineEdit* m_address{nullptr};
    QRImageWidget* m_qr{nullptr};

    QLineEdit* m_send_to{nullptr};
    QLineEdit* m_send_amount{nullptr};
    QPlainTextEdit* m_send_memo{nullptr};
    QLabel* m_send_hint{nullptr};

    QLineEdit* m_shield_amount{nullptr};
    QPlainTextEdit* m_shield_memo{nullptr};

    QLineEdit* m_unshield_amount{nullptr};
    QLineEdit* m_unshield_to{nullptr};
    bool m_unshield_everything{false};

    QLineEdit* m_cast_amount{nullptr};
    QLabel* m_cast_bills{nullptr};
    QTableWidget* m_plan{nullptr};
    std::string m_plan_listed;

    QTableWidget* m_history{nullptr};
    QTableWidget* m_notes{nullptr};
    std::string m_history_listed;
    std::string m_notes_listed;
};

#endif // BITCOIN_QT_SHIELDEDPAGE_H
