// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SIDEBARMINING_H
#define BITCOIN_QT_SIDEBARMINING_H

#include <qt/noderpc.h>

#include <QWidget>

class ClientModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
QT_END_NAMESPACE

/** Tells when the mining of this chain was set otherwise, or a block asked for, by whichever of the places it is done from. */
class MiningSignals : public QObject
{
    Q_OBJECT

Q_SIGNALS:
    void changed();
};
MiningSignals* MiningChanges();

/**
 * The mining of a sidechain, in the bar at the left of the main window:
 * automatic mining on or off, and a block mined by hand for a fee.
 * The Mine tab of the Mainchain page says what these do at length.
 */
class SidebarMining : public QWidget
{
    Q_OBJECT

public:
    explicit SidebarMining(NodeRpc::WalletNameFn wallet_name, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);

public Q_SLOTS:
    void refresh();

private Q_SLOTS:
    void toggleAuto();
    void mineOnce();

private:
    /** An address of the open wallet for the fees of blocks; empty, with the reason shown, if there is none to be had. */
    QString address();
    void say(const QString& text, bool error = false);

    ClientModel* m_client_model{nullptr};
    NodeRpc::WalletNameFn m_wallet_name;
    QPushButton* m_auto{nullptr};
    QLineEdit* m_fee{nullptr};
    QPushButton* m_once{nullptr};
    QLabel* m_status{nullptr};
    bool m_mining{false};
    //! What the last action came to, shown until the state of mining changes.
    QString m_note;
    bool m_note_is_error{false};
    int m_note_blocks{-1};
};

#endif // BITCOIN_QT_SIDEBARMINING_H
