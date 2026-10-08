// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/multisigdialog.h>

#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/itemviews.h>

#include <QApplication>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {
using NodeRpc::Args;
using NodeRpc::Item;
using NodeRpc::Text;

const char* const SETTING_KEYS{"MultisigKeys"};
const char* const SETTING_ADDRESSES{"MultisigAddresses"};
//! Columns of the table of shared addresses.
enum { COL_NAME, COL_SIGNERS, COL_BALANCE, COL_ADDRESS, COL_DESCRIPTOR };

QTableWidget* Table(const QStringList& headers, QWidget* parent, const char* name)
{
    auto* table{new ItemViews::Table(0, headers.size(), parent)};
    table->setObjectName(name);
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

/** A descriptor without its checksum. */
QString Bare(const QString& descriptor) { return descriptor.section('#', 0, 0); }
} // namespace

MultisigDialog::MultisigDialog(NodeRpc::WalletNameFn wallet_name, QWidget* parent)
    : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMinMaxButtonsHint), m_wallet_name{std::move(wallet_name)}
{
    setWindowTitle(tr("Multisig Lounge"));
    auto* layout{new QVBoxLayout(this)};
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName("multisigTabs");
    layout->addWidget(m_tabs);

    // Keys
    auto* keys_tab{new QWidget(m_tabs)};
    auto* keys_layout{new QVBoxLayout(keys_tab)};
    auto* keys_intro{new QLabel(tr("Collect the public keys of the people who will share an address: one of your own wallet, which you send "
                                   "to your partners, and theirs, which they send to you. Public keys are safe to share."), keys_tab)};
    keys_intro->setWordWrap(true);
    keys_layout->addWidget(keys_intro);
    m_keys = Table({tr("Name"), tr("Public key")}, keys_tab, "multisigKeys");
    keys_layout->addWidget(m_keys, 1);
    auto* key_row{new QHBoxLayout};
    m_key_name = new QLineEdit(keys_tab);
    m_key_name->setObjectName("multisigKeyName");
    m_key_name->setPlaceholderText(tr("Name"));
    m_key = new QLineEdit(keys_tab);
    m_key->setObjectName("multisigKey");
    m_key->setPlaceholderText(tr("Public key of a partner, as they copied it from this window"));
    auto* add_partner{new QPushButton(tr("Add partner"), keys_tab)};
    key_row->addWidget(m_key_name, 1);
    key_row->addWidget(m_key, 3);
    key_row->addWidget(add_partner);
    keys_layout->addLayout(key_row);
    auto* key_buttons{new QHBoxLayout};
    auto* add_own{new QPushButton(tr("Add a key of my wallet"), keys_tab)};
    add_own->setObjectName("multisigAddOwn");
    auto* copy_key{new QPushButton(tr("Copy key"), keys_tab)};
    auto* remove_key{new QPushButton(tr("Remove"), keys_tab)};
    key_buttons->addWidget(add_own);
    key_buttons->addWidget(copy_key);
    key_buttons->addWidget(remove_key);
    key_buttons->addStretch();
    keys_layout->addLayout(key_buttons);
    m_tabs->addTab(keys_tab, tr("Keys"));

    // Shared addresses
    auto* addresses_tab{new QWidget(m_tabs)};
    auto* addresses_layout{new QVBoxLayout(addresses_tab)};
    auto* create{new QGroupBox(tr("New shared address"), addresses_tab)};
    auto* create_layout{new QHBoxLayout(create)};
    m_signers = new ItemViews::List(create);
    m_signers->setObjectName("multisigSigners");
    m_signers->setToolTip(tr("Tick the keys that share the address."));
    create_layout->addWidget(m_signers, 1);
    auto* create_form{new QFormLayout};
    m_required = new QSpinBox(create);
    m_required->setObjectName("multisigRequired");
    m_required->setRange(1, 15);
    m_required->setValue(2);
    create_form->addRow(tr("Signatures needed to spend:"), m_required);
    m_address_name = new QLineEdit(create);
    m_address_name->setObjectName("multisigAddressName");
    create_form->addRow(tr("Name:"), m_address_name);
    auto* create_button{new QPushButton(tr("Create address"), create)};
    create_button->setObjectName("multisigCreate");
    create_form->addRow(create_button);
    create_layout->addLayout(create_form, 1);
    addresses_layout->addWidget(create);
    auto* addresses_intro{new QLabel(tr("Every partner needs the same shared address in this window: send them its descriptor, which they "
                                        "add with \"Add from descriptor\". Pay to the address like to any other."), addresses_tab)};
    addresses_intro->setWordWrap(true);
    addresses_layout->addWidget(addresses_intro);
    m_addresses = Table({tr("Name"), tr("Signers"), tr("Balance"), tr("Address"), tr("Descriptor")}, addresses_tab, "multisigAddresses");
    addresses_layout->addWidget(m_addresses, 1);
    auto* address_buttons{new QHBoxLayout};
    auto* copy_address{new QPushButton(tr("Copy address"), addresses_tab)};
    auto* copy_descriptor{new QPushButton(tr("Copy descriptor"), addresses_tab)};
    auto* import_button{new QPushButton(tr("Add from descriptor…"), addresses_tab)};
    auto* refresh_button{new QPushButton(tr("Refresh balances"), addresses_tab)};
    auto* remove_address{new QPushButton(tr("Remove"), addresses_tab)};
    for (auto* button : {copy_address, copy_descriptor, import_button, refresh_button, remove_address}) address_buttons->addWidget(button);
    address_buttons->addStretch();
    addresses_layout->addLayout(address_buttons);
    m_tabs->addTab(addresses_tab, tr("Shared addresses"));

    // Spend
    auto* spend_tab{new QWidget(m_tabs)};
    auto* spend_layout{new QVBoxLayout(spend_tab)};
    auto* spend{new QGroupBox(tr("1. One partner prepares the transaction"), spend_tab)};
    auto* spend_form{new QFormLayout(spend)};
    m_from = new QComboBox(spend);
    m_from->setObjectName("multisigFrom");
    spend_form->addRow(tr("Spend from:"), m_from);
    m_destination = new QLineEdit(spend);
    m_destination->setObjectName("multisigDestination");
    spend_form->addRow(tr("Pay to address:"), m_destination);
    m_amount = new QLineEdit(spend);
    m_amount->setObjectName("multisigAmount");
    m_amount->setPlaceholderText(QStringLiteral("0.00000000"));
    spend_form->addRow(tr("Amount (CHN):"), m_amount);
    m_fee = new QLineEdit(QStringLiteral("0.0001"), spend);
    m_fee->setObjectName("multisigFee");
    spend_form->addRow(tr("Fee (CHN):"), m_fee);
    auto* create_tx{new QPushButton(tr("Prepare transaction"), spend)};
    create_tx->setObjectName("multisigPrepare");
    spend_form->addRow(create_tx);
    spend_layout->addWidget(spend);
    auto* sign_box{new QGroupBox(tr("2. Each partner signs it and passes it on, until it has enough signatures"), spend_tab)};
    auto* sign_layout{new QVBoxLayout(sign_box)};
    m_psbt = new QPlainTextEdit(sign_box);
    m_psbt->setObjectName("multisigPsbt");
    m_psbt->setPlaceholderText(tr("The transaction, as text to pass around (PSBT). Paste the one you received here."));
    m_psbt->setFont(GUIUtil::fixedPitchFont());
    sign_layout->addWidget(m_psbt, 1);
    m_psbt_status = new QLabel(sign_box);
    m_psbt_status->setObjectName("multisigPsbtStatus");
    m_psbt_status->setWordWrap(true);
    sign_layout->addWidget(m_psbt_status);
    auto* sign_buttons{new QHBoxLayout};
    auto* sign_button{new QPushButton(tr("Sign with my wallet"), sign_box)};
    sign_button->setObjectName("multisigSign");
    auto* copy_psbt{new QPushButton(tr("Copy"), sign_box)};
    auto* broadcast_button{new QPushButton(tr("Send transaction"), sign_box)};
    broadcast_button->setObjectName("multisigBroadcast");
    sign_buttons->addWidget(sign_button);
    sign_buttons->addWidget(copy_psbt);
    sign_buttons->addStretch();
    sign_buttons->addWidget(broadcast_button);
    sign_layout->addLayout(sign_buttons);
    spend_layout->addWidget(sign_box, 1);
    m_tabs->addTab(spend_tab, tr("Spend"));

    connect(add_own, &QPushButton::clicked, this, &MultisigDialog::addOwnKey);
    connect(add_partner, &QPushButton::clicked, this, &MultisigDialog::addPartnerKey);
    connect(remove_key, &QPushButton::clicked, this, &MultisigDialog::removeKey);
    connect(copy_key, &QPushButton::clicked, this, [this] {
        if (m_keys->currentRow() >= 0) GUIUtil::setClipboard(m_keys->item(m_keys->currentRow(), 1)->text());
    });
    connect(create_button, &QPushButton::clicked, this, &MultisigDialog::createAddress);
    connect(import_button, &QPushButton::clicked, this, &MultisigDialog::importAddress);
    connect(remove_address, &QPushButton::clicked, this, &MultisigDialog::removeAddress);
    connect(refresh_button, &QPushButton::clicked, this, &MultisigDialog::refreshBalances);
    connect(copy_address, &QPushButton::clicked, this, [this] {
        if (m_addresses->currentRow() >= 0) GUIUtil::setClipboard(m_addresses->item(m_addresses->currentRow(), COL_ADDRESS)->text());
    });
    connect(copy_descriptor, &QPushButton::clicked, this, [this] {
        if (m_addresses->currentRow() >= 0) GUIUtil::setClipboard(m_addresses->item(m_addresses->currentRow(), COL_DESCRIPTOR)->text());
    });
    connect(create_tx, &QPushButton::clicked, this, &MultisigDialog::createTransaction);
    connect(sign_button, &QPushButton::clicked, this, &MultisigDialog::sign);
    connect(broadcast_button, &QPushButton::clicked, this, &MultisigDialog::broadcast);
    connect(copy_psbt, &QPushButton::clicked, this, [this] { GUIUtil::setClipboard(m_psbt->toPlainText().trimmed()); });
    connect(m_psbt, &QPlainTextEdit::textChanged, this, &MultisigDialog::updateSignatureCount);

    resize(900, 620);
    GUIUtil::handleCloseWindowShortcut(this);
}

