// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/sidechaintests.h>

#include <addresstype.h>
#include <core_io.h>
#include <drivechain/sidechain.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <chainparams.h>
#include <key_io.h>
#include <node/context.h>
#include <policy/feerate.h>
#include <primitives/transaction.h>
#include <interfaces/mining.h>
#include <node/cpuminer.h>
#include <qt/bitcoinamountfield.h>
#include <qt/blockexplorer.h>
#include <qt/chainactivity.h>
#include <qt/cryptotoolsdialog.h>
#include <qt/miningdialog.h>
#include <qt/multisigdialog.h>
#include <qt/noderpc.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/proofoffundsdialog.h>
#include <qt/shieldedpage.h>
#include <qt/sidechainpage.h>
#include <qt/theme.h>
#include <qt/timestampdialog.h>
#include <qt/walletframe.h>
#include <qt/walletmodel.h>
#include <qt/walletview.h>
#include <rpc/server.h>
#include <script/descriptor.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <validation.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QDir>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QTimer>

#include <map>
#include <memory>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::CWallet;
using wallet::RemoveWallet;
using wallet::WalletContext;
using wallet::WalletDescriptor;
using wallet::WALLET_FLAG_DESCRIPTORS;

namespace {

//! Regtest drivechain parameters, see CRegTestParams.
constexpr int ACTIVATION_PERIOD{20};
constexpr int WITHDRAWAL_MIN_SCORE{30};

/**
 * Press a button of every message box that pops up while `action` runs.
 * @return the texts of the message boxes
 */
QStringList WithMessageBoxes(QMessageBox::StandardButton button, const std::function<void()>& action)
{
    QStringList texts;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [button, &texts] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* box{qobject_cast<QMessageBox*>(widget)};
            if (!box || !box->isVisible()) continue;
            texts << box->text();
            // An information box only has OK.
            QAbstractButton* choice{box->button(button)};
            if (!choice) choice = box->button(QMessageBox::Ok);
            if (choice) choice->click();
        }
    });
    timer.start(20);
    action();
    timer.stop();
    return texts;
}

/** Mine a block with the transactions in the mempool, parents before children. */
void MineBlock(TestChain100Setup& test)
{
    std::vector<CTransactionRef> left;
    for (const auto& info : test.m_node.mempool->infoAll()) left.push_back(info.tx);
    std::vector<CMutableTransaction> txs;
    while (!left.empty()) {
        const auto ready{std::find_if(left.begin(), left.end(), [&](const CTransactionRef& tx) {
            return std::none_of(tx->vin.begin(), tx->vin.end(), [&](const CTxIn& in) {
                return std::any_of(left.begin(), left.end(), [&](const CTransactionRef& parent) { return parent->GetHash() == in.prevout.hash; });
            });
        })};
        QVERIFY(ready != left.end());
        txs.emplace_back(**ready);
        left.erase(ready);
    }
    const CBlock block{test.CreateAndProcessBlock(txs, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()))};
    QCOMPARE(WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Tip()->GetBlockHash()), block.GetHash());
}

void SaveScreenshot(QWidget& widget, const QString& name)
{
    const QString dir{qEnvironmentVariable("CHAINS_GUI_SCREENSHOT_DIR")};
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    widget.grab().save(QDir(dir).filePath(name + ".png"));
}

