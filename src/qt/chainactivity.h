// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_CHAINACTIVITY_H
#define BITCOIN_QT_CHAINACTIVITY_H

#include <univalue.h>

#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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

    //! What the table shows of a transaction of the mempool.
    struct MempoolEntry {
        int64_t time;
        QString fee;
        QString vsize;
    };
    //! A row of the table of blocks.
    struct BlockRow {
        QString height;
        QString time;
        QString transactions;
        QString hash;
    };
    //! What a refresh shows.
    struct Snapshot {
        //! The latest blocks, the newest first.
        std::vector<BlockRow> blocks;
        //! The transactions of the mempool, by txid.
        std::unordered_map<std::string, MempoolEntry> mempool;
    };
    //! Runs an RPC method of the node: the result, or nothing when it fails.
    using CallFn = std::function<std::optional<UniValue>(const std::string& method, const UniValue& params)>;

    /**
     * Gathers what a refresh shows with `call`, starting from what the last one gathered: blocks and
     * transactions already known are not looked up again, so a refresh costs a few calls. Touches
     * no widget (it may run on another thread) and never throws: whatever fails is left as it was.
     */
    static Snapshot Fetch(const CallFn& call, Snapshot last);

public Q_SLOTS:
    void refresh();

Q_SIGNALS:
    /** The user asked for the details of a block or of a transaction. */
    void detailsRequested(const QString& hash);

protected:
    void showEvent(QShowEvent* event) override;

private:
    //! Shows a snapshot (on the GUI thread) and keeps it for the next Fetch.
    void apply(Snapshot snapshot);

    ClientModel* m_client_model{nullptr};
    //! What the last refresh showed.
    Snapshot m_last;
    //! A Fetch is under way off the GUI thread (with m_last), and whether to refresh again after it.
    bool m_fetching{false};
    bool m_refresh_again{false};
    QTableWidget* m_blocks;
    QTableWidget* m_transactions;
    //! Limits how often the tables are refreshed while blocks come in fast.
    QTimer* m_timer;
};

#endif // BITCOIN_QT_CHAINACTIVITY_H
