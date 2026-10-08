// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sidechainpage.h>

#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/itemviews.h>
#include <qt/noderpc.h>
#include <qt/platformstyle.h>
#include <qt/sidebarmining.h>
#include <qt/walletmodel.h>

#include <QDebug>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
using NodeRpc::Args;
using NodeRpc::Item;
using NodeRpc::Text;

constexpr int REFRESH_INTERVAL_MS{5000};
//! Columns of the table of withdrawals.
enum { COL_STATUS, COL_AMOUNT, COL_FEE, COL_HEIGHT, COL_REFUND, COL_TXID, COL_VOUT };

//! An amount of a reply; one of another type is shown as it is (get_real would throw out of a slot).
QString Amount(const UniValue& value) { return value.isNum() ? QString::number(value.get_real(), 'f', 8) : Text(value); }

bool IsAmount(const QString& text)
{
    static const QRegularExpression expression{QStringLiteral("^\\d{1,8}(\\.\\d{1,8})?$")};
    return expression.match(text).hasMatch();
}
} // namespace

SidechainPage::SidechainPage(const PlatformStyle* platform_style, QWidget* parent) : QWidget(parent)
{
    auto* layout{new QVBoxLayout(this)};

    m_summary = new QLabel(this);
    m_summary->setObjectName("mainchainSummary");
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName("mainchainTabs");
    m_tabs->addTab(createDepositTab(), tr("&Deposit"));
    m_tabs->addTab(createWithdrawTab(), tr("&Withdraw"));
    m_tabs->addTab(createMiningTab(), tr("&Mine"));
    layout->addWidget(m_tabs);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &SidechainPage::refresh);
}

QWidget* SidechainPage::createDepositTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    m_deposit_instructions = new QLabel(tab);
    m_deposit_instructions->setWordWrap(true);
    m_deposit_instructions->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_deposit_instructions);

    auto* row{new QHBoxLayout};
    m_deposit_address = new QLineEdit(tab);
    m_deposit_address->setObjectName("depositAddress");
    m_deposit_address->setReadOnly(true);
    m_deposit_address->setPlaceholderText(tr("Press the button for a deposit address of this wallet"));
    auto* new_address{new QPushButton(tr("New deposit address"), tab)};
    new_address->setObjectName("depositNewAddress");
    auto* copy{new QPushButton(tr("Copy"), tab)};
    row->addWidget(m_deposit_address, 1);
    row->addWidget(new_address);
    row->addWidget(copy);
    layout->addLayout(row);
    layout->addStretch();

    connect(new_address, &QPushButton::clicked, this, &SidechainPage::newDepositAddress);
    connect(copy, &QPushButton::clicked, this, [this] { GUIUtil::setClipboard(m_deposit_address->text()); });
    return tab;
}