MultisigDialog::Entries MultisigDialog::Load(const char* setting)
{
    Entries entries;
    const QStringList saved{QSettings().value(setting).toStringList()};
    for (const QString& entry : saved) entries.push_back({entry.section('\t', 0, 0), entry.section('\t', 1)});
    return entries;
}

void MultisigDialog::Store(const char* setting, const Entries& entries)
{
    QStringList saved;
    for (const auto& [name, value] : entries) saved << name + '\t' + value;
    QSettings().setValue(setting, saved);
}

void MultisigDialog::warn(const QString& text)
{
    QMessageBox::warning(this, windowTitle(), text);
}

void MultisigDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    reload();
}

void MultisigDialog::reload()
{
    const Entries keys{Load(SETTING_KEYS)};
    m_keys->setRowCount(keys.size());
    m_signers->clear();
    for (int row{0}; row < keys.size(); ++row) {
        m_keys->setItem(row, 0, Item(keys[row].first));
        m_keys->setItem(row, 1, Item(keys[row].second));
        auto* signer{new QListWidgetItem(keys[row].first, m_signers)};
        signer->setFlags(signer->flags() | Qt::ItemIsUserCheckable);
        signer->setCheckState(Qt::Unchecked);
        signer->setData(Qt::UserRole, keys[row].second);
    }
    m_keys->resizeColumnToContents(0);

    const Entries addresses{Load(SETTING_ADDRESSES)};
    const QString chosen{m_from->currentData().toString()};
    m_addresses->setRowCount(addresses.size());
    m_from->clear();
    static const QRegularExpression threshold{QStringLiteral("multi\\((\\d+),")};
    for (int row{0}; row < addresses.size(); ++row) {
        const QString& descriptor{addresses[row].second};
        QString error;
        const auto derived{NodeRpc::Call(m_client_model, "deriveaddresses", Args({descriptor.toStdString()}), error)};
        const QString address{derived && derived->size() == 1 ? Text((*derived)[0]) : QString{}};
        m_addresses->setItem(row, COL_NAME, Item(addresses[row].first));
        m_addresses->setItem(row, COL_SIGNERS, Item(tr("%1 of %2").arg(threshold.match(descriptor).captured(1)).arg(descriptor.count(',') )));
        m_addresses->setItem(row, COL_BALANCE, Item(QStringLiteral("?")));
        m_addresses->setItem(row, COL_ADDRESS, Item(address));
        m_addresses->setItem(row, COL_DESCRIPTOR, Item(descriptor));
        m_from->addItem(addresses[row].first, descriptor);
    }
    m_from->setCurrentIndex(std::max(0, m_from->findData(chosen)));
    m_addresses->resizeColumnsToContents();
}

