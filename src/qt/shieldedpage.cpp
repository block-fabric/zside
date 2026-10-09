// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/shieldedpage.h>

#include <qt/clientmodel.h>
#include <qt/itemviews.h>
#include <qt/qrimagewidget.h>

#include <QApplication>

#include <algorithm>
#include <QClipboard>
#include <QDateTime>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QShowEvent>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

using NodeRpc::Args;
using NodeRpc::Item;
using NodeRpc::Text;

namespace {
const char* const GREEN{"#10b981"};
const char* const ORANGE{"#f59e0b"};
const char* const RED{"#ef4444"};
const char* const PURPLE{"#8b5cf6"};

QTableWidget* ViewTable(const QStringList& headers, QWidget* parent)
{
    // Read-only, no input method (ItemViews: the Wayland crash).
    auto* table{new ItemViews::Table(0, headers.size(), parent)};
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

QLabel* Dim(const QString& text, QWidget* parent)
{
    auto* label{new QLabel(text, parent)};
    label->setWordWrap(true);
    label->setEnabled(false);
    return label;
}

QLabel* Heading(const QString& text, QWidget* parent)
{
    auto* label{new QLabel(text, parent)};
    label->setObjectName("sectionHeading");
    return label;
}

QFrame* Card(QWidget* parent)
{
    auto* card{new QFrame(parent)};
    card->setObjectName("card");
    return card;
}

QLineEdit* AmountEdit(QWidget* parent)
{
    auto* edit{new QLineEdit(parent)};
    edit->setPlaceholderText(QObject::tr("Amount in CHN"));
    edit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral(R"(\d{0,8}(\.\d{0,8})?)")), edit));
    edit->setMinimumHeight(28);
    return edit;
}

QPlainTextEdit* MemoEdit(QWidget* parent)
{
    auto* edit{new QPlainTextEdit(parent)};
    edit->setPlaceholderText(QObject::tr("A memo only the recipient can read (optional, 512 bytes at most)"));
    edit->setMaximumHeight(64);
    return edit;
}

/** An amount typed in CHN, in satoshis; -1 if it is not one. */
int64_t Satoshis(const QString& text)
{
    const QStringList parts{text.split(QLatin1Char('.'))};
    if (text.isEmpty() || parts.size() > 2) return -1;
    bool ok{true};
    const int64_t whole{parts[0].isEmpty() ? 0 : parts[0].toLongLong(&ok)};
    if (!ok) return -1;
    QString fraction{parts.size() == 2 ? parts[1] : QString{}};
    fraction = fraction.leftJustified(8, QLatin1Char('0'));
    const int64_t sats{fraction.toLongLong(&ok)};
    return ok ? whole * 100'000'000 + sats : -1;
}

QString Chn(int64_t sats)
{
    QString text{QString::number(sats / 100'000'000) + QLatin1Char('.') + QStringLiteral("%1").arg(sats % 100'000'000, 8, 10, QLatin1Char('0'))};
    while (text.endsWith(QLatin1Char('0'))) text.chop(1);
    if (text.endsWith(QLatin1Char('.'))) text += QLatin1Char('0');
    return text;
}

// Melt/cast: the smallest bill (0.01 CHN), and how many there are (wallet/meltcast.h).
constexpr int64_t BILL_UNIT{1'000'000};
constexpr int NUM_BILLS{21};

bool IsShieldedAddress(const QString& address)
{
    return address.startsWith(QLatin1String("zo1")) || address.startsWith(QLatin1String("tzo1")) || address.startsWith(QLatin1String("rzo1"));
}
} // namespace