QWidget* SidechainPage::createWithdrawTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};

    auto* box{new QGroupBox(tr("Withdraw to the mainchain"), tab)};
    auto* form{new QFormLayout(box)};
    auto* intro{new QLabel(tr("The coins leave this wallet at once. The mainchain pays them once its miners have voted the withdrawal "
                              "through together with those of others, which takes long. Until the withdrawal is in a bundle that is "
                              "being voted on, it can be taken back."), box)};
    intro->setWordWrap(true);
    form->addRow(intro);
    m_withdraw_address = new QLineEdit(box);
    m_withdraw_address->setObjectName("withdrawAddress");
    form->addRow(tr("Mainchain address:"), m_withdraw_address);
    m_withdraw_amount = new QLineEdit(box);
    m_withdraw_amount->setObjectName("withdrawAmount");
    m_withdraw_amount->setPlaceholderText(QStringLiteral("0.00000000"));
    form->addRow(tr("Amount:"), m_withdraw_amount);
    m_withdraw_fee = new QLineEdit(QStringLiteral("0.0001"), box);
    m_withdraw_fee->setObjectName("withdrawFee");
    m_withdraw_fee->setToolTip(tr("Paid to the miners of the mainchain, on top of the amount. Withdrawals that offer more go first."));
    form->addRow(tr("Mainchain fee:"), m_withdraw_fee);
    m_withdraw_button = new QPushButton(tr("Withdraw"), box);
    m_withdraw_button->setObjectName("withdrawButton");
    form->addRow(m_withdraw_button);
    layout->addWidget(box);

    m_bundle = new QLabel(tab);
    m_bundle->setObjectName("bundleStatus");
    m_bundle->setWordWrap(true);
    layout->addWidget(m_bundle);

    m_withdrawals = new ItemViews::Table(0, 7, tab);
    m_withdrawals->setObjectName("withdrawals");
    m_withdrawals->setHorizontalHeaderLabels({tr("Status"), tr("Amount"), tr("Mainchain fee"), tr("Block"), tr("Refund address"), tr("Transaction"), tr("Output")});
    m_withdrawals->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_withdrawals->setSelectionMode(QAbstractItemView::SingleSelection);
    m_withdrawals->setAlternatingRowColors(true);
    m_withdrawals->verticalHeader()->hide();
    m_withdrawals->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_withdrawals, 1);
    m_refund_button = new QPushButton(tr("Take the selected withdrawal back"), tab);
    m_refund_button->setObjectName("refundButton");
    m_refund_button->setToolTip(tr("Works for withdrawals of this wallet that are waiting, not for those in the bundle being voted on."));
    layout->addWidget(m_refund_button, 0, Qt::AlignLeft);

    connect(m_withdraw_button, &QPushButton::clicked, this, &SidechainPage::withdraw);
    connect(m_refund_button, &QPushButton::clicked, this, &SidechainPage::refund);
    return tab;
}

QWidget* SidechainPage::createMiningTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    auto* intro{new QLabel(tr("The blocks of this chain are mined by the miners of the mainchain, who commit to them for a fee (blind merged "
                              "mining), which the wallet of the mainchain node pays.\n\n"
                              "Automatic mining asks for a block whenever there are transactions whose fees pay for one. It offers the "
                              "mainchain miners 99% of those fees and you keep the fees themselves, so one hundredth is yours. Without "
                              "fees it asks for nothing and costs nothing.\n\n"
                              "A block can also be mined by hand, for a fee you choose: this is how a deposit gets paid, or a "
                              "withdrawal bundle started, when there are no transactions to pay for a block."), tab)};
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form{new QFormLayout};
    m_mining_address = new QLineEdit(tab);
    m_mining_address->setObjectName("bmmAddress");
    m_mining_address->setPlaceholderText(tr("Address of this chain that receives the transaction fees; empty for a new one of this wallet"));
    form->addRow(tr("Pay fees to:"), m_mining_address);
    layout->addLayout(form);

    auto* buttons{new QHBoxLayout};
    m_mining_start = new QPushButton(tr("Start mining"), tab);
    m_mining_start->setObjectName("bmmStart");
    m_mining_stop = new QPushButton(tr("Stop mining"), tab);
    m_mining_stop->setObjectName("bmmStop");
    buttons->addWidget(m_mining_start);
    buttons->addWidget(m_mining_stop);
    buttons->addStretch();
    layout->addLayout(buttons);

    auto* once{new QHBoxLayout};
    once->addWidget(new QLabel(tr("One block by hand, offering in mainchain coins:"), tab));
    m_mining_amount = new QLineEdit(QStringLiteral("0.0001"), tab);
    m_mining_amount->setObjectName("bmmAmount");
    m_mining_amount->setMaximumWidth(140);
    once->addWidget(m_mining_amount);
    m_mine_once = new QPushButton(tr("Mine one block"), tab);
    m_mine_once->setObjectName("bmmOnce");
    once->addWidget(m_mine_once);
    once->addStretch();
    layout->addLayout(once);
    connect(m_mine_once, &QPushButton::clicked, this, &SidechainPage::mineOnce);

    m_mining_status = new QLabel(tab);
    m_mining_status->setObjectName("bmmStatus");
    m_mining_status->setWordWrap(true);
    layout->addWidget(m_mining_status);
    layout->addStretch();

    // Mining is also set from the bar at the left of the main window. There is a page per wallet:
    // only the one shown is refreshed, the others are when they are shown (showEvent).
    connect(MiningChanges(), &MiningSignals::changed, this, [this] {
        if (isVisible()) refresh();
    });
    connect(m_mining_start, &QPushButton::clicked, this, [this] { setMining(true); });
    connect(m_mining_stop, &QPushButton::clicked, this, [this] { setMining(false); });
    return tab;
}