void MultisigDialog::addOwnKey()
{
    const auto wallet{m_wallet_name()};
    if (!wallet) return warn(tr("Open or create a wallet first."));
    QString error;
    const auto address{NodeRpc::Call(m_client_model, "getnewaddress", Args({"multisig key", "bech32"}), error, wallet)};
    const auto info{address ? NodeRpc::Call(m_client_model, "getaddressinfo", Args({*address}), error, wallet) : std::nullopt};
    if (!info) return warn(error);
    // The key with the path it has in the wallet, which is what lets the wallet find it again to sign.
    static const QRegularExpression key_expression{QStringLiteral("^wpkh\\((.+)\\)#")};
    const QString key{key_expression.match(Text((*info)["desc"])).captured(1)};
    if (key.isEmpty()) return warn(tr("The wallet did not give a key that can be used here."));
    Entries keys{Load(SETTING_KEYS)};
    const QString name{m_key_name->text().trimmed().isEmpty() ? tr("Me (%1)").arg(wallet->isEmpty() ? tr("default wallet") : *wallet) : m_key_name->text().trimmed()};
    keys.push_back({name, key});
    Store(SETTING_KEYS, keys);
    m_key_name->clear();
    reload();
}

void MultisigDialog::addPartnerKey()
{
    const QString name{m_key_name->text().trimmed()};
    const QString key{m_key->text().trimmed()};
    if (name.isEmpty() || key.isEmpty()) return warn(tr("Enter the name of the partner and their public key."));
    QString error;
    if (!NodeRpc::Call(m_client_model, "getdescriptorinfo", Args({"wpkh(" + key.toStdString() + ")"}), error)) {
        return warn(tr("This is not a public key: %1").arg(error));
    }
    Entries keys{Load(SETTING_KEYS)};
    keys.push_back({name, key});
    Store(SETTING_KEYS, keys);
    m_key_name->clear();
    m_key->clear();
    reload();
}

