// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/blockexplorer.h>

#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/itemviews.h>
#include <qt/noderpc.h>

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {
using NodeRpc::Item;
using NodeRpc::Text;

//! Most transactions of the mempool to list.
constexpr size_t MAX_MEMPOOL_ROWS{1000};

QTableWidget* Table(const QStringList& headers, QWidget* parent)
{
    auto* table{new ItemViews::Table(0, headers.size(), parent)};
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

QString Time(const UniValue& time)
{
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(time.getInt<int64_t>()), QLocale::ShortFormat);
}

UniValue Params(std::initializer_list<UniValue> values)
{
    UniValue params(UniValue::VARR);
    for (const auto& value : values) params.push_back(value);
    return params;
}
} // namespace

BlockExplorer::BlockExplorer(QWidget* parent) : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMinMaxButtonsHint)
{
    setWindowTitle(tr("Block Explorer"));
    auto* layout{new QVBoxLayout(this)};

    auto* top{new QHBoxLayout};
    m_search = new QLineEdit(this);
    m_search->setObjectName("explorerSearch");
    m_search->setPlaceholderText(tr("Block height, block hash or transaction id"));
    auto* search_button{new QPushButton(tr("Search"), this)};
    m_count = new QSpinBox(this);
    m_count->setRange(10, 1000);
    m_count->setValue(50);
    m_count->setPrefix(tr("Last "));
    m_count->setSuffix(tr(" blocks"));
    auto* refresh_button{new QPushButton(tr("Refresh"), this)};
    top->addWidget(m_search, 1);
    top->addWidget(search_button);
    top->addWidget(m_count);
    top->addWidget(refresh_button);
    layout->addLayout(top);

    auto* splitter{new QSplitter(Qt::Vertical, this)};
    m_tabs = new QTabWidget(splitter);
    m_blocks = Table({tr("Height"), tr("Time"), tr("Transactions"), tr("Size"), tr("Weight"), tr("Difficulty"), tr("Hash")}, m_tabs);
    m_blocks->setObjectName("explorerBlocks");
    m_tabs->addTab(m_blocks, tr("Blocks"));
    m_mempool = Table({tr("Received"), tr("Fee"), tr("Virtual size"), tr("Transaction id")}, m_tabs);
    m_mempool->setObjectName("explorerMempool");
    m_tabs->addTab(m_mempool, tr("Waiting transactions"));
    splitter->addWidget(m_tabs);

    auto* lower{new QWidget(splitter)};
    auto* lower_layout{new QVBoxLayout(lower)};
    lower_layout->setContentsMargins(0, 0, 0, 0);
    m_details_title = new QLabel(tr("Double-click a block or a transaction to see its details."), lower);
    m_details_title->setObjectName("explorerTitle");
    m_details_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lower_layout->addWidget(m_details_title);
    auto* details_splitter{new QSplitter(Qt::Horizontal, lower)};
    m_block_txs = new ItemViews::List(details_splitter);
    m_block_txs->setObjectName("explorerBlockTxs");
    m_block_txs->setToolTip(tr("Transactions of the block. Click one to see it."));
    m_details = new QPlainTextEdit(details_splitter);
    m_details->setObjectName("explorerDetails");
    m_details->setReadOnly(true);
    m_details->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_details->setFont(GUIUtil::fixedPitchFont());
    details_splitter->addWidget(m_block_txs);
    details_splitter->addWidget(m_details);
    details_splitter->setStretchFactor(1, 2);
    lower_layout->addWidget(details_splitter, 1);
    splitter->addWidget(lower);
    layout->addWidget(splitter, 1);

    connect(search_button, &QPushButton::clicked, this, [this] { search(m_search->text()); });
    connect(m_search, &QLineEdit::returnPressed, this, [this] { search(m_search->text()); });
    connect(refresh_button, &QPushButton::clicked, this, &BlockExplorer::refresh);
    connect(m_blocks, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { showBlock(m_blocks->item(row, 6)->text()); });
    connect(m_mempool, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        m_block_txs->clear();
        m_shown_block.clear();
        showTransaction(m_mempool->item(row, 3)->text(), {});
    });
    connect(m_block_txs, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { showTransaction(item->text(), m_shown_block); });

    resize(980, 700);
    GUIUtil::handleCloseWindowShortcut(this);
}

void BlockExplorer::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (client_model) {
        connect(client_model, &ClientModel::numBlocksChanged, this, [this] { if (isVisible()) refreshBlocks(); });
    }
}

void BlockExplorer::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    refresh();
}

void BlockExplorer::refresh()
{
    refreshBlocks();
    refreshMempool();
}