void SidechainPage::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (!client_model) m_timer->stop();
}

void SidechainPage::setWalletModel(WalletModel* wallet_model)
{
    m_wallet_model = wallet_model;
}

void SidechainPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
    m_timer->start(REFRESH_INTERVAL_MS);
}

void SidechainPage::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
}

std::optional<UniValue> SidechainPage::call(const std::string& method, const UniValue& params, bool wallet, bool quiet)
{
    QString error;
    std::optional<QString> wallet_name;
    if (wallet) {
        if (!m_wallet_model) return std::nullopt;
        wallet_name = m_wallet_model->getWalletName();
    }
    auto result{NodeRpc::Call(m_client_model, method, params, error, wallet_name)};
    if (!result && !quiet) QMessageBox::warning(this, tr("Mainchain"), error);
    return result;
}

void SidechainPage::callAsync(const std::string& method, const UniValue& params, std::function<void(const UniValue&)> success,
                              std::function<void()> finally, bool wallet)
{
    std::optional<QString> wallet_name;
    if (wallet) {
        if (!m_wallet_model) {
            finally();
            return;
        }
        wallet_name = m_wallet_model->getWalletName();
    }
    NodeRpc::CallAsync(this, m_client_model, method, params, [this, method, success = std::move(success), finally = std::move(finally)](std::optional<UniValue> result, const QString& error) {
        finally();
        if (!result) {
            QMessageBox::warning(this, tr("Mainchain"), error);
            return;
        }
        // A reply of an unexpected shape makes the UniValue getters throw: not out of this slot.
        try {
            success(*result);
        } catch (const std::exception& e) {
            QMessageBox::warning(this, tr("Mainchain"), tr("Unexpected reply to %1: %2").arg(QString::fromStdString(method), QString::fromStdString(e.what())));
        }
    }, wallet_name);
}

void SidechainPage::newDepositAddress()
{
    if (const auto address{call("getdepositaddress", Args({}), /*wallet=*/true)}) m_deposit_address->setText(Text((*address)["depositaddress"]));
}

void SidechainPage::withdraw()
{
    const QString address{m_withdraw_address->text().trimmed()};
    const QString amount{m_withdraw_amount->text().trimmed()};
    const QString fee{m_withdraw_fee->text().trimmed()};
    if (address.isEmpty() || !IsAmount(amount) || !IsAmount(fee)) {
        QMessageBox::information(this, tr("Mainchain"), tr("Enter a mainchain address, and the amount and the fee as numbers, like 1.5"));
        return;
    }
    // The page, or the wallet, may be gone after a message box: its event loop runs everything else.
    const QPointer<SidechainPage> self{this};
    if (QMessageBox::question(this, tr("Mainchain"), tr("Withdraw %1 to the mainchain address %2, offering mainchain miners %3? %4 leave this wallet now.")
                                                         .arg(amount, address, fee, QString::number(amount.toDouble() + fee.toDouble(), 'f', 8))) != QMessageBox::Yes) return;
    if (!self || !m_wallet_model) return;
    m_withdraw_button->setEnabled(false);
    callAsync("createwithdrawal", Args({address.toStdString(), amount.toStdString(), fee.toStdString()}), [this](const UniValue& result) {
        m_withdraw_address->clear();
        m_withdraw_amount->clear();
        const QPointer<SidechainPage> self{this};
        QMessageBox::information(this, tr("Mainchain"), tr("The withdrawal is in transaction %1. It shows in the list once the transaction is mined.").arg(Text(result["txid"])));
        if (self) refresh();
    }, [this] { m_withdraw_button->setEnabled(true); });
}