void MultisigDialog::removeKey()
{
    Entries keys{Load(SETTING_KEYS)};
    const int row{m_keys->currentRow()};
    if (row < 0 || row >= keys.size()) return;
    keys.removeAt(row);
    Store(SETTING_KEYS, keys);
    reload();
}

bool MultisigDialog::addAddress(const QString& name, const QString& descriptor)
{
    QString error;
    const auto info{NodeRpc::Call(m_client_model, "getdescriptorinfo", Args({Bare(descriptor).toStdString()}), error)};
    if (!info) {
        warn(error);
        return false;
    }
    const QString checked{Bare(descriptor) + '#' + Text((*info)["checksum"])};
    Entries addresses{Load(SETTING_ADDRESSES)};
    for (const auto& entry : addresses) {
        if (entry.second == checked) {
            warn(tr("This shared address is already in the list, as \"%1\".").arg(entry.first));
            return false;
        }
    }
    addresses.push_back({name, checked});
    Store(SETTING_ADDRESSES, addresses);
    reload();
    return true;
}

void MultisigDialog::createAddress()
{
    QStringList keys;
    for (int i{0}; i < m_signers->count(); ++i) {
        if (m_signers->item(i)->checkState() == Qt::Checked) keys << m_signers->item(i)->data(Qt::UserRole).toString();
    }
    if (keys.size() < 2) return warn(tr("Tick at least two keys. Add them on the Keys tab first."));
    if (m_required->value() > keys.size()) return warn(tr("More signatures are needed than there are keys."));
    const QString name{m_address_name->text().trimmed()};
    if (name.isEmpty()) return warn(tr("Give the shared address a name."));
    // The keys are sorted by the descriptor, so that all partners get the same address whatever their order.
    if (addAddress(name, QStringLiteral("wsh(sortedmulti(%1,%2))").arg(m_required->value()).arg(keys.join(',')))) m_address_name->clear();
}

void MultisigDialog::importAddress()
{
    bool ok;
    const QString descriptor{QInputDialog::getText(this, windowTitle(), tr("Descriptor of the shared address, as a partner copied it:"), QLineEdit::Normal, {}, &ok).trimmed()};
    if (!ok || descriptor.isEmpty()) return;
    if (!descriptor.startsWith("wsh(sortedmulti(")) return warn(tr("This is not the descriptor of a shared address made in this window."));
    const QString name{QInputDialog::getText(this, windowTitle(), tr("Name for this shared address:"), QLineEdit::Normal, {}, &ok).trimmed()};
    if (!ok || name.isEmpty()) return;
    addAddress(name, descriptor);
}