ShieldedPage::ShieldedPage(NodeRpc::WalletNameFn wallet_name, QWidget* parent)
    : QWidget(parent), m_wallet_name{std::move(wallet_name)}
{
    setStyleSheet(QStringLiteral(
        "QLabel#pageTitle { font-size: 18pt; font-weight: bold; }"
        "QLabel#bigNumber { font-size: 20pt; font-weight: bold; }"
        "QLabel#midNumber { font-size: 13pt; font-weight: bold; }"
        "QLabel#sectionHeading { font-size: 11pt; font-weight: bold; margin-top: 6px; }"
        "QFrame#card { border: 1px solid palette(mid); border-radius: 10px; }"
        "QPushButton#primary { font-weight: bold; padding: 6px 18px; }"
        "QLineEdit#address { font-family: monospace; }"));
    auto* outer{new QVBoxLayout(this)};

    // Always in sight: the balances.
    auto* title_row{new QHBoxLayout};
    auto* title{new QLabel(tr("Private"), this)};
    title->setObjectName("pageTitle");
    title_row->addWidget(title);
    title_row->addSpacing(12);
    title_row->addWidget(Dim(tr("Coins in the shielded pool move without showing who pays whom, or how much: only the sender and the recipient know."), this), 1);
    outer->addLayout(title_row);

    auto* strip{Card(this)};
    auto* strip_layout{new QHBoxLayout(strip)};
    auto* shielded_box{new QVBoxLayout};
    shielded_box->addWidget(Dim(tr("Shielded"), strip));
    m_available = new QLabel(QStringLiteral("—"), strip);
    m_available->setObjectName("bigNumber");
    shielded_box->addWidget(m_available);
    m_pending = Dim(QString{}, strip);
    shielded_box->addWidget(m_pending);
    strip_layout->addLayout(shielded_box, 2);
    auto* transparent_box{new QVBoxLayout};
    transparent_box->addWidget(Dim(tr("Transparent"), strip));
    m_transparent = new QLabel(QStringLiteral("—"), strip);
    m_transparent->setObjectName("midNumber");
    transparent_box->addWidget(m_transparent);
    transparent_box->addStretch();
    strip_layout->addLayout(transparent_box, 1);
    auto* pool_box{new QVBoxLayout};
    pool_box->addWidget(Dim(tr("The whole pool"), strip));
    m_pool = new QLabel(QStringLiteral("—"), strip);
    m_pool->setWordWrap(true);
    pool_box->addWidget(m_pool);
    pool_box->addStretch();
    strip_layout->addLayout(pool_box, 1);
    outer->addWidget(strip);

    auto* tabs{new QTabWidget(this)};
    outer->addWidget(tabs, 1);

    // Receive.
    auto* receive_tab{new QWidget(tabs)};
    auto* receive_layout{new QHBoxLayout(receive_tab)};
    auto* receive_text{new QVBoxLayout};
    receive_text->addWidget(Heading(tr("Your shielded address"), receive_tab));
    m_address = new QLineEdit(receive_tab);
    m_address->setObjectName("address");
    m_address->setReadOnly(true);
    receive_text->addWidget(m_address);
    auto* address_buttons{new QHBoxLayout};
    auto* copy{new QPushButton(tr("Copy"), receive_tab)};
    auto* fresh{new QPushButton(tr("New address"), receive_tab)};
    fresh->setToolTip(tr("Another address of the same account, which nobody can link to the others: one per payer, for privacy"));
    address_buttons->addWidget(copy);
    address_buttons->addWidget(fresh);
    address_buttons->addStretch();
    receive_text->addLayout(address_buttons);
    receive_text->addWidget(Dim(tr("Every address you make here pays the same shielded balance. Give a new one to each payer: nobody can tell they belong together."), receive_tab));
    receive_text->addStretch();
    receive_layout->addLayout(receive_text, 1);
    m_qr = new QRImageWidget(receive_tab);
    m_qr->setFixedSize(220, 220);
    m_qr->setScaledContents(true);
    receive_layout->addWidget(m_qr, 0, Qt::AlignTop);
    tabs->addTab(receive_tab, tr("Receive"));

    // Pay.
    auto* pay_tab{new QWidget(tabs)};
    auto* send_layout{new QVBoxLayout(pay_tab)};
    send_layout->addWidget(Heading(tr("Pay privately"), pay_tab));
    auto* send_form{new QFormLayout};
    m_send_to = new QLineEdit(pay_tab);
    m_send_to->setPlaceholderText(tr("A shielded address (zo1…), or a transparent one to unshield"));
    m_send_amount = AmountEdit(pay_tab);
    m_send_memo = MemoEdit(pay_tab);
    send_form->addRow(tr("To"), m_send_to);
    send_form->addRow(tr("Amount"), m_send_amount);
    send_form->addRow(tr("Memo"), m_send_memo);
    send_layout->addLayout(send_form);
    m_send_hint = new QLabel(pay_tab);
    m_send_hint->setWordWrap(true);
    send_layout->addWidget(m_send_hint);
    auto* send{new QPushButton(tr("Pay"), pay_tab)};
    send->setObjectName("primary");
    send_layout->addWidget(send, 0, Qt::AlignRight);
    send_layout->addStretch();
    tabs->addTab(pay_tab, tr("Pay"));

    // Shield and unshield, side by side.
    auto* move_tab{new QWidget(tabs)};
    auto* move_layout{new QHBoxLayout(move_tab)};
    auto* shield_card{Card(move_tab)};
    auto* shield_layout{new QVBoxLayout(shield_card)};
    shield_layout->addWidget(Heading(tr("Shield: transparent → shielded"), shield_card));
    shield_layout->addWidget(Dim(tr("Move transparent coins of this wallet into its shielded balance. That move is public; what you do with them after is not."), shield_card));
    m_shield_amount = AmountEdit(shield_card);
    auto* all{new QPushButton(tr("All"), shield_card)};
    auto* amount_row{new QHBoxLayout};
    amount_row->addWidget(m_shield_amount, 1);
    amount_row->addWidget(all);
    shield_layout->addLayout(amount_row);
    m_shield_memo = MemoEdit(shield_card);
    m_shield_memo->setPlaceholderText(tr("A note to yourself (optional)"));
    shield_layout->addWidget(m_shield_memo);
    auto* shield{new QPushButton(tr("Shield"), shield_card)};
    shield->setObjectName("primary");
    shield_layout->addWidget(shield, 0, Qt::AlignRight);
    shield_layout->addStretch();
    move_layout->addWidget(shield_card, 1);

    auto* unshield_card{Card(move_tab)};
    auto* unshield_layout{new QVBoxLayout(unshield_card)};
    unshield_layout->addWidget(Heading(tr("Unshield: shielded → transparent"), unshield_card));
    unshield_layout->addWidget(Dim(tr("Move coins out of the shielded balance. The amount and the address become public; where the coins came from does not. "
                                      "For amounts nobody can link to what went in, use Melt & cast."), unshield_card));
    m_unshield_amount = AmountEdit(unshield_card);
    auto* unshield_all{new QPushButton(tr("All"), unshield_card)};
    unshield_all->setToolTip(tr("The whole shielded balance in blocks, the fee taken out of it"));
    auto* unshield_row{new QHBoxLayout};
    unshield_row->addWidget(m_unshield_amount, 1);
    unshield_row->addWidget(unshield_all);
    unshield_layout->addLayout(unshield_row);
    m_unshield_to = new QLineEdit(unshield_card);
    m_unshield_to->setPlaceholderText(tr("To a transparent address (empty: a new address of this wallet)"));
    unshield_layout->addWidget(m_unshield_to);
    auto* unshield{new QPushButton(tr("Unshield"), unshield_card)};
    unshield->setObjectName("primary");
    unshield_layout->addWidget(unshield, 0, Qt::AlignRight);
    unshield_layout->addStretch();
    move_layout->addWidget(unshield_card, 1);
    tabs->addTab(move_tab, tr("Shield && unshield"));

    // Melt and cast.
    auto* meltcast_tab{new QWidget(tabs)};
    auto* meltcast_layout{new QVBoxLayout(meltcast_tab)};
    meltcast_layout->addWidget(Dim(tr("What goes into the pool and what comes out are public, and can be linked: 13.47 in, 13.47 out an hour later. "
                                      "Melting shields each of your transparent coins alone, at random blocks over the next few hours. "
                                      "Casting takes coins out only as standard bills (0.01 CHN times a power of two), each to a new address of this wallet, "
                                      "at the blocks every wallet uses for that bill: they look like everyone else's. "
                                      "Keep the wallet running and unlocked meanwhile: it carries out the plan as the blocks come."), meltcast_tab));
    auto* meltcast_row{new QHBoxLayout};
    auto* melt{new QPushButton(tr("Melt my transparent coins"), meltcast_tab)};
    melt->setObjectName("primary");
    melt->setToolTip(tr("Every transparent coin in a block goes into the shielded balance alone, over about the next 36 blocks; the coins are locked until then"));
    meltcast_row->addWidget(melt);
    meltcast_row->addSpacing(24);
    meltcast_row->addWidget(new QLabel(tr("Cast"), meltcast_tab));
    m_cast_amount = AmountEdit(meltcast_tab);
    m_cast_amount->setMaximumWidth(180);
    meltcast_row->addWidget(m_cast_amount);
    auto* cast{new QPushButton(tr("Cast"), meltcast_tab)};
    cast->setObjectName("primary");
    meltcast_row->addWidget(cast);
    meltcast_row->addStretch();
    meltcast_layout->addLayout(meltcast_row);
    m_cast_bills = Dim(QString{}, meltcast_tab);
    meltcast_layout->addWidget(m_cast_bills);
    m_plan = ViewTable({tr("Step"), tr("Amount"), tr("Status"), tr("When"), tr("Transaction")}, meltcast_tab);
    m_plan->setColumnWidth(0, 90);
    m_plan->setColumnWidth(1, 120);
    m_plan->setColumnWidth(2, 110);
    m_plan->setColumnWidth(3, 280);
    meltcast_layout->addWidget(m_plan, 1);
    auto* plan_buttons{new QHBoxLayout};
    auto* cancel{new QPushButton(tr("Cancel the selected step"), meltcast_tab)};
    auto* cancel_all{new QPushButton(tr("Cancel all"), meltcast_tab)};
    plan_buttons->addStretch();
    plan_buttons->addWidget(cancel);
    plan_buttons->addWidget(cancel_all);
    meltcast_layout->addLayout(plan_buttons);
    tabs->addTab(meltcast_tab, tr("Melt && cast"));

    // History, and the notes.
    m_history = ViewTable({tr("When"), tr("What"), tr("Amount"), tr("Memo"), tr("Confirmations")}, tabs);
    m_history->setColumnWidth(0, 130);
    m_history->setColumnWidth(1, 260);
    m_history->setColumnWidth(2, 130);
    m_history->setColumnWidth(3, 300);
    tabs->addTab(m_history, tr("History"));
    m_notes = ViewTable({tr("Amount"), tr("Memo"), tr("Kind"), tr("Confirmations"), tr("Transaction")}, tabs);
    tabs->addTab(m_notes, tr("Notes"));
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int index) {
        if (tabs->widget(index) == m_notes) m_notes_listed.clear();
        refresh();
    });

    m_status = new QLabel(this);
    m_status->setObjectName("shieldedStatus");
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(m_status);

    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_address->text());
        say(tr("The address is copied."));
    });
    connect(fresh, &QPushButton::clicked, this, [this] {
        callThen("getshieldedaddress", Args({true}), [this](const UniValue& a) { showAddress(Text(a)); });
    });
    connect(m_send_to, &QLineEdit::textChanged, this, &ShieldedPage::updateSendHint);
    connect(send, &QPushButton::clicked, this, [this, send] {
        const QString to{m_send_to->text().trimmed()};
        const QString amount{m_send_amount->text()};
        if (to.isEmpty() || amount.isEmpty()) return say(tr("Give an address and an amount."), true);
        const bool shielded{IsShieldedAddress(to)};
        const QString memo{m_send_memo->toPlainText()};
        if (memo.toUtf8().size() > 512) return say(tr("The memo is longer than 512 bytes."), true);
        if (!confirm(tr("Pay"), shielded ? tr("Pay %1 CHN privately to %2?").arg(amount, to)
                                         : tr("Unshield %1 CHN to the transparent address %2? The amount and the address will be public; where they came from will not.").arg(amount, to))) return;
        UniValue amounts(UniValue::VOBJ);
        UniValue value;
        value.setNumStr(amount.toStdString());
        amounts.pushKV(to.toStdString(), value);
        // Proving takes a few seconds: off the GUI thread.
        callAsync("sendshielded", Args({amounts, memo.toStdString()}), send, tr("Proving the payment…"), [this, amount](const UniValue&) {
            say(tr("Paid: %1 CHN, with the next block.").arg(amount));
            m_send_amount->clear();
            m_send_memo->clear();
        });
    });
    connect(all, &QPushButton::clicked, this, [this] {
        callThen("getbalance", UniValue{UniValue::VARR}, [this](const UniValue& b) {
            // Leave a little for the fee.
            const double left{b.get_real() - 0.001};
            if (left > 0) m_shield_amount->setText(QString::number(left, 'f', 8));
        }, /*quiet=*/true);
    });
    connect(shield, &QPushButton::clicked, this, [this, shield] {
        const QString amount{m_shield_amount->text()};
        if (amount.isEmpty()) return say(tr("Give an amount."), true);
        if (!confirm(tr("Shield"), tr("Move %1 CHN of transparent coins into the shielded balance?").arg(amount))) return;
        UniValue value;
        value.setNumStr(amount.toStdString());
        callAsync("shieldcoins", Args({value, UniValue{}, m_shield_memo->toPlainText().toStdString()}), shield, tr("Shielding…"), [this, amount](const UniValue&) {
            say(tr("Shielding %1 CHN: spendable after the next block.").arg(amount));
            m_shield_amount->clear();
            m_shield_memo->clear();
        });
    });
    connect(unshield_all, &QPushButton::clicked, this, [this] {
        callThen("getshieldedbalance", UniValue{UniValue::VARR}, [this](const UniValue& b) {
            m_unshield_amount->setText(Text(b["available"]));
            // The whole balance: the fee comes out of it. Typing another amount undoes it.
            m_unshield_everything = true;
        }, /*quiet=*/true);
    });
    connect(m_unshield_amount, &QLineEdit::textEdited, this, [this] { m_unshield_everything = false; });
    connect(unshield, &QPushButton::clicked, this, [this, unshield] {
        const QString amount{m_unshield_amount->text().trimmed()};
        const QString to{m_unshield_to->text().trimmed()};
        if (amount.isEmpty()) return say(tr("Give an amount."), true);
        if (!to.isEmpty() && IsShieldedAddress(to)) return say(tr("That is a shielded address: pay it privately in the Pay tab."), true);
        const bool everything{m_unshield_everything};
        const QString what{everything ? tr("the whole shielded balance in blocks (less the fee)") : tr("%1 CHN").arg(amount)};
        if (!confirm(tr("Unshield"), tr("Move %1 out of the shielded balance to %2? The amount and the address will be public; where the coins came from will not.")
                                         .arg(what, to.isEmpty() ? tr("a new address of this wallet") : to))) return;
        UniValue value;
        if (everything) {
            value.setStr("all");
        } else {
            value.setNumStr(amount.toStdString());
        }
        callAsync("unshieldcoins", Args({value, to.isEmpty() ? UniValue{} : UniValue{to.toStdString()}}), unshield, tr("Proving the unshielding…"), [this](const UniValue& r) {
            say(tr("Unshielding %1 CHN to %2: spendable after the next block.").arg(Text(r["amount"]), Text(r["address"])));
            m_unshield_amount->clear();
            m_unshield_everything = false;
            m_unshield_to->clear();
        });
    });
    connect(melt, &QPushButton::clicked, this, [this] {
        if (!confirm(tr("Melt"), tr("Shield every transparent coin of this wallet, each one alone, over about the next 36 blocks? "
                                    "The coins are locked until they go. Keep the wallet running and unlocked meanwhile."))) return;
        callThen("meltcoins", UniValue{UniValue::VARR}, [this](const UniValue& r) {
            say(tr("%n coin(s) will be melted over the next blocks.", "", static_cast<int>(r.size())));
            m_plan_listed.clear();
            refresh();
        });
    });
    connect(m_cast_amount, &QLineEdit::textChanged, this, &ShieldedPage::updateCastBills);
    connect(cast, &QPushButton::clicked, this, [this] {
        const QString amount{m_cast_amount->text()};
        if (Satoshis(amount) < BILL_UNIT) return say(tr("Give an amount of 0.01 CHN at least."), true);
        if (!confirm(tr("Cast"), tr("Take %1 CHN out of the shielded balance as standard bills, each to a new address of this wallet, at the blocks set for each bill "
                                    "(within a day or two)? Keep the wallet running and unlocked meanwhile.").arg(amount))) return;
        UniValue value;
        value.setNumStr(amount.toStdString());
        callThen("castcoins", Args({value}), [this](const UniValue& r) {
            say(tr("%n bill(s) will be cast, at their blocks.", "", static_cast<int>(r.size())));
            m_cast_amount->clear();
            m_plan_listed.clear();
            refresh();
        });
    });
    connect(cancel, &QPushButton::clicked, this, [this] {
        const int row{m_plan->currentRow()};
        if (row < 0 || !m_plan->item(row, 0) || m_plan->item(row, 0)->data(Qt::UserRole).isNull()) return say(tr("Select a step that has not gone yet."), true);
        const QString id{m_plan->item(row, 0)->data(Qt::UserRole).toString()};
        callThen("cancelmeltcast", Args({id.toStdString()}), [this](const UniValue&) {
            say(tr("The step is cancelled."));
            m_plan_listed.clear();
            refresh();
        });
    });
    connect(cancel_all, &QPushButton::clicked, this, [this] {
        if (!confirm(tr("Cancel all"), tr("Cancel every step of the plan that has not gone yet? Coins waiting to melt are unlocked."))) return;
        callThen("cancelmeltcast", Args({std::string{"all"}}), [this](const UniValue& r) {
            say(tr("%n step(s) cancelled.", "", r.getInt<int>()));
            m_plan_listed.clear();
            refresh();
        });
    });

    auto* timer{new QTimer(this)};
    connect(timer, &QTimer::timeout, this, [this] {
        if (isVisible()) refresh();
    });
    timer->start(5000);
    updateSendHint();
    updateCastBills();
}

