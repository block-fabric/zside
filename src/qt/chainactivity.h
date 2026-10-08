// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_CHAINACTIVITY_H
#define BITCOIN_QT_CHAINACTIVITY_H

#include <QString>
#include <QWidget>

#include <cstdint>
#include <string>
#include <unordered_map>

class ClientModel;

QT_BEGIN_NAMESPACE
class QTableWidget;
class QTimer;
QT_END_NAMESPACE

/** The latest blocks and the latest transactions waiting to be mined, for the overview page. */
class ChainActivity : public QWidget
{
    Q_OBJECT

public:
    explicit ChainActivity(QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);

public Q_SLOTS:
    void refresh();

Q_SIGNALS:
    /** The user asked for the details of a block or of a transaction. */
    void detailsRequested(const QString& hash);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void refreshMempool();

    //! What the table shows of a transaction of the mempool.
    struct MempoolEntry {
        int64_t time;
        QString fee;
        QString vsize;
    };

    ClientModel* m_client_model{nullptr};
    //! The transactions of the mempool seen at the last refresh, by txid.
    std::unordered_map<std::string, MempoolEntry> m_mempool;
    QTableWidget* m_blocks;
    QTableWidget* m_transactions;
    //! Limits how often the tables are refreshed while blocks come in fast.
    QTimer* m_timer;
};

#endif // BITCOIN_QT_CHAINACTIVITY_H