void MultisigDialog::removeAddress()
{
    Entries addresses{Load(SETTING_ADDRESSES)};
    const int row{m_addresses->currentRow()};
    if (row < 0 || row >= addresses.size()) return;
    if (QMessageBox::question(this, windowTitle(), tr("Remove \"%1\" from the list? Coins on the address stay there, but spending them needs its descriptor; make sure it is kept somewhere.").arg(addresses[row].first)) != QMessageBox::Yes) return;
    addresses.removeAt(row);
    Store(SETTING_ADDRESSES, addresses);
    reload();
}

void MultisigDialog::refreshBalances()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (int row{0}; row < m_addresses->rowCount(); ++row) {
        UniValue descriptors(UniValue::VARR);
        descriptors.push_back(m_addresses->item(row, COL_DESCRIPTOR)->text().toStdString());
        QString error;
        const auto scan{NodeRpc::Call(m_client_model, "scantxoutset", Args({"start", descriptors}), error)};
        m_addresses->setItem(row, COL_BALANCE, Item(scan ? Text((*scan)["total_amount"]) : QStringLiteral("?")));
    }
    QApplication::restoreOverrideCursor();
    m_addresses->resizeColumnsToContents();
}

void MultisigDialog::createTransaction()
{
    const QString descriptor{m_from->currentData().toString()};
    if (descriptor.isEmpty()) return warn(tr("There is no shared address to spend from. Make one on the Shared addresses tab."));
    static const QRegularExpression amount_expression{QStringLiteral("^\\d{1,8}(\\.\\d{1,8})?$")};
    const QString amount{m_amount->text().trimmed()};
    const QString fee{m_fee->text().trimmed()};
    if (!amount_expression.match(amount).hasMatch() || !amount_expression.match(fee).hasMatch()) return warn(tr("Enter the amount and the fee as numbers, like 1.5"));
    const QString destination{m_destination->text().trimmed()};

    QString error;
    UniValue descriptors(UniValue::VARR);
    descriptors.push_back(descriptor.toStdString());
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto scan{NodeRpc::Call(m_client_model, "scantxoutset", Args({"start", descriptors}), error)};
    QApplication::restoreOverrideCursor();
    const auto derived{NodeRpc::Call(m_client_model, "deriveaddresses", Args({descriptor.toStdString()}), error)};
    if (!scan || !derived) return warn(error);

    // All coins of the address are spent, and what is left goes back to it.
    // Amounts are counted in the smallest unit to keep the arithmetic exact.
    const auto units{[](const QString& text) {
        const QStringList parts{text.split('.')};
        return parts[0].toLongLong() * 100000000LL + parts.value(1).leftJustified(8, '0').toLongLong();
    }};
    const auto text{[](qint64 value) { return QStringLiteral("%1.%2").arg(value / 100000000LL).arg(value % 100000000LL, 8, 10, QLatin1Char('0')); }};
    UniValue inputs(UniValue::VARR);
    qint64 available{0};
    for (const UniValue& coin : (*scan)["unspents"].getValues()) {
        UniValue input(UniValue::VOBJ);
        input.pushKV("txid", coin["txid"]);
        input.pushKV("vout", coin["vout"]);
        inputs.push_back(input);
        available += units(QString::fromStdString(coin["amount"].getValStr()).contains('e') ? QString::number(coin["amount"].get_real(), 'f', 8) : QString::fromStdString(coin["amount"].getValStr()));
    }
    const qint64 change{available - units(amount) - units(fee)};
    if (inputs.empty()) return warn(tr("The shared address holds no coins."));
    if (change < 0) return warn(tr("The shared address holds %1 CHN, which is less than the amount and the fee.").arg(text(available)));

    UniValue outputs(UniValue::VARR);
    UniValue payment(UniValue::VOBJ);
    payment.pushKV(destination.toStdString(), amount.toStdString());
    outputs.push_back(payment);
    // Change too small to be worth an output goes to the fee.
    if (change >= 1000) {
        UniValue change_output(UniValue::VOBJ);
        change_output.pushKV((*derived)[0].get_str(), text(change).toStdString());
        outputs.push_back(change_output);
    }
    const auto psbt{NodeRpc::Call(m_client_model, "createpsbt", Args({inputs, outputs}), error)};
    // Adds what the signers need to know about the coins and about the keys.
    const auto updated{psbt ? NodeRpc::Call(m_client_model, "utxoupdatepsbt", Args({*psbt, descriptors}), error) : std::nullopt};
    if (!updated) return warn(error);
    m_psbt->setPlainText(Text(*updated));
}