void ShieldedPage::setClientModel(ClientModel* client_model) { m_client_model = client_model; }

void ShieldedPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
    if (m_address->text().isEmpty()) {
        callThen("getshieldedaddress", UniValue{UniValue::VARR}, [this](const UniValue& a) {
            if (m_address->text().isEmpty()) showAddress(Text(a));
        }, /*quiet=*/true);
    }
}

void ShieldedPage::callThen(const std::string& method, const UniValue& params, std::function<void(const UniValue&)> then, bool quiet, std::function<void()> finally)
{
    // Off the GUI thread, always: the wallet may be busy (proving, scanning) and the node waiting
    // on its queue; the page must not freeze meanwhile.
    const auto wallet{m_client_model ? m_wallet_name() : std::nullopt};
    if (!wallet) {
        if (m_client_model && !quiet) say(tr("Open or create a wallet first."), true);
        if (finally) finally();
        return;
    }
    NodeRpc::CallAsync(this, m_client_model, method, params, [this, quiet, then = std::move(then), finally = std::move(finally)](std::optional<UniValue> result, const QString& error) {
        if (result) {
            then(*result);
        } else if (!quiet) {
            say(error, true);
        }
        if (finally) finally();
    }, wallet);
}

void ShieldedPage::callAsync(const std::string& method, const UniValue& params, QPushButton* button, const QString& working,
                             std::function<void(const UniValue&)> success)
{
    if (!m_client_model) return;
    const auto wallet{m_wallet_name()};
    if (!wallet) return say(tr("Open or create a wallet first."), true);
    // The wallet is busy proving: the lists wait for it rather than the GUI thread (refresh).
    ++m_calls_running;
    button->setEnabled(false);
    say(working);
    QPointer<QPushButton> guard{button};
    NodeRpc::CallAsync(this, m_client_model, method, params, [this, guard, success = std::move(success)](std::optional<UniValue> result, const QString& error) {
        --m_calls_running;
        if (guard) guard->setEnabled(true);
        if (!result) return say(error, true);
        success(*result);
        refresh();
    }, wallet);
}