/** The windows of the Tools menu. */
void TestTools(TestChain100Setup& test, ClientModel& client_model)
{
    const NodeRpc::WalletNameFn wallet_name{[] { return std::optional<QString>{QString{}}; }};

    // Hash calculator, merkle tree and address decoder
    CryptoToolsDialog tools;
    tools.setClientModel(&client_model);
    tools.findChild<QPlainTextEdit*>("hashInput")->setPlainText("abc");
    auto* hashes{tools.findChild<QPlainTextEdit*>("hashOutput")};
    QVERIFY(hashes->toPlainText().contains("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    QVERIFY(hashes->toPlainText().contains("4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358"));
    tools.findChild<QCheckBox*>("hashHex")->setChecked(true);
    tools.findChild<QPlainTextEdit*>("hashInput")->setPlainText("616263");
    QVERIFY(hashes->toPlainText().contains("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    const CBlockIndex* tip{WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Tip())};
    tools.findChild<QLineEdit*>("merkleBlock")->setText(QString::number(tip->nHeight));
    tools.findChild<QPushButton*>("merkleLoad")->click();
    const QString root{QString::fromStdString(tip->hashMerkleRoot.GetHex())};
    const QString merkle{tools.findChild<QPlainTextEdit*>("merkleOutput")->toPlainText()};
    QVERIFY(merkle.contains("Merkle root:\n  " + root));
    QVERIFY(merkle.contains("Merkle root in the block header: " + root));

    const QString address{QString::fromStdString(EncodeDestination(WitnessV0KeyHash{test.coinbaseKey.GetPubKey()}))};
    auto* decode{tools.findChild<QLineEdit*>("decodeAddress")};
    decode->setText(address);
    Q_EMIT decode->returnPressed();
    const QString decoded{tools.findChild<QPlainTextEdit*>("decodeOutput")->toPlainText()};
    QVERIFY(decoded.contains("A valid address"));
    QVERIFY(decoded.contains(QString::fromStdString("prefix: " + Params().Bech32HRP())));
    QVERIFY(decoded.contains(QString::fromStdString(HexStr(test.coinbaseKey.GetPubKey().GetID()))));
    SaveScreenshot(tools, "tools-address");

    // A transaction for the explorer to find
    QString send_error;
    const auto explorer_tx{NodeRpc::Call(&client_model, "sendtoaddress", NodeRpc::Args({address.toStdString(), 1}), send_error, QString{})};
    QVERIFY2(explorer_tx, qPrintable(send_error));
    const QString sent_txid{QString::fromStdString(explorer_tx->get_str())};
    MineBlock(test);

    // Block explorer
    BlockExplorer explorer;
    explorer.setClientModel(&client_model);
    explorer.refresh();
    auto* blocks{explorer.findChild<QTableWidget*>("explorerBlocks")};
    tip = WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Tip());
    QCOMPARE(blocks->rowCount(), 50);
    QCOMPARE(blocks->item(0, 0)->text(), QString::number(tip->nHeight));
    QCOMPARE(blocks->item(0, 6)->text(), QString::fromStdString(tip->GetBlockHash().GetHex()));
    QCOMPARE(blocks->item(49, 0)->text(), QString::number(tip->nHeight - 49));
    explorer.search(QString::number(tip->nHeight));
    auto* block_txs{explorer.findChild<QListWidget*>("explorerBlockTxs")};
    QCOMPARE(block_txs->count(), 2);
    QCOMPARE(block_txs->item(1)->text(), sent_txid);
    QVERIFY(explorer.findChild<QLabel*>("explorerTitle")->text().startsWith(QString("Block %1:").arg(tip->nHeight)));
    Q_EMIT block_txs->itemClicked(block_txs->item(1));
    QCOMPARE(explorer.findChild<QLabel*>("explorerTitle")->text(), "Transaction " + sent_txid);
    QVERIFY(explorer.findChild<QPlainTextEdit*>("explorerDetails")->toPlainText().contains("\"witness_v0_keyhash\""));
    explorer.search(blocks->item(3, 6)->text());
    QVERIFY(explorer.findChild<QLabel*>("explorerTitle")->text().startsWith(QString("Block %1:").arg(tip->nHeight - 3)));
    QCOMPARE(WithMessageBoxes(QMessageBox::Ok, [&] { explorer.search("not a block"); }).size(), 1);
    SaveScreenshot(explorer, "explorer");

    // Mining
    test.m_node.mining = interfaces::MakeMining(test.m_node);
    test.m_node.cpu_miner = std::make_unique<node::CpuMiner>(test.m_node);
    MiningDialog mining(wallet_name);
    mining.setClientModel(&client_model);
    mining.refresh();
    QCOMPARE(mining.findChild<QLabel*>("miningStatus")->text(), QString("Not mining"));
    QCOMPARE(mining.findChild<QLabel*>("miningHeight")->text(), QString::number(tip->nHeight));
    QVERIFY(!mining.findChild<QPushButton*>("miningStop")->isEnabled());
    mining.findChild<QLineEdit*>("miningAddress")->setText("not an address");
    QCOMPARE(WithMessageBoxes(QMessageBox::Ok, [&] { mining.findChild<QPushButton*>("miningStart")->click(); }).size(), 1);
    mining.findChild<QLineEdit*>("miningAddress")->setText(address);
    mining.findChild<QSpinBox*>("miningThreads")->setValue(1);
    mining.findChild<QPushButton*>("miningStart")->click();
    QCOMPARE(mining.findChild<QLabel*>("miningStatus")->text(), QString("Mining, threads: 1"));
    QVERIFY(mining.findChild<QPushButton*>("miningStop")->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Height()) >= tip->nHeight + 2, 30000);
    mining.refresh();
    QVERIFY(mining.findChild<QLabel*>("miningFound")->text().toInt() >= 2);
    SaveScreenshot(mining, "mining");
    mining.findChild<QPushButton*>("miningStop")->click();
    QCOMPARE(mining.findChild<QLabel*>("miningStatus")->text(), QString("Not mining"));
    const int height{WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Height())};
    QTest::qWait(1500);
    QCOMPARE(WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Height()), height);
    {
        // The miner paid the address it was given.
        LOCK(::cs_main);
        const CBlockIndex* mined{test.m_node.chainman->ActiveChain().Tip()};
        CBlock block;
        QVERIFY(test.m_node.chainman->m_blockman.ReadBlock(block, *mined));
        QCOMPARE(block.vtx[0]->vout[0].scriptPubKey, GetScriptForDestination(WitnessV0KeyHash{test.coinbaseKey.GetPubKey()}));
    }
    test.m_node.cpu_miner.reset();

    // Timestamp a file, then find its stamp
    QTemporaryFile file;
    QVERIFY(file.open());
    file.write("sidechain");
    file.close();
    TimestampDialog stamps(wallet_name);
    stamps.setClientModel(&client_model);
    stamps.setFile(file.fileName());
    QCOMPARE(stamps.findChild<QLabel*>("timestampHash")->text(), QString::fromLatin1(QCryptographicHash::hash("sidechain", QCryptographicHash::Sha256).toHex()));
    auto* stamp_result{stamps.findChild<QPlainTextEdit*>("timestampResult")};
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { stamps.stamp(); }).size(), 1);
    QVERIFY2(stamp_result->toPlainText().startsWith("The fingerprint was published in transaction"), qPrintable(stamp_result->toPlainText()));
    stamps.verify();
    QVERIFY(stamp_result->toPlainText().startsWith("No stamp of this file was found"));
    MineBlock(test);
    stamps.verify();
    const int stamp_height{WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Height())};
    QVERIFY2(stamp_result->toPlainText().startsWith(QString("This file was stamped in block %1,").arg(stamp_height)), qPrintable(stamp_result->toPlainText()));
    SaveScreenshot(stamps, "timestamp");

    // Proof of funds
    ProofOfFundsDialog proof(wallet_name);
    proof.setClientModel(&client_model);
    proof.findChild<QLineEdit*>("proofStatement")->setText("These coins are mine");
    WithMessageBoxes(QMessageBox::Ok, [&] { proof.prove(); });
    const QString proof_text{proof.findChild<QPlainTextEdit*>("proofOutput")->toPlainText()};
    QVERIFY2(proof_text.startsWith("-----BEGIN CHAINS PROOF OF FUNDS-----\nThese coins are mine\n-----SIGNATURES-----\n" + QString::fromStdString(Params().Bech32HRP()) + "1"), qPrintable(proof_text));
    auto* proof_input{proof.findChild<QPlainTextEdit*>("proofInput")};
    auto* verdict{proof.findChild<QPlainTextEdit*>("proofVerdict")};
    proof_input->setPlainText(proof_text);
    proof.verify();
    QVERIFY2(verdict->toPlainText().startsWith("VALID. The holder of"), qPrintable(verdict->toPlainText()));
    QVERIFY(!verdict->toPlainText().contains("with 0 CHN") && !verdict->toPlainText().contains("with 0.00000000 CHN"));
    SaveScreenshot(proof, "proof");
    proof_input->setPlainText(QString{proof_text}.replace("These coins are mine", "These coins are yours"));
    proof.verify();
    QVERIFY2(verdict->toPlainText().startsWith("NOT VALID."), qPrintable(verdict->toPlainText()));
    proof_input->setPlainText("hello");
    proof.verify();
    QCOMPARE(verdict->toPlainText(), QString("This is not a proof of funds."));

    // Multisig: a 2-of-2 address of two keys of the wallet, funded and spent from
    QSettings().remove("MultisigKeys");
    QSettings().remove("MultisigAddresses");
    MultisigDialog multisig(wallet_name);
    multisig.setClientModel(&client_model);
    multisig.addOwnKey();
    multisig.findChild<QLineEdit*>("multisigKeyName")->setText("Second key");
    multisig.addOwnKey();
    auto* keys{multisig.findChild<QTableWidget*>("multisigKeys")};
    QCOMPARE(keys->rowCount(), 2);
    QCOMPARE(keys->item(1, 0)->text(), QString("Second key"));
    QVERIFY(keys->item(0, 1)->text().startsWith("["));
    auto* signers{multisig.findChild<QListWidget*>("multisigSigners")};
    QCOMPARE(signers->count(), 2);
    auto* create_button{multisig.findChild<QPushButton*>("multisigCreate")};
    QCOMPARE(WithMessageBoxes(QMessageBox::Ok, [&] { create_button->click(); }).size(), 1); // no key ticked
    signers->item(0)->setCheckState(Qt::Checked);
    signers->item(1)->setCheckState(Qt::Checked);
    multisig.findChild<QLineEdit*>("multisigAddressName")->setText("Joint account");
    create_button->click();
    auto* shared{multisig.findChild<QTableWidget*>("multisigAddresses")};
    QCOMPARE(shared->rowCount(), 1);
    QCOMPARE(shared->item(0, 1)->text(), QString("2 of 2"));
    const QString shared_address{shared->item(0, 3)->text()};
    QVERIFY(shared_address.startsWith(QString::fromStdString(Params().Bech32HRP() + "1q")) && shared_address.size() == qsizetype(60 + Params().Bech32HRP().size()));

    QString rpc_error;
    QVERIFY2(NodeRpc::Call(&client_model, "sendtoaddress", NodeRpc::Args({shared_address.toStdString(), 3}), rpc_error, QString{}), qPrintable(rpc_error));
    MineBlock(test);
    multisig.refreshBalances();
    QCOMPARE(shared->item(0, 2)->text(), QString("3.00000000"));

    multisig.findChild<QLineEdit*>("multisigDestination")->setText(address);
    multisig.findChild<QLineEdit*>("multisigAmount")->setText("1");
    multisig.findChild<QPushButton*>("multisigPrepare")->click();
    auto* psbt_status{multisig.findChild<QLabel*>("multisigPsbtStatus")};
    QVERIFY2(psbt_status->text().startsWith("Signatures: 0 of the 2 needed. Pays 1.00000000 CHN to " + address + ", and 1.99990000 CHN to " + shared_address + ". Fee: 0.00010000 CHN."), qPrintable(psbt_status->text()));
    QCOMPARE(WithMessageBoxes(QMessageBox::Ok, [&] { multisig.broadcast(); }).size(), 1); // not signed yet
    multisig.sign();
    QVERIFY2(psbt_status->text().startsWith("Signatures: 2 of the 2 needed."), qPrintable(psbt_status->text()));
    SaveScreenshot(multisig, "multisig-keys");
    const QStringList sent{WithMessageBoxes(QMessageBox::Yes, [&] { multisig.broadcast(); })};
    QCOMPARE(sent.size(), 2);
    QVERIFY2(sent.at(1).startsWith("Sent as transaction"), qPrintable(sent.at(1)));
    MineBlock(test);
    multisig.refreshBalances();
    QCOMPARE(shared->item(0, 2)->text(), QString("1.99990000"));
    QSettings().remove("MultisigKeys");
    QSettings().remove("MultisigAddresses");

    // Themes
    // By default the text is a little larger than on the desktop, which takes a style sheet.
    QCOMPARE(Theme::SavedFontSizeAdjustment(), Theme::DEFAULT_FONT_SIZE_ADJUSTMENT);
    Theme::SaveFont({}, 0);
    const QPalette original_palette{QApplication::palette()};
    QCOMPARE(Theme::Available().size(), 18);
    Theme::Apply("midnight");
    QCOMPARE(QApplication::palette().color(QPalette::Window).name(), QString("#0d1117"));
    multisig.findChild<QTabWidget*>("multisigTabs")->setCurrentIndex(2);
    QApplication::processEvents();
    SaveScreenshot(multisig, "theme-midnight");
    Theme::Apply("solar");
    QApplication::processEvents();
    SaveScreenshot(explorer, "theme-gold");
    Theme::Apply("system");
    QCOMPARE(QApplication::palette().color(QPalette::Window), original_palette.color(QPalette::Window));
    QVERIFY(qApp->styleSheet().isEmpty());

    // Looks: the colours of a theme with a font, picked together
    const QList<Theme::Info> looks{Theme::Looks()};
    QCOMPARE(looks.size(), 18);
    QCOMPARE(looks.at(0).id, QString("system"));
    int look_changes{0};
    const auto counting{QObject::connect(Theme::Changes(), &Theme::Signals::changed, [&] { ++look_changes; })};
    Theme::SaveLook("matrix");
    QCOMPARE(look_changes, 1);
    QCOMPARE(Theme::Saved(), QString("matrix"));
    QCOMPARE(Theme::SavedLook(), QString("matrix"));
    QCOMPARE(QApplication::palette().color(QPalette::Window).name(), QString("#000000"));
    // The font of a look is one this computer has, or the one of the desktop.
    QVERIFY(Theme::SavedFontFamily().isEmpty() || QFontDatabase::families().contains(Theme::SavedFontFamily()));
    // Other colours with the same font are a combination of one's own, unless the fonts of both looks are the same here.
    Theme::Save("rose");
    QCOMPARE(look_changes, 2);
    // The size of the text is set apart from the look, and a look leaves it as it is.
    Theme::SaveLook("rose");
    QCOMPARE(Theme::SavedLook(), QString("rose"));
    Theme::SaveFont(Theme::SavedFontFamily(), 5);
    QCOMPARE(Theme::SavedLook(), QString("rose"));
    Theme::SaveLook("matrix");
    QCOMPARE(Theme::SavedFontSizeAdjustment(), 5);
    Theme::SaveFont(Theme::SavedFontFamily(), 0);
    Theme::SaveLook("no such look");
    QCOMPARE(look_changes, 6);
    Theme::SaveLook("system");
    QCOMPARE(Theme::SavedLook(), QString("system"));
    QCOMPARE(Theme::SavedFontFamily(), QString{});
    QCOMPARE(Theme::SavedFontSizeAdjustment(), 0);
    QVERIFY(qApp->styleSheet().isEmpty());
    QObject::disconnect(counting);
}

