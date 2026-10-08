// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/chainactivity.h>

#include <qt/clientmodel.h>
#include <qt/itemviews.h>
#include <qt/noderpc.h>

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QShowEvent>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <string>
#include <vector>

namespace {
using NodeRpc::Args;
using NodeRpc::Item;
using NodeRpc::Text;

constexpr size_t ROWS{8};
constexpr int REFRESH_DELAY_MS{1000};
//! Most new mempool transactions looked up one by one; beyond, the mempool is listed verbosely.
constexpr size_t MAX_MEMPOOL_LOOKUPS{100};

QTableWidget* Table(const QStringList& headers, QWidget* parent, const char* name)
{
    auto* table{new ItemViews::Table(0, headers.size(), parent)};
    table->setObjectName(name);
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    table->setToolTip(QObject::tr("Double-click a row to see it in the block explorer."));
    return table;
}

QString Time(const UniValue& time)
{
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(time.getInt<int64_t>()), QLocale::ShortFormat);
}

QWidget* Titled(const QString& title, QTableWidget* table, QWidget* parent)
{
    auto* box{new QWidget(parent)};
    auto* layout{new QVBoxLayout(box)};
    layout->setContentsMargins(0, 0, 0, 0);
    auto* label{new QLabel(QStringLiteral("<b>%1</b>").arg(title), box)};
    layout->addWidget(label);
    layout->addWidget(table);
    return box;
}
} // namespace

ChainActivity::ChainActivity(QWidget* parent) : QWidget(parent)
{
    auto* layout{new QHBoxLayout(this)};
    layout->setContentsMargins(0, 0, 0, 0);
    m_transactions = Table({tr("Received"), tr("Fee"), tr("Size"), tr("Transaction id")}, this, "latestTransactions");
    m_blocks = Table({tr("Height"), tr("Time"), tr("Transactions"), tr("Hash")}, this, "latestBlocks");
    layout->addWidget(Titled(tr("Latest transactions"), m_transactions, this), 1);
    layout->addWidget(Titled(tr("Latest blocks"), m_blocks, this), 1);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &ChainActivity::refresh);
    connect(m_blocks, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { Q_EMIT detailsRequested(m_blocks->item(row, 3)->text()); });
    connect(m_transactions, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { Q_EMIT detailsRequested(m_transactions->item(row, 3)->text()); });
}

void ChainActivity::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (!client_model) {
        m_timer->stop();
        return;
    }
    const auto schedule{[this] {
        if (isVisible() && !m_timer->isActive()) m_timer->start(REFRESH_DELAY_MS);
    }};
    connect(client_model, &ClientModel::numBlocksChanged, this, schedule);
    connect(client_model, &ClientModel::mempoolSizeChanged, this, schedule);
}

void ChainActivity::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
}

void ChainActivity::refresh()
{
    if (!m_client_model) return;
    // Off the GUI thread, after the calls of the other pages (NodeRpc::RunAsync), one at a time.
    if (m_fetching) {
        m_refresh_again = true;
        return;
    }
    m_fetching = true;
    // The client model outlives the thread of RunAsync (NodeRpc::Stop comes first at shutdown).
    ClientModel* const client_model{m_client_model};
    NodeRpc::RunAsync(this, [this, client_model, last = std::move(m_last)]() mutable -> std::function<void()> {
        Snapshot snapshot{Fetch([client_model](const std::string& method, const UniValue& params) {
            QString error;
            return NodeRpc::Call(client_model, method, params, error);
        }, std::move(last))};
        // On the GUI thread, if this widget is still there.
        return [this, snapshot = std::move(snapshot)]() mutable {
            m_fetching = false;
            apply(std::move(snapshot));
            if (std::exchange(m_refresh_again, false) && isVisible()) refresh();
        };
    });
}