void ShieldedPage::say(const QString& text, bool error)
{
    m_status->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(error ? RED : GREEN)));
    m_status->setText(text);
}

bool ShieldedPage::confirm(const QString& title, const QString& text)
{
    m_busy = true;
    const bool yes{QMessageBox::question(this, title, text) == QMessageBox::Yes};
    m_busy = false;
    return yes;
}

void ShieldedPage::showAddress(const QString& address)
{
    m_address->setText(address);
    m_address->setCursorPosition(0);
    m_qr->setQR(address);
}

void ShieldedPage::updateSendHint()
{
    const QString to{m_send_to->text().trimmed()};
    if (to.isEmpty() || IsShieldedAddress(to)) {
        m_send_hint->setText(tr("Private: nobody but the recipient sees the amount, the memo, or that you paid."));
        m_send_hint->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(GREEN)));
        m_send_memo->setEnabled(true);
    } else {
        m_send_hint->setText(tr("To a transparent address: this unshields. The amount and the address are public; where the coins came from is not. No memo."));
        m_send_hint->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(ORANGE)));
        m_send_memo->setEnabled(false);
    }
}

void ShieldedPage::updateCastBills()
{
    const int64_t sats{Satoshis(m_cast_amount->text())};
    if (sats < BILL_UNIT) {
        m_cast_bills->setText(tr("Bills: 0.01, 0.02, 0.04 … 10485.76 CHN. Each goes at its own blocks, the same for every wallet."));
        return;
    }
    QStringList bills;
    const int64_t units{sats / BILL_UNIT};
    if (units >= (int64_t{1} << NUM_BILLS)) {
        m_cast_bills->setText(tr("At most %1 CHN at a time.").arg(Chn(((int64_t{1} << NUM_BILLS) - 1) * BILL_UNIT)));
        return;
    }
    for (int k{NUM_BILLS - 1}; k >= 0; --k) {
        if (units & (int64_t{1} << k)) bills << Chn(BILL_UNIT << k);
    }
    const int64_t rest{sats - units * BILL_UNIT};
    m_cast_bills->setText(tr("Comes out as %n bill(s): %1 CHN", "", bills.size()).arg(bills.join(QStringLiteral(" + "))) +
                          (rest > 0 ? tr("; %1 CHN stays shielded").arg(Chn(rest)) : QString{}) + tr(", plus the fees, out of the shielded balance."));
}