/** Wait until the calls queued on the thread of NodeRpc::CallAsync so far are answered on the GUI thread. */
void DrainNodeRpc()
{
    bool drained{false};
    QObject anchor;
    NodeRpc::RunAsync(&anchor, [&drained]() -> std::function<void()> { return [&drained] { drained = true; }; });
    QTRY_VERIFY_WITH_TIMEOUT(drained, 60000);
}

/**
 * The shielded page: it shows the account's address and balances, as the wallet has them, through
 * calls off the GUI thread; refreshes asked for while others are under way, or of a page deleted
 * meanwhile, are harmless. Shielding goes through the page's own Shield button. The test chain is
 * no sidechain, so the coins may not reach the shielded pool here (feature_zside.py covers that on
 * a sidechain); the page has to show what the wallet says either way.
 */
void TestShieldedPage(TestChain100Setup& test, ClientModel& client_model)
{
    const NodeRpc::WalletNameFn wallet_name{[] { return std::optional<QString>{QString{}}; }};
    // AppTests shut its node down, which stopped the thread the page calls the node on.
    NodeRpc::Restart();
    const auto call{[&](const std::string& method, const UniValue& params) {
        QString error;
        const auto result{NodeRpc::Call(&client_model, method, params, error, QString{})};
        if (!result) qWarning("%s: %s", method.c_str(), qPrintable(error));
        return result;
    }};

    // A page deleted while its first refresh is still queued: the answers are dropped.
    {
        auto* page{new ShieldedPage(wallet_name)};
        page->setClientModel(&client_model);
        page->show();
        delete page;
        DrainNodeRpc();
    }

    ShieldedPage page(wallet_name);
    page.resize(1000, 700);
    page.setClientModel(&client_model);
    page.show();
    auto* address{page.findChild<QLineEdit*>("address")};
    auto* available{page.findChild<QLabel*>("bigNumber")};
    auto* transparent{page.findChild<QLabel*>("midNumber")};
    auto* status{page.findChild<QLabel*>("shieldedStatus")};
    auto* tabs{page.findChild<QTabWidget*>()};
    QVERIFY(address && available && transparent && status && tabs);

    // The address and the balances, once the calls are answered.
    QTRY_VERIFY_WITH_TIMEOUT(!address->text().isEmpty(), 30000);
    const auto own_address{call("getshieldedaddress", UniValue{UniValue::VARR})};
    QVERIFY(own_address);
    QCOMPARE(address->text(), QString::fromStdString(own_address->get_str()));
    QVERIFY2(address->text().startsWith("rzo1"), qPrintable(address->text()));
    QTRY_COMPARE_WITH_TIMEOUT(available->text(), QString("0.00000000 CHN"), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(transparent->text() != QString::fromUtf8("—"), 30000);

    // Refreshes asked for one after the other, while the first is under way: one follows the other.
    for (int i{0}; i < 2 * tabs->count(); ++i) tabs->setCurrentIndex(i % tabs->count());
    DrainNodeRpc();
    DrainNodeRpc();
    QCOMPARE(address->text(), QString::fromStdString(own_address->get_str()));

    // Shield a coin through the page.
    QPushButton* shield{nullptr};
    for (auto* button : page.findChildren<QPushButton*>()) {
        if (button->text() == "Shield") shield = button;
    }
    QVERIFY(shield);
    tabs->setCurrentIndex(2);
    auto* shield_amount{shield->parentWidget()->findChild<QLineEdit*>()};
    QVERIFY(shield_amount);
    shield_amount->setText("1");
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { shield->click(); }).size(), 1);
    // A refresh while the shielding is under way waits for it.
    tabs->setCurrentIndex(0);
    tabs->setCurrentIndex(2);
    QTRY_VERIFY_WITH_TIMEOUT(shield->isEnabled(), 120000);
    QCOMPARE(status->text(), QString("Shielding 1 CHN: spendable after the next block."));
    QVERIFY(shield_amount->text().isEmpty());
    // Waiting for a block: shown as such, and in the history.
    QLabel* pending{nullptr};
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (auto* label : page.findChildren<QLabel*>()) {
            if (label->text() == "1.00000000 CHN waiting for a block") pending = label;
        }
        return pending != nullptr;
    }(), 30000);
    auto* history{qobject_cast<QTableWidget*>(tabs->widget(4))};
    QVERIFY(history);
    tabs->setCurrentWidget(history);
    QTRY_COMPARE_WITH_TIMEOUT(history->item(0, 1) ? history->item(0, 1)->text() : QString{}, QString("Shielded"), 30000);
    QCOMPARE(history->item(0, 2)->text(), QString("1.00000000"));
    QCOMPARE(history->item(0, 4)->text(), QString("waiting"));
    SaveScreenshot(page, "shielded-page");

    // In a block. The test chain is no sidechain, which has no shielded pool (feature_zside.py covers
    // one). The page shows what the wallet says.
    MineBlock(test);
    const auto balance{call("getshieldedbalance", UniValue{UniValue::VARR})};
    QVERIFY(balance);
    tabs->setCurrentIndex(0);
    QTRY_COMPARE_WITH_TIMEOUT(available->text(), QString::fromStdString((*balance)["available"].getValStr()) + " CHN", 30000);
    SaveScreenshot(page, "shielded-balance");
    DrainNodeRpc();
}