void MultisigDialog::updateSignatureCount()
{
    const std::string psbt{m_psbt->toPlainText().trimmed().toStdString()};
    if (psbt.empty()) {
        m_psbt_status->clear();
        return;
    }
    QString error;
    const auto decoded{NodeRpc::Call(m_client_model, "decodepsbt", Args({psbt}), error)};
    if (!decoded) {
        m_psbt_status->setText(tr("This is not a transaction to sign: %1").arg(error));
        return;
    }
    // The input with the fewest signatures tells how far the transaction is.
    static const QRegularExpression threshold{QStringLiteral("^(\\d+) ")};
    int signatures{-1};
    QString needed{QStringLiteral("?")};
    bool final{true};
    for (const UniValue& input : (*decoded)["inputs"].getValues()) {
        if (input.exists("final_scriptwitness")) continue;
        final = false;
        const int count{input.exists("partial_signatures") ? static_cast<int>(input["partial_signatures"].size()) : 0};
        if (signatures < 0 || count < signatures) signatures = count;
        if (input.exists("witness_script")) needed = threshold.match(Text(input["witness_script"]["asm"])).captured(1);
    }
    QStringList payments;
    // A PSBT of version 2 lists its outputs itself; an older one carries a transaction.
    const bool v2{!decoded->exists("tx")};
    for (const UniValue& output : (v2 ? (*decoded)["outputs"] : (*decoded)["tx"]["vout"]).getValues()) {
        const UniValue& script{output[v2 ? "script" : "scriptPubKey"]};
        payments << tr("%1 CHN to %2").arg(QString::number(output[v2 ? "amount" : "value"].get_real(), 'f', 8), script.isObject() && script.exists("address") ? Text(script["address"]) : tr("(no address)"));
    }
    const QString fee{decoded->exists("fee") ? QString::number((*decoded)["fee"].get_real(), 'f', 8) : QStringLiteral("?")};
    m_psbt_status->setText((final ? tr("Fully signed, ready to send.") : tr("Signatures: %1 of the %2 needed.").arg(signatures).arg(needed)) +
                           ' ' + tr("Pays %1. Fee: %2 CHN.").arg(payments.join(tr(", and ")), fee));
}

void MultisigDialog::sign()
{
    const auto wallet{m_wallet_name()};
    if (!wallet) return warn(tr("Open the wallet that holds your key first."));
    QString error;
    // Signs, without finalizing, so that the next partner can still add a signature.
    const auto result{NodeRpc::Call(m_client_model, "walletprocesspsbt", Args({m_psbt->toPlainText().trimmed().toStdString(), true, "ALL", true, false}), error, wallet)};
    if (!result) return warn(error);
    const QString before{m_psbt->toPlainText().trimmed()};
    m_psbt->setPlainText(Text((*result)["psbt"]));
    if (before == m_psbt->toPlainText()) {
        QMessageBox::information(this, windowTitle(), tr("This wallet added no signature: either it already signed, or it holds none of the keys of the shared address."));
    }
}

void MultisigDialog::broadcast()
{
    QString error;
    const auto final{NodeRpc::Call(m_client_model, "finalizepsbt", Args({m_psbt->toPlainText().trimmed().toStdString()}), error)};
    if (!final) return warn(error);
    if (!(*final)["complete"].get_bool()) return warn(tr("The transaction does not have enough signatures yet."));
    if (QMessageBox::question(this, windowTitle(), tr("Send this transaction? %1").arg(m_psbt_status->text())) != QMessageBox::Yes) return;
    const auto txid{NodeRpc::Call(m_client_model, "sendrawtransaction", Args({(*final)["hex"]}), error)};
    if (!txid) return warn(error);
    m_psbt->clear();
    QMessageBox::information(this, windowTitle(), tr("Sent as transaction %1").arg(Text(*txid)));
}