void ShieldedPage::refresh()
{
    if (!m_client_model || m_busy || m_calls_running > 0) return;
    // One round at a time; one asked for meanwhile follows it.
    if (m_refreshing > 0) {
        m_refresh_again = true;
        return;
    }
    if (!m_wallet_name()) return;
    m_refresh_again = false;
    // The calls of a round, each answered on the GUI thread; the round ends with the last.
    const auto done{[this] {
        if (--m_refreshing == 0 && m_refresh_again) refresh();
    }};
    const auto ask{[&](const std::string& method, const UniValue& params, std::function<void(const UniValue&)> then) {
        ++m_refreshing;
        callThen(method, params, std::move(then), /*quiet=*/true, done);
    }};
    ask("getshieldedbalance", UniValue{UniValue::VARR}, [this](const UniValue& b) {
        m_available->setText(tr("%1 CHN").arg(Text(b["available"])));
        const double pending{b["pending"].get_real()};
        QString more{pending > 0 ? tr("%1 CHN waiting for a block").arg(Text(b["pending"])) : tr("in %1 notes").arg(Text(b["notes"]))};
        if (!b["rescanning_from"].isNull()) more += tr(" — scanning the blocks from %1 again for it").arg(Text(b["rescanning_from"]));
        m_pending->setText(more);
    });
    ask("getbalance", UniValue{UniValue::VARR}, [this](const UniValue& t) {
        m_transparent->setText(tr("%1 CHN").arg(Text(t)));
    });
    ask("getshieldedinfo", UniValue{UniValue::VARR}, [this](const UniValue& p) {
        m_pool->setText(tr("%1 CHN in %2 notes").arg(Text(p["pool"]), Text(p["notes"])));
    });
    ask("listshieldedtransactions", Args({200}), [this](const UniValue& h) { showHistory(h); });
    ask("listmeltcast", UniValue{UniValue::VARR}, [this](const UniValue& p) { showPlan(p); });
    if (m_notes->isVisible()) ask("listshieldednotes", Args({false}), [this](const UniValue& n) { showNotes(n); });
}