/** The button with this text among the children of `root`. */
QPushButton* ButtonOf(QWidget& root, const QString& text)
{
    for (auto* button : root.findChildren<QPushButton*>()) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

/**
 * Every control of the shielded page, on a chain without -orchardindex (no note can be spent):
 * the address buttons, the checks before each call, the confirmations (declined and accepted), the
 * errors the wallet gives back, the melt/cast plan and its cancelling, the hints as amounts are typed.
 */
void TestShieldedControls(TestChain100Setup& test, ClientModel& client_model)
{
    const NodeRpc::WalletNameFn wallet_name{[] { return std::optional<QString>{QString{}}; }};
    const auto call{[&](const std::string& method, const UniValue& params) {
        QString error;
        const auto result{NodeRpc::Call(&client_model, method, params, error, QString{})};
        if (!result) qWarning("%s: %s", method.c_str(), qPrintable(error));
        return result;
    }};
    ShieldedPage page(wallet_name);
    page.resize(1000, 700);
    page.setClientModel(&client_model);
    page.show();
    auto* address{page.findChild<QLineEdit*>("address")};
    auto* status{page.findChild<QLabel*>("shieldedStatus")};
    auto* tabs{page.findChild<QTabWidget*>()};
    QVERIFY(address && status && tabs);
    QTRY_VERIFY_WITH_TIMEOUT(!address->text().isEmpty(), 30000);
    DrainNodeRpc();
    const auto status_becomes{[&](const QString& text) { QTRY_COMPARE_WITH_TIMEOUT(status->text(), text, 60000); }};

    // Receive: copy the address; a new one of the same account.
    QApplication::clipboard()->clear();
    ButtonOf(page, "Copy")->click();
    QCOMPARE(QApplication::clipboard()->text(), address->text());
    QCOMPARE(status->text(), QString("The address is copied."));
    const QString first{address->text()};
    ButtonOf(page, "New address")->click();
    QTRY_VERIFY_WITH_TIMEOUT(address->text() != first, 30000);
    QVERIFY(address->text().startsWith("rzo1"));

    // Pay: an address and an amount first; the hint follows the kind of address.
    QWidget* pay_tab{tabs->widget(1)};
    tabs->setCurrentWidget(pay_tab);
    auto* send{ButtonOf(*pay_tab, "Pay")};
    QLineEdit* send_to{nullptr};
    QLineEdit* send_amount{nullptr};
    for (auto* edit : pay_tab->findChildren<QLineEdit*>()) {
        if (edit->placeholderText().startsWith("A shielded address")) send_to = edit;
        if (edit->placeholderText() == "Amount in CHN") send_amount = edit;
    }
    auto* memo{pay_tab->findChild<QPlainTextEdit*>()};
    QLabel* hint{nullptr};
    for (auto* label : pay_tab->findChildren<QLabel*>()) {
        if (label->text().startsWith("Private:")) hint = label;
    }
    QVERIFY(send && send_to && send_amount && memo && hint);
    send->click();
    QCOMPARE(status->text(), QString("Give an address and an amount."));
    const QString transparent_address{QString::fromStdString(call("getnewaddress", UniValue{UniValue::VARR})->get_str())};
    send_to->setText(transparent_address);
    QVERIFY(hint->text().startsWith("To a transparent address: this unshields."));
    QVERIFY(!memo->isEnabled());
    send_to->setText(first);
    QVERIFY(hint->text().startsWith("Private:"));
    QVERIFY(memo->isEnabled());
    send_amount->setText("0.5");
    memo->setPlainText(QString(513, 'x'));
    send->click();
    QCOMPARE(status->text(), QString("The memo is longer than 512 bytes."));
    memo->setPlainText("for the test");
    // Declined: nothing is sent.
    QCOMPARE(WithMessageBoxes(QMessageBox::No, [&] { send->click(); }), QStringList{"Pay 0.5 CHN privately to " + first + "?"});
    QCOMPARE(status->text(), QString("The memo is longer than 512 bytes."));
    // Accepted: the wallet has the note shielded above, but the node keeps no tree to spend it with
    // (no -orchardindex here), and says so.
    const QString no_index{"The node cannot find the notes in its tree yet: its -orchardindex is off, or not caught up with the last blocks (try again in a moment)"};
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { send->click(); }).size(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 60000);
    status_becomes(no_index);
    QCOMPARE(send_amount->text(), QString("0.5"));
    send_to->setText(transparent_address);
    const QStringList unshielding{WithMessageBoxes(QMessageBox::No, [&] { send->click(); })};
    QCOMPARE(unshielding.size(), 1);
    QVERIFY2(unshielding[0].startsWith("Unshield 0.5 CHN to the transparent address " + transparent_address + "?"), qPrintable(unshielding[0]));

    // Shield and unshield.
    QWidget* move_tab{tabs->widget(2)};
    tabs->setCurrentWidget(move_tab);
    QPushButton* shield{ButtonOf(*move_tab, "Shield")};
    QPushButton* unshield{ButtonOf(*move_tab, "Unshield")};
    QVERIFY(shield && unshield);
    QWidget* shield_card{shield->parentWidget()};
    QWidget* unshield_card{unshield->parentWidget()};
    QPushButton* shield_all{ButtonOf(*shield_card, "All")};
    QPushButton* unshield_all{ButtonOf(*unshield_card, "All")};
    QLineEdit* shield_amount{shield_card->findChild<QLineEdit*>()};
    QLineEdit* unshield_amount{nullptr};
    QLineEdit* unshield_to{nullptr};
    for (auto* edit : unshield_card->findChildren<QLineEdit*>()) {
        if (edit->placeholderText() == "Amount in CHN") unshield_amount = edit;
        if (edit->placeholderText().startsWith("To a transparent address")) unshield_to = edit;
    }
    QVERIFY(shield_all && unshield_all && shield_amount && unshield_amount && unshield_to);
    shield_amount->clear();
    shield->click();
    QCOMPARE(status->text(), QString("Give an amount."));
    // All: the transparent balance less a little for the fee.
    shield_all->click();
    const double balance{call("getbalance", UniValue{UniValue::VARR})->get_real()};
    QTRY_COMPARE_WITH_TIMEOUT(shield_amount->text(), QString::number(balance - 0.001, 'f', 8), 30000);
    QCOMPARE(WithMessageBoxes(QMessageBox::No, [&] { shield->click(); }).size(), 1);
    shield_amount->clear();
    unshield_amount->clear();
    unshield->click();
    QCOMPARE(status->text(), QString("Give an amount."));
    // All: what is available in blocks, the fee taken out of it.
    unshield_all->click();
    const auto shielded{call("getshieldedbalance", UniValue{UniValue::VARR})};
    QTRY_COMPARE_WITH_TIMEOUT(unshield_amount->text(), QString::fromStdString((*shielded)["available"].getValStr()), 30000);
    const QStringList asked{WithMessageBoxes(QMessageBox::Yes, [&] { unshield->click(); })};
    QCOMPARE(asked.size(), 1);
    QVERIFY2(asked[0].contains("the whole shielded balance in blocks (less the fee)") && asked[0].contains("a new address of this wallet"), qPrintable(asked[0]));
    QTRY_VERIFY_WITH_TIMEOUT(unshield->isEnabled(), 60000);
    status_becomes(no_index);
    // Typed by hand, the amount is no longer all of it; to a shielded address, refused.
    unshield_amount->clear();
    QTest::keyClicks(unshield_amount, "0.5");
    unshield_to->setText(first);
    unshield->click();
    QCOMPARE(status->text(), QString("That is a shielded address: pay it privately in the Pay tab."));
    unshield_to->setText(transparent_address);
    const QStringList asked_amount{WithMessageBoxes(QMessageBox::Yes, [&] { unshield->click(); })};
    QCOMPARE(asked_amount.size(), 1);
    QVERIFY2(asked_amount[0].startsWith("Move 0.5 CHN out of the shielded balance to " + transparent_address), qPrintable(asked_amount[0]));
    QTRY_VERIFY_WITH_TIMEOUT(unshield->isEnabled(), 60000);
    status_becomes(no_index);

    // Melt and cast.
    QWidget* meltcast_tab{tabs->widget(3)};
    tabs->setCurrentWidget(meltcast_tab);
    QPushButton* melt{ButtonOf(*meltcast_tab, "Melt my transparent coins")};
    QPushButton* cast{ButtonOf(*meltcast_tab, "Cast")};
    QPushButton* cancel{ButtonOf(*meltcast_tab, "Cancel the selected step")};
    QPushButton* cancel_all{ButtonOf(*meltcast_tab, "Cancel all")};
    auto* cast_amount{meltcast_tab->findChild<QLineEdit*>()};
    auto* plan{meltcast_tab->findChild<QTableWidget*>()};
    QLabel* bills{nullptr};
    for (auto* label : meltcast_tab->findChildren<QLabel*>()) {
        if (label->text().startsWith("Bills:")) bills = label;
    }
    QVERIFY(melt && cast && cancel && cancel_all && cast_amount && plan && bills);
    QTRY_COMPARE_WITH_TIMEOUT(plan->item(0, 3) ? plan->item(0, 3)->text() : QString{}, QString("Nothing planned."), 30000);
    // The bills an amount comes out as, as it is typed.
    cast_amount->setText("0.03");
    QCOMPARE(bills->text(), QString("Comes out as 2 bill(s): 0.02 + 0.01 CHN, plus the fees, out of the shielded balance."));
    cast_amount->setText("0.035");
    QCOMPARE(bills->text(), QString("Comes out as 2 bill(s): 0.02 + 0.01 CHN; 0.005 CHN stays shielded, plus the fees, out of the shielded balance."));
    cast_amount->setText("30000");
    QCOMPARE(bills->text(), QString("At most 20971.51 CHN at a time."));
    cast_amount->setText("0.001");
    QVERIFY(bills->text().startsWith("Bills: 0.01, 0.02, 0.04"));
    cast->click();
    QCOMPARE(status->text(), QString("Give an amount of 0.01 CHN at least."));
    cast_amount->setText("100");
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { cast->click(); }).size(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith("Not enough in the shielded account"), 60000);
    // Two coins in a block, for a plan of two steps at least.
    const auto split{call("sendtoaddress", NodeRpc::Args({transparent_address.toStdString(), 1}))};
    QVERIFY(split);
    MineBlock(test);
    // Nothing selected: nothing to cancel.
    cancel->click();
    QCOMPARE(status->text(), QString("Select a step that has not gone yet."));
    // Melt: declined, then done; the plan lists every coin, still to go.
    QCOMPARE(WithMessageBoxes(QMessageBox::No, [&] { melt->click(); }).size(), 1);
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { melt->click(); }).size(), 1);
    QTRY_VERIFY2_WITH_TIMEOUT(status->text().endsWith("coin(s) will be melted over the next blocks."), qPrintable(status->text()), 60000);
    const int planned{static_cast<int>(call("listmeltcast", UniValue{UniValue::VARR})->size())};
    QVERIFY(planned >= 2);
    QCOMPARE(status->text(), QString("%1 coin(s) will be melted over the next blocks.").arg(planned));
    QTRY_COMPARE_WITH_TIMEOUT(plan->rowCount(), planned, 30000);
    QVERIFY(plan->item(0, 0) && plan->item(0, 0)->text() == "Melt" && !plan->item(0, 0)->data(Qt::UserRole).isNull());
    QCOMPARE(plan->item(0, 2)->text(), QString("planned"));
    // One cancelled, selected first; then all the others.
    plan->setCurrentCell(0, 0);
    cancel->click();
    status_becomes("The step is cancelled.");
    QCOMPARE(static_cast<int>(call("listmeltcast", UniValue{UniValue::VARR})->size()), planned - 1);
    plan->clearFocus();
    QTRY_COMPARE_WITH_TIMEOUT(plan->rowCount(), planned - 1, 30000);
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { cancel_all->click(); }).size(), 1);
    status_becomes(QString("%1 step(s) cancelled.").arg(planned - 1));
    QTRY_COMPARE_WITH_TIMEOUT(plan->item(0, 3) ? plan->item(0, 3)->text() : QString{}, QString("Nothing planned."), 30000);
    QCOMPARE(call("listlockunspent", UniValue{UniValue::VARR})->size(), size_t{0});

    // The notes, when their tab is shown.
    auto* notes{qobject_cast<QTableWidget*>(tabs->widget(5))};
    QVERIFY(notes);
    tabs->setCurrentWidget(notes);
    const auto listed{call("listshieldednotes", NodeRpc::Args({false}))};
    QTRY_COMPARE_WITH_TIMEOUT(notes->rowCount(), static_cast<int>(listed->size()), 30000);
    DrainNodeRpc();
}

