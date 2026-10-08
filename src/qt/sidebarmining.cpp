// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sidebarmining.h>

#include <qt/clientmodel.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

using NodeRpc::Args;
using NodeRpc::Text;

MiningSignals* MiningChanges()
{
    static MiningSignals signals_object;
    return &signals_object;
}

SidebarMining::SidebarMining(NodeRpc::WalletNameFn wallet_name, QWidget* parent)
    : QWidget(parent), m_wallet_name{std::move(wallet_name)}
{
    setObjectName("sidebarMining");
    auto* layout{new QVBoxLayout(this)};
    layout->setContentsMargins(4, 0, 2, 4);
    layout->setSpacing(3);

    m_auto = new QPushButton(this);
    m_auto->setObjectName("sidebarAutoMining");
    m_auto->setCheckable(true);
    m_auto->setFocusPolicy(Qt::NoFocus);
    m_auto->setToolTip(tr("Automatic mining asks for a block whenever there are transactions whose fees pay for one, and offers the miners of the mainchain 99% of those fees. Without fees it asks for nothing and costs nothing."));
    layout->addWidget(m_auto);

    auto* row{new QHBoxLayout};
    row->setSpacing(3);
    m_fee = new QLineEdit(QStringLiteral("0.0001"), this);
    m_fee->setObjectName("sidebarMiningFee");
    m_fee->setToolTip(tr("What to offer the miners of the mainchain for a block mined by hand, in coins of the mainchain. The wallet of the mainchain node pays it."));
    // No wider than the bar is for its entries: a field would ask for room for a line of text.
    m_fee->setMinimumWidth(56);
    m_fee->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_once = new QPushButton(tr("Mine one block"), this);
    m_once->setObjectName("sidebarMineOnce");
    m_once->setFocusPolicy(Qt::NoFocus);
    m_once->setToolTip(tr("Mine one block now for the fee on the left, empty or not. This is how a deposit gets paid, or a withdrawal bundle started, when there are no transactions to pay for a block."));
    row->addWidget(new QLabel(tr("Fee:"), this));
    row->addWidget(m_fee, 1);
    layout->addLayout(row);
    layout->addWidget(m_once);

    m_status = new QLabel(this);
    m_status->setObjectName("sidebarMiningStatus");
    m_status->setWordWrap(true);
    QFont small{m_status->font()};
    small.setPointSizeF(small.pointSizeF() * 0.85);
    m_status->setFont(small);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    layout->addWidget(m_status);

    connect(m_auto, &QPushButton::clicked, this, &SidebarMining::toggleAuto);
    connect(m_once, &QPushButton::clicked, this, &SidebarMining::mineOnce);
    connect(m_fee, &QLineEdit::returnPressed, this, &SidebarMining::mineOnce);
    auto* timer{new QTimer(this)};
    connect(timer, &QTimer::timeout, this, &SidebarMining::refresh);
    timer->start(3000);
    // The Mine tab of the Mainchain page does the same things.
    connect(MiningChanges(), &MiningSignals::changed, this, &SidebarMining::refresh);
    refresh();
}

void SidebarMining::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (client_model) connect(client_model, &ClientModel::numBlocksChanged, this, &SidebarMining::refresh);
    refresh();
}

void SidebarMining::say(const QString& text, bool error)
{
    m_note = text;
    m_note_is_error = error;
    m_note_blocks = m_client_model ? m_client_model->getNumBlocks() : -1;
    Q_EMIT MiningChanges()->changed();
}

QString SidebarMining::address()
{
    const auto wallet{m_wallet_name()};
    if (!wallet) {
        say(tr("Open or create a wallet first: the fees of the blocks go to it."), /*error=*/true);
        return {};
    }
    QString error;
    const auto fresh{NodeRpc::Call(m_client_model, "getnewaddress", Args({"mining"}), error, wallet)};
    if (!fresh) {
        say(error, /*error=*/true);
        return {};
    }
    return Text(*fresh);
}

void SidebarMining::toggleAuto()
{
    QString error;
    if (m_mining) {
        if (NodeRpc::Call(m_client_model, "setbmm", Args({false}), error)) {
            say({});
        } else {
            say(error, /*error=*/true);
        }
        return;
    }
    const QString to{address()};
    if (to.isEmpty()) {
        refresh();
        return;
    }
    if (NodeRpc::Call(m_client_model, "setbmm", Args({true, to.toStdString()}), error)) {
        say({});
    } else {
        say(error, /*error=*/true);
    }
}

void SidebarMining::mineOnce()
{
    bool ok{false};
    const QString fee{m_fee->text().trimmed()};
    if (fee.toDouble(&ok) <= 0 || !ok) {
        say(tr("Enter the fee as a number above zero, like 0.0001."), /*error=*/true);
        return;
    }
    const QString to{address()};
    if (to.isEmpty()) return;
    UniValue params{Args({to.toStdString()})};
    params.push_back(fee.toStdString());
    // Off the GUI thread: the request waits on the mainchain node, for up to a minute.
    m_once->setEnabled(false);
    NodeRpc::CallAsync(this, m_client_model, "requestbmmblock", params, [this](std::optional<UniValue> result, const QString& error) {
        m_once->setEnabled(true);
        if (result) {
            say(tr("Block asked for; the next mainchain block mines it."));
        } else {
            say(error, /*error=*/true);
        }
    });
}

void SidebarMining::refresh()
{
    QString error;
    const auto info{m_client_model ? NodeRpc::Call(m_client_model, "getbmminfo", Args({}), error) : std::nullopt};
    setEnabled(info.has_value());
    if (!info) {
        m_auto->setText(tr("Auto mining"));
        m_auto->setChecked(false);
        m_status->setText(m_client_model ? tr("This chain is not running as a sidechain.") : QString{});
        return;
    }
    // What was said of the last action holds until the chain moves on.
    if (!m_note.isEmpty() && m_client_model->getNumBlocks() != m_note_blocks) m_note.clear();
    // isTrue(): a field missing or of another type is false (get_bool would throw out of this slot).
    m_mining = (*info)["mining"].isTrue();
    m_auto->setChecked(m_mining);
    m_auto->setText(m_mining ? tr("Auto mining: on") : tr("Auto mining: off"));
    QString status;
    if (m_mining) {
        const bool idle{(*info)["idle"].isTrue()};
        status = idle ? tr("Waiting for fees.") : tr("Asking for a block.");
        status += QLatin1Char(' ') + tr("Mined: %1.").arg(Text((*info)["blocks"]));
    }
    if (info->exists("error")) {
        status += (status.isEmpty() ? QString{} : QStringLiteral(" ")) + Text((*info)["error"]);
        m_status->setStyleSheet(QStringLiteral("QLabel { color: #ef4444; }"));
    } else if (!m_note.isEmpty()) {
        status += (status.isEmpty() ? QString{} : QStringLiteral(" ")) + m_note;
        m_status->setStyleSheet(m_note_is_error ? QStringLiteral("QLabel { color: #ef4444; }") : QString{});
    } else {
        m_status->setStyleSheet({});
    }
    m_status->setText(status);
}