void ShieldedPage::showHistory(const UniValue& history)
{
    const std::string listed{history.write()};
    if (listed != m_history_listed) {
        m_history_listed = listed;
        m_history->setRowCount(0);
        for (const UniValue& h : history.getValues()) {
            const double received{h["received"].get_real()}, spent{h["spent"].get_real()};
            const double shielded{h["shielded"].get_real()}, unshielded{h["unshielded"].get_real()};
            QString what;
            const char* colour{nullptr};
            double amount{0};
            QStringList memos;
            for (const UniValue& m : h["memos"].getValues()) memos << Text(m);
            if (shielded > 0) {
                what = tr("Shielded");
                amount = shielded;
                colour = PURPLE;
            } else if (spent > 0) {
                // Paid out: to others (and unshielded), the rest came back as change.
                QStringList to;
                for (const UniValue& p : h["payments"].getValues()) {
                    if (p["to_self"].isTrue()) continue;
                    to << Text(p["address"]).left(14) + QStringLiteral("…");
                    if (!Text(p["memo"]).isEmpty()) memos << Text(p["memo"]);
                }
                what = unshielded > 0 && to.isEmpty() ? tr("Unshielded") : tr("Paid %1").arg(to.join(QStringLiteral(", ")));
                amount = -(spent - received);
                colour = RED;
            } else {
                what = tr("Received");
                amount = received;
                colour = GREEN;
            }
            const int row{m_history->rowCount()};
            m_history->insertRow(row);
            m_history->setItem(row, 0, Item(QDateTime::fromSecsSinceEpoch(h["time"].getInt<int64_t>()).toString(QStringLiteral("yyyy-MM-dd hh:mm"))));
            m_history->setItem(row, 1, Item(what));
            auto* amount_item{Item(QString::number(amount, 'f', 8))};
            amount_item->setForeground(QColor{colour});
            m_history->setItem(row, 2, amount_item);
            m_history->setItem(row, 3, Item(memos.join(QStringLiteral(" · "))));
            const int confirmations{h["confirmations"].getInt<int>()};
            auto* conf{Item(confirmations == 0 ? tr("waiting") : QString::number(confirmations))};
            if (confirmations == 0) conf->setForeground(QColor{ORANGE});
            m_history->setItem(row, 4, conf);
            m_history->item(row, 0)->setToolTip(Text(h["txid"]));
        }
        if (m_history->rowCount() == 0) {
            m_history->insertRow(0);
            m_history->setItem(0, 1, Item(tr("Nothing yet: shield some coins to start.")));
        }
    }
}