/** The wallet's views show the shielded page when asked (the Private action of the window). */
void TestShieldedNavigation(WalletModel& wallet_model, ClientModel& client_model, const PlatformStyle* platform_style)
{
    WalletFrame frame(platform_style, nullptr);
    frame.setClientModel(&client_model);
    auto* view{new WalletView(&wallet_model, platform_style, &frame)};
    QVERIFY(frame.addView(view));
    frame.setCurrentWallet(&wallet_model);
    frame.show();
    QVERIFY(!qobject_cast<ShieldedPage*>(view->currentWidget()));
    frame.gotoShieldedPage();
    auto* page{qobject_cast<ShieldedPage*>(view->currentWidget())};
    QVERIFY(page);
    // Shown, the page asks the wallet of its view for its address.
    auto* address{page->findChild<QLineEdit*>("address")};
    QTRY_VERIFY_WITH_TIMEOUT(address->text().startsWith("rzo1"), 30000);
    DrainNodeRpc();
}

void TestSidechainPage(interfaces::Node& node)
{
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    node.setContext(&test.m_node);
    // The page works through RPC, including the wallet commands.
    wallet_loader->registerRpcs();
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();

    // A wallet holding the coinbase key of the test chain.
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockableWalletDatabase());
    {
        LOCK(wallet->cs_wallet);
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet->SetupDescriptorScriptPubKeyMans();
        FlatSigningProvider provider;
        std::string error;
        auto descs = Parse("combo(" + EncodeSecret(test.coinbaseKey) + ")", provider, error, /*require_checksum=*/false);
        QVERIFY(descs.size() == 1);
        WalletDescriptor w_desc(std::move(descs.at(0)), 0, 0, 1, 1);
        QVERIFY(wallet->AddWalletDescriptor(w_desc, provider, "", false));
        wallet->SetLastBlockProcessed(105, WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    }
    {
        wallet::WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        const auto result{wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, /*start_height=*/0, /*max_height=*/{}, reserver, /*save_progress=*/false)};
        QCOMPARE(result.status, CWallet::ScanResult::SUCCESS);
    }
    wallet->SetBroadcastTransactions(true);
    // The wallet follows the blocks the tests mine.
    wallet->m_chain_notifications_handler = wallet->chain().handleNotifications(wallet);
    // There is no fee estimate on the test chain.
    wallet->m_min_fee = CFeeRate{10000};
    wallet->m_fallback_fee = CFeeRate{10000};
    WalletContext& context = *node.walletLoader().context();
    AddWallet(context, wallet);

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    OptionsModel options_model(node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    ClientModel client_model(node, &options_model);
    WalletModel wallet_model(interfaces::MakeWallet(context, wallet), client_model, platform_style.get());

    // The test chain is not a sidechain, which the page has to cope with. What the page does on a
    // sidechain is covered by feature_sidechain.py, through the commands the page uses.
    SidechainPage page(platform_style.get());
    page.resize(1000, 700);
    page.setClientModel(&client_model);
    page.setWalletModel(&wallet_model);
    page.refresh();
    QVERIFY(page.findChild<QLabel*>("mainchainSummary")->text().contains("not running as a sidechain"));
    QVERIFY(!page.findChild<QTabWidget*>("mainchainTabs")->isEnabled());
    SaveScreenshot(page, "mainchain-page");

    TestTools(test, client_model);
    TestShieldedPage(test, client_model);
    TestShieldedControls(test, client_model);
    TestShieldedNavigation(wallet_model, client_model, platform_style.get());

    RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt);
}