void SidechainPage::refund()
{
    const int row{m_withdrawals->currentRow()};
    if (row < 0) {
        QMessageBox::information(this, tr("Mainchain"), tr("Select a withdrawal in the list first."));
        return;
    }
    m_refund_button->setEnabled(false);
    callAsync("refundwithdrawal", Args({m_withdrawals->item(row, COL_TXID)->text().toStdString(), m_withdrawals->item(row, COL_VOUT)->text().toInt()}), [this](const UniValue& result) {
        const QPointer<SidechainPage> self{this};
        QMessageBox::information(this, tr("Mainchain"), tr("%1 will be paid back to %2 by the block that mines the request.").arg(Amount(result["amount"]), Text(result["refundaddress"])));
        if (self) refresh();
    }, [this] { m_refund_button->setEnabled(true); });
}

QString SidechainPage::miningAddress()
{
    QString address{m_mining_address->text().trimmed()};
    if (address.isEmpty()) {
        const auto fresh{call("getnewaddress", Args({"mining"}), /*wallet=*/true)};
        if (!fresh) return {};
        address = Text(*fresh);
        m_mining_address->setText(address);
    }
    return address;
}

void SidechainPage::setMining(bool mine)
{
    UniValue params{Args({mine})};
    if (mine) {
        const QString address{miningAddress()};
        if (address.isEmpty()) return;
        params.push_back(address.toStdString());
    }
    call("setbmm", params);
    // The bar at the left shows the same.
    Q_EMIT MiningChanges()->changed();
}

void SidechainPage::mineOnce()
{
    const QString amount{m_mining_amount->text().trimmed()};
    if (!IsAmount(amount)) {
        QMessageBox::information(this, tr("Mainchain"), tr("Enter the offer as a number, like 0.0001"));
        return;
    }
    const QString address{miningAddress()};
    if (address.isEmpty()) return;
    UniValue params{Args({address.toStdString()})};
    params.push_back(amount.toStdString());
    m_mine_once->setEnabled(false);
    callAsync("requestbmmblock", params, [this](const UniValue& result) {
        // (callAsync catches what the getters throw.)
        m_mining_once = tr("Asked for one block with %1 transactions and %2 of fees, offering %3. It is mined if the next block of the mainchain takes the offer.")
                            .arg(QString::number(result["transactions"].getInt<int>() - 1), Amount(result["fees"]), Amount(result["amount"]));
    }, [this] {
        m_mine_once->setEnabled(true);
        Q_EMIT MiningChanges()->changed();
    }, /*wallet=*/false);
}

void SidechainPage::refresh()
{
    // A reply of an unexpected shape makes the UniValue getters throw, which would terminate the
    // program from a slot: what is left is shown as it was.
    try {
        refreshPage();
    } catch (const std::exception& e) {
        qWarning() << "SidechainPage::refresh:" << e.what();
    }
}