ChainActivity::Snapshot ChainActivity::Fetch(const CallFn& call, Snapshot last)
{
    Snapshot snapshot;
    // The UniValue getters throw on a field of another type or a missing one: whatever is not as
    // expected is skipped (a block or a transaction) or left as it was (a table).
    try {
        const auto hash{call("getbestblockhash", Args({}))};
        QString next{hash && hash->isStr() ? Text(*hash) : QString{}};
        while (snapshot.blocks.size() < ROWS && !next.isEmpty()) {
            // A block shown last time: so are the ones before it, in order.
            const auto known{std::find_if(last.blocks.begin(), last.blocks.end(), [&](const BlockRow& row) { return row.hash == next; })};
            if (known != last.blocks.end()) {
                for (auto it{known}; it != last.blocks.end() && snapshot.blocks.size() < ROWS; ++it) snapshot.blocks.push_back(*it);
                break;
            }
            const auto header{call("getblockheader", Args({next.toStdString()}))};
            if (!header || !header->isObject()) break;
            snapshot.blocks.push_back({Text((*header)["height"]), Time((*header)["time"]), Text((*header)["nTx"]), next});
            const UniValue& previous{header->find_value("previousblockhash")};
            next = previous.isStr() ? Text(previous) : QString{};
        }
    } catch (const std::exception&) {
        snapshot.blocks = std::move(last.blocks);
    }

    try {
        // The txids only: the verbose listing of a large mempool every second would cost too much.
        // What is known of a transaction is kept until it leaves the mempool, and only the new ones
        // are looked up.
        const auto txids{call("getrawmempool", Args({}))};
        if (!txids || !txids->isArray()) {
            snapshot.mempool = std::move(last.mempool);
            return snapshot;
        }
        std::vector<std::string> unknown;
        for (const UniValue& txid : txids->getValues()) {
            if (!txid.isStr()) continue;
            if (auto known{last.mempool.extract(txid.get_str())}) {
                snapshot.mempool.insert(std::move(known));
            } else {
                unknown.push_back(txid.get_str());
            }
        }
        const auto remember{[&snapshot](const std::string& txid, const UniValue& entry) {
            const UniValue& time{entry.find_value("time")};
            const UniValue& fees{entry.find_value("fees")};
            const UniValue& vsize{entry.find_value("vsize")};
            if (!time.isNum() || !fees.isObject() || !fees.find_value("base").isNum() || !vsize.isNum()) return;
            snapshot.mempool[txid] = {time.getInt<int64_t>(), QString::number(fees.find_value("base").get_real(), 'f', 8), Text(vsize)};
        }};
        if (unknown.size() > MAX_MEMPOOL_LOOKUPS) {
            // Many new ones (the first refresh, a burst): one verbose listing costs less than a call each.
            const auto mempool{call("getrawmempool", Args({true}))};
            if (mempool && mempool->isObject()) {
                snapshot.mempool.clear();
                for (const std::string& txid : mempool->getKeys()) remember(txid, (*mempool)[txid]);
            }
        } else {
            for (const std::string& txid : unknown) {
                // Gone from the mempool meanwhile when this fails: left out until the next refresh.
                if (const auto entry{call("getmempoolentry", Args({txid}))}; entry && entry->isObject()) remember(txid, *entry);
            }
        }
    } catch (const std::exception&) {
        snapshot.mempool = std::move(last.mempool);
    }
    return snapshot;
}

void ChainActivity::apply(Snapshot snapshot)
{
    m_last = std::move(snapshot);

    m_blocks->setRowCount(m_last.blocks.size());
    for (size_t row{0}; row < m_last.blocks.size(); ++row) {
        const BlockRow& block{m_last.blocks[row]};
        m_blocks->setItem(row, 0, Item(block.height));
        m_blocks->setItem(row, 1, Item(block.time));
        m_blocks->setItem(row, 2, Item(block.transactions));
        m_blocks->setItem(row, 3, Item(block.hash));
    }
    m_blocks->resizeColumnsToContents();

    // The newest first.
    std::vector<std::pair<int64_t, std::string>> newest;
    newest.reserve(m_last.mempool.size());
    for (const auto& [txid, entry] : m_last.mempool) newest.emplace_back(entry.time, txid);
    const size_t rows{std::min<size_t>(newest.size(), ROWS)};
    std::partial_sort(newest.begin(), newest.begin() + rows, newest.end(), std::greater<>{});
    m_transactions->setRowCount(rows);
    for (size_t i{0}; i < rows; ++i) {
        const MempoolEntry& entry{m_last.mempool.at(newest[i].second)};
        m_transactions->setItem(i, 0, Item(QLocale().toString(QDateTime::fromSecsSinceEpoch(entry.time), QLocale::ShortFormat)));
        m_transactions->setItem(i, 1, Item(entry.fee));
        m_transactions->setItem(i, 2, Item(entry.vsize));
        m_transactions->setItem(i, 3, Item(QString::fromStdString(newest[i].second)));
    }
    m_transactions->resizeColumnsToContents();
}