void ShieldedPage::showPlan(const UniValue& plan)
{
    const std::string listed{plan.write()};
    // Not while a step is selected: a rebuild would lose it.
    if (listed != m_plan_listed && !(m_plan->hasFocus() && m_plan->currentRow() >= 0)) {
        m_plan_listed = listed;
        const int tip{m_client_model ? m_client_model->getNumBlocks() : 0};
        m_plan->setRowCount(0);
        // What is still to go first, then what went, the latest first.
        std::vector<const UniValue*> steps;
        for (const UniValue& s : plan.getValues()) steps.push_back(&s);
        std::stable_sort(steps.begin(), steps.end(), [](const UniValue* a, const UniValue* b) {
            const bool sa{Text((*a)["status"]) == QLatin1String("sent")}, sb{Text((*b)["status"]) == QLatin1String("sent")};
            if (sa != sb) return !sa;
            return sa ? (*a)["id"].getInt<int64_t>() > (*b)["id"].getInt<int64_t>() : (*a)["height"].getInt<int>() < (*b)["height"].getInt<int>();
        });
        for (const UniValue* s : steps) {
            const UniValue& step{*s};
            const bool is_melt{Text(step["kind"]) == QLatin1String("melt")};
            const QString status{Text(step["status"])};
            const int row{m_plan->rowCount()};
            m_plan->insertRow(row);
            auto* kind{Item(is_melt ? tr("Melt") : tr("Cast"))};
            kind->setForeground(QColor{is_melt ? PURPLE : GREEN});
            if (status != QLatin1String("sent")) kind->setData(Qt::UserRole, Text(step["id"]));
            m_plan->setItem(row, 0, kind);
            m_plan->setItem(row, 1, Item(Text(step["amount"])));
            QString when;
            QString state;
            const char* colour{nullptr};
            if (status == QLatin1String("sent")) {
                const int confirmations{step["confirmations"].getInt<int>()};
                state = confirmations > 0 ? tr("done") : tr("sent");
                when = confirmations > 0 ? tr("%n confirmation(s)", "", confirmations) : tr("waiting for a block");
                colour = confirmations > 0 ? GREEN : ORANGE;
                m_plan->setItem(row, 4, Item(Text(step["txid"])));
            } else {
                const int height{step["height"].getInt<int>()};
                state = status == QLatin1String("waiting") ? tr("waiting") : tr("planned");
                when = height <= tip ? tr("now") : tr("at block %1, in %n block(s)", "", height - tip).arg(height);
                if (status == QLatin1String("waiting")) {
                    colour = ORANGE;
                    when += QStringLiteral(" — ") + Text(step["error"]);
                }
            }
            auto* state_item{Item(state)};
            if (colour) state_item->setForeground(QColor{colour});
            m_plan->setItem(row, 2, state_item);
            m_plan->setItem(row, 3, Item(when));
            if (is_melt) m_plan->item(row, 1)->setToolTip(tr("The coin %1").arg(Text(step["coin"])));
        }
        if (m_plan->rowCount() == 0) {
            m_plan->insertRow(0);
            m_plan->setItem(0, 3, Item(tr("Nothing planned.")));
        }
    }
}

void ShieldedPage::showNotes(const UniValue& notes)
{
    const std::string listed{notes.write()};
    if (listed != m_notes_listed) {
        m_notes_listed = listed;
        m_notes->setRowCount(0);
        for (const UniValue& n : notes.getValues()) {
            const int row{m_notes->rowCount()};
            m_notes->insertRow(row);
            m_notes->setItem(row, 0, Item(Text(n["amount"])));
            m_notes->setItem(row, 1, Item(Text(n["memo"])));
            m_notes->setItem(row, 2, Item(n["change"].isTrue() ? tr("change") : tr("received")));
            m_notes->setItem(row, 3, Item(Text(n["confirmations"])));
            m_notes->setItem(row, 4, Item(Text(n["txid"])));
        }
    }
}