void SidechainPage::refreshPage()
{
    const auto info{call("getmainchaininfo", Args({}), /*wallet=*/false, /*quiet=*/true)};
    if (!info) {
        m_summary->setText(tr("This chain is not running as a sidechain, so there is nothing to show here."));
        m_tabs->setEnabled(false);
        return;
    }
    m_tabs->setEnabled(true);
    m_slot = (*info)["slot"].isNum() ? (*info)["slot"].getInt<int>() : -1;
    const bool connected{(*info)["connected"].isTrue()};
    m_summary->setText(connected
                           ? tr("Following the mainchain node at %1, which is at block %2. This chain is the sidechain in slot %3 of the mainchain.")
                                 .arg(Text((*info)["node"]), Text((*info)["height"])).arg(m_slot)
                           : tr("<b>The mainchain node cannot be reached</b>, so this node cannot tell which new blocks are valid. %1").arg(Text((*info)["error"]).toHtmlEscaped()));
    m_deposit_instructions->setText(tr("Coins come to this chain from the mainchain. Get a deposit address below and give it to the mainchain wallet "
                                       "as the destination of a deposit (Sidechains, Deposit). The address names this sidechain, slot %1, and "
                                       "ends in a checksum, so the mainchain wallet refuses it if it is mistyped or used for another sidechain. "
                                       "The coins arrive with the next block of this chain after the deposit is mined on the mainchain.").arg(m_slot));

    if (const auto bundle{call("getwithdrawalbundle", Args({}), false, true)}) {
        const QString status{Text((*bundle)["status"])};
        QString text;
        if (status == "pending") {
            text = tr("The mainchain is voting on a bundle of %1 withdrawal(s) paying %2 (hash %3).").arg(Text((*bundle)["withdrawals"]), Amount((*bundle)["amount"]), Text((*bundle)["hash"]));
        } else if (status == "next") {
            text = tr("The next block can start a bundle of %1 withdrawal(s) paying %2.").arg(Text((*bundle)["withdrawals"]), Amount((*bundle)["amount"]));
        } else {
            text = bundle->exists("lastfailureheight") && (*bundle)["waiting"].isNum() && (*bundle)["waiting"].getInt<int>() > 0
                       ? tr("The last bundle failed on the mainchain; a new one can be made after a waiting time.")
                       : tr("No bundle is being voted on.");
        }
        m_bundle->setText(text);
    }
    if (const auto withdrawals{call("listwithdrawals", Args({}), false, true)}) {
        const QString selected{m_withdrawals->currentRow() >= 0 ? m_withdrawals->item(m_withdrawals->currentRow(), COL_TXID)->text() : QString{}};
        m_withdrawals->setRowCount(withdrawals->size());
        for (size_t row{0}; row < withdrawals->size(); ++row) {
            const UniValue& w{(*withdrawals)[row]};
            m_withdrawals->setItem(row, COL_STATUS, Item(Text(w["status"]) == "bundled" ? tr("Being voted on") : tr("Waiting")));
            m_withdrawals->setItem(row, COL_AMOUNT, Item(Amount(w["amount"])));
            m_withdrawals->setItem(row, COL_FEE, Item(Amount(w["mainchainfee"])));
            m_withdrawals->setItem(row, COL_HEIGHT, Item(Text(w["height"])));
            m_withdrawals->setItem(row, COL_REFUND, Item(Text(w["refundaddress"])));
            m_withdrawals->setItem(row, COL_TXID, Item(Text(w["txid"])));
            m_withdrawals->setItem(row, COL_VOUT, Item(Text(w["vout"])));
            if (Text(w["txid"]) == selected) m_withdrawals->selectRow(row);
        }
        m_withdrawals->resizeColumnsToContents();
    }
    if (const auto mining{call("getbmminfo", Args({}), false, true)}) {
        const bool on{(*mining)["mining"].isTrue()};
        m_mining_start->setEnabled(!on);
        m_mining_stop->setEnabled(on);
        m_mining_address->setEnabled(!on);
        QString text{on ? tr("Automatic mining is on. Blocks asked for: %1. Blocks mined: %2. Outbid by other nodes: %3.").arg(Text((*mining)["requests"]), Text((*mining)["blocks"]), Text((*mining)["outbid"])) : tr("Automatic mining is off.")};
        if (on && (*mining)["idle"].isTrue()) text += QStringLiteral(" ") + tr("Waiting for transactions whose fees pay for a block.");
        if (on && mining->exists("lastoffer")) text += QStringLiteral(" ") + tr("Last block asked for: %1 of fees, %2 offered.").arg(Amount((*mining)["lastfees"]), Amount((*mining)["lastoffer"]));
        if (!m_mining_once.isEmpty()) text += QStringLiteral("\n") + m_mining_once;
        if (mining->exists("error")) text += ' ' + tr("Last attempt failed: %1").arg(Text((*mining)["error"]));
        m_mining_status->setText(text);
    }
}