//! ChainActivity::Fetch looks up only what it does not know yet, and a reply of an unexpected shape
//! leaves a table as it was instead of throwing out of a Qt slot (which terminates).
void TestChainActivityFetch()
{
    std::map<std::string, int> calls;
    bool malformed{false};
    const ChainActivity::CallFn call{[&](const std::string& method, const UniValue& params) -> std::optional<UniValue> {
        ++calls[method];
        UniValue result;
        if (method == "getbestblockhash") return UniValue{"b2"};
        if (method == "getblockheader") {
            const std::string hash{params[0].get_str()};
            if (!result.read(hash == "b3" ? (malformed ? R"({"height":3,"time":"soon","nTx":1,"previousblockhash":"b2"})" : R"({"height":3,"time":300,"nTx":1,"previousblockhash":"b2"})")
                      : hash == "b2" ? R"({"height":2,"time":200,"nTx":2,"previousblockhash":"b1"})"
                                     : R"({"height":1,"time":100,"nTx":1})")) return std::nullopt;
            return result;
        }
        if (method == "getrawmempool") {
            if (!result.read(R"(["t1", 5, "t2"])")) return std::nullopt;
            return result;
        }
        if (method == "getmempoolentry") {
            if (!result.read(params[0].get_str() == "t1" ? R"({"time":10,"fees":{"base":0.0001},"vsize":150})" : R"({"time":"x"})")) return std::nullopt;
            return result;
        }
        return std::nullopt;
    }};
    ChainActivity::Snapshot first{ChainActivity::Fetch(call, {})};
    QCOMPARE(first.blocks.size(), size_t{2});
    QCOMPARE(first.blocks[0].hash, QString{"b2"});
    QCOMPARE(first.blocks[1].height, QString{"1"});
    QCOMPARE(calls["getblockheader"], 2);
    // The malformed entry is left out.
    QCOMPARE(first.mempool.size(), size_t{1});
    QVERIFY(first.mempool.contains("t1"));
    QCOMPARE(calls["getmempoolentry"], 2);

    // Nothing new: no block and no transaction looked up again (t2 is, being still unknown).
    calls.clear();
    const ChainActivity::Snapshot second{ChainActivity::Fetch(call, first)};
    QCOMPARE(second.blocks.size(), size_t{2});
    QCOMPARE(calls["getblockheader"], 0);
    QCOMPARE(calls["getmempoolentry"], 1);
    QCOMPARE(second.mempool.size(), size_t{1});

    // A malformed header keeps the blocks shown last time.
    malformed = true;
    const ChainActivity::CallFn new_tip{[&](const std::string& method, const UniValue& params) -> std::optional<UniValue> {
        if (method == "getbestblockhash") return UniValue{"b3"};
        return call(method, params);
    }};
    const ChainActivity::Snapshot third{ChainActivity::Fetch(new_tip, second)};
    QCOMPARE(third.blocks.size(), size_t{2});
    QCOMPARE(third.blocks[0].hash, QString{"b2"});
    malformed = false;
    calls.clear();
    const ChainActivity::Snapshot fourth{ChainActivity::Fetch(new_tip, second)};
    QCOMPARE(fourth.blocks.size(), size_t{3});
    QCOMPARE(fourth.blocks[0].height, QString{"3"});
    // Only the new block was looked up.
    QCOMPARE(calls["getblockheader"], 1);
}

} // namespace

void SidechainTests::sidechainTests()
{
    // Earlier tests in this binary leave single-shot timers behind (see
    // ConfirmMessage() in qt/test/util.cpp) that click on whatever message box
    // is open when they fire and store its text through a pointer that is no
    // longer valid. Let them fire now, while there is no message box.
    QTest::qWait(500);
    TestChainActivityFetch();
    TestSidechainPage(m_node);
}