void BlockExplorer::refreshBlocks()
{
    QString error;
    const auto tip{NodeRpc::Call(m_client_model, "getblockcount", Params({}), error)};
    if (!tip) return;
    const int height{tip->getInt<int>()};
    const int first{std::max(0, height - m_count->value() + 1)};
    QString hash;
    if (auto best{NodeRpc::Call(m_client_model, "getblockhash", Params({height}), error)}) hash = Text(*best);
    const int rows{height - first + 1};
    // Nothing to do if the list is up to date.
    if (m_blocks->rowCount() == rows && m_blocks->item(0, 6) && m_blocks->item(0, 6)->text() == hash) return;
    m_blocks->setRowCount(rows);
    for (int row{0}; row < rows && !hash.isEmpty(); ++row) {
        const auto block{NodeRpc::Call(m_client_model, "getblock", Params({hash.toStdString(), 1}), error)};
        if (!block) break;
        m_blocks->setItem(row, 0, Item(Text((*block)["height"])));
        m_blocks->setItem(row, 1, Item(Time((*block)["time"])));
        m_blocks->setItem(row, 2, Item(Text((*block)["nTx"])));
        m_blocks->setItem(row, 3, Item(Text((*block)["size"])));
        m_blocks->setItem(row, 4, Item(Text((*block)["weight"])));
        m_blocks->setItem(row, 5, Item(QString::number((*block)["difficulty"].get_real(), 'g', 8)));
        m_blocks->setItem(row, 6, Item(hash));
        hash = block->exists("previousblockhash") ? Text((*block)["previousblockhash"]) : QString{};
    }
    m_blocks->resizeColumnsToContents();
}

void BlockExplorer::refreshMempool()
{
    QString error;
    const auto mempool{NodeRpc::Call(m_client_model, "getrawmempool", Params({true}), error)};
    if (!mempool) return;
    const auto& txids{mempool->getKeys()};
    const size_t rows{std::min(txids.size(), MAX_MEMPOOL_ROWS)};
    m_mempool->setRowCount(rows);
    for (size_t row{0}; row < rows; ++row) {
        const UniValue& entry{(*mempool)[txids[row]]};
        m_mempool->setItem(row, 0, Item(Time(entry["time"])));
        m_mempool->setItem(row, 1, Item(Text(entry["fees"]["base"])));
        m_mempool->setItem(row, 2, Item(Text(entry["vsize"])));
        m_mempool->setItem(row, 3, Item(QString::fromStdString(txids[row])));
    }
    m_mempool->resizeColumnsToContents();
    m_tabs->setTabText(1, tr("Waiting transactions (%1)").arg(txids.size()));
}

bool BlockExplorer::showBlock(const QString& hash)
{
    QString error;
    const auto block{NodeRpc::Call(m_client_model, "getblock", Params({hash.toStdString(), 1}), error)};
    if (!block) return false;
    m_shown_block = hash;
    m_block_txs->clear();
    UniValue summary(UniValue::VOBJ);
    for (const auto& key : block->getKeys()) {
        if (key == "tx") {
            for (const UniValue& txid : (*block)[key].getValues()) m_block_txs->addItem(Text(txid));
        } else {
            summary.pushKV(key, (*block)[key]);
        }
    }
    m_details_title->setText(tr("Block %1: %2").arg(Text((*block)["height"]), hash));
    m_details->setPlainText(QString::fromStdString(summary.write(2)));
    return true;
}

bool BlockExplorer::showTransaction(const QString& txid, const QString& block_hash)
{
    QString error;
    UniValue params{Params({txid.toStdString(), true})};
    if (!block_hash.isEmpty()) params.push_back(block_hash.toStdString());
    const auto tx{NodeRpc::Call(m_client_model, "getrawtransaction", params, error)};
    if (!tx) return false;
    // Everything but the serialized transaction, which is long and says nothing new.
    UniValue shown(UniValue::VOBJ);
    for (const auto& key : tx->getKeys()) {
        if (key != "hex") shown.pushKV(key, (*tx)[key]);
    }
    m_details_title->setText(tr("Transaction %1").arg(txid));
    m_details->setPlainText(QString::fromStdString(shown.write(2)));
    return true;
}

void BlockExplorer::search(const QString& input)
{
    const QString query{input.trimmed()};
    if (query.isEmpty()) return;
    static const QRegularExpression number{QStringLiteral("^[0-9]{1,9}$")};
    static const QRegularExpression hash{QStringLiteral("^[0-9a-fA-F]{64}$")};
    QString error;
    if (number.match(query).hasMatch()) {
        if (const auto block_hash{NodeRpc::Call(m_client_model, "getblockhash", Params({query.toInt()}), error)}) {
            showBlock(Text(*block_hash));
            return;
        }
    } else if (hash.match(query).hasMatch()) {
        if (showBlock(query.toLower())) return;
        m_block_txs->clear();
        m_shown_block.clear();
        if (showTransaction(query.toLower(), {})) return;
        error = tr("No block has this hash, and no transaction with this id is waiting to be mined. "
                   "Finding a mined transaction by its id alone requires the node to run with -txindex.");
    } else {
        error = tr("Enter a block height, a block hash or a transaction id.");
    }
    QMessageBox::information(this, windowTitle(), error);
}
