// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/sidechaintests.h>

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <addresstype.h>
#include <common/args.h>
#include <key.h>
#include <outputtype.h>
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
#include <qt/guiutil.h>
#include <qt/optionsdialog.h>
#include <qt/optionsmodel.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/signverifymessagedialog.h>
#include <qt/themedframe.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletcontroller.h>
#include <qt/platformstyle.h>
#include <qt/proofoffundsdialog.h>
#include <qt/shieldedpage.h>
#include <qt/sidebarmining.h>
#include <qt/sidechainpage.h>
#include <qt/intro.h>
#include <qt/theme.h>
#include <qt/timestampdialog.h>
#include <qt/walletframe.h>
#include <qt/walletmodel.h>
#include <qt/walletview.h>
#include <rpc/server.h>
#include <consensus/merkle.h>
#include <pow.h>
#include <script/descriptor.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <sidechain/follower.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
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
#include <QComboBox>
#include <QDateTime>
#include <QInputDialog>
#include <QMainWindow>
#include <QProcess>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUrl>
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
#include <QClipboard>
#include <QGuiApplication>
#include <QTemporaryFile>
#include <QTimer>

#include <map>
#include <memory>
#include <atomic>
#include <thread>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::CWallet;
using wallet::GetWallet;
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

/** A push button of `parent` by its text. */
QPushButton* Button(QWidget& parent, const QString& text)
{
    for (QPushButton* button : parent.findChildren<QPushButton*>()) {
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

/**
 * Answer the dialogs that pop up while `action` runs: input dialogs with `answers` in turn (one
 * with none left is cancelled), message boxes with `button`. @return the texts of the message boxes
 */
QStringList AnswerDialogs(QStringList answers, QMessageBox::StandardButton button, const std::function<void()>& action)
{
    QStringList texts;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (!widget->isVisible()) continue;
            if (auto* input{qobject_cast<QInputDialog*>(widget)}) {
                if (answers.isEmpty()) {
                    input->reject();
                    continue;
                }
                const QString answer{answers.takeFirst()};
                if (input->inputMode() == QInputDialog::IntInput) {
                    input->setIntValue(answer.toInt());
                } else {
                    input->setTextValue(answer);
                }
                input->accept();
            } else if (auto* box{qobject_cast<QMessageBox*>(widget)}) {
                texts << box->text();
                QAbstractButton* choice{box->button(button)};
                if (!choice) choice = box->button(QMessageBox::Ok);
                if (choice) choice->click();
            }
        }
    });
    timer.start(20);
    action();
    timer.stop();
    return texts;
}

/** The windows and models around the sidechain page: the parts the tests above leave out. */
void TestMoreWindows(TestChain100Setup& test, ClientModel& client_model, WalletModel& wallet_model, OptionsModel& options_model, const PlatformStyle* platform_style)
{
    const int height{WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Height())};

    // Calls off the GUI thread: the answer comes back on it, to a receiver that is still there.
    {
        QObject receiver;
        bool ran{false};
        NodeRpc::RunAsync(&receiver, [&]() -> std::function<void()> { return [&] { ran = true; }; });
        QTRY_VERIFY_WITH_TIMEOUT(ran, 10000);
        std::optional<UniValue> result;
        QString error;
        bool done{false};
        const auto keep{[&](std::optional<UniValue> r, const QString& e) {
            result = std::move(r);
            error = e;
            done = true;
        }};
        NodeRpc::CallAsync(&receiver, &client_model, "getblockcount", UniValue{UniValue::VARR}, keep);
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QVERIFY(result);
        QCOMPARE(result->getInt<int>(), height);
        done = false;
        NodeRpc::CallAsync(&receiver, &client_model, "nosuchcommand", UniValue{UniValue::VARR}, keep);
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QVERIFY(!result);
        QCOMPARE(error, QString("Method not found"));
        // Without a node, the answer comes at once.
        done = false;
        NodeRpc::CallAsync(&receiver, nullptr, "getblockcount", UniValue{UniValue::VARR}, keep);
        QVERIFY(done);
        QCOMPARE(error, QString("The node is not available."));
        // A receiver gone before the answer hears nothing.
        bool called{false};
        auto* gone{new QObject};
        NodeRpc::CallAsync(gone, &client_model, "getblockcount", UniValue{UniValue::VARR}, [&](std::optional<UniValue>, const QString&) { called = true; });
        delete gone;
        // The next call is answered after it: by then the first was dropped.
        done = false;
        NodeRpc::CallAsync(&receiver, &client_model, "getblockcount", UniValue{UniValue::VARR}, keep);
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QVERIFY(!called);
    }

    // The latest blocks and transactions, fetched off the GUI thread when the window shows.
    {
        ChainActivity activity;
        activity.setClientModel(&client_model);
        auto* blocks{activity.findChild<QTableWidget*>("latestBlocks")};
        auto* transactions{activity.findChild<QTableWidget*>("latestTransactions")};
        activity.show();
        QTRY_VERIFY_WITH_TIMEOUT(blocks->rowCount() > 0, 10000);
        QCOMPARE(blocks->item(0, 0)->text(), QString::number(height));
        QString details;
        QObject::connect(&activity, &ChainActivity::detailsRequested, [&](const QString& hash) { details = hash; });
        Q_EMIT blocks->cellDoubleClicked(0, 1);
        QCOMPARE(details, blocks->item(0, 3)->text());
        // A transaction in the mempool shows on the next refresh; a refresh asked for while one is under way follows it.
        QString send_error;
        const auto sent{NodeRpc::Call(&client_model, "sendtoaddress", NodeRpc::Args({EncodeDestination(WitnessV0KeyHash{test.coinbaseKey.GetPubKey()}), 1}), send_error, QString{})};
        QVERIFY2(sent, qPrintable(send_error));
        activity.refresh();
        activity.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(transactions->rowCount() == 1, 10000);
        QCOMPARE(transactions->item(0, 3)->text(), QString::fromStdString(sent->get_str()));
        Q_EMIT transactions->cellDoubleClicked(0, 0);
        QCOMPARE(details, QString::fromStdString(sent->get_str()));
        MineBlock(test);
        activity.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(transactions->rowCount() == 0 && blocks->item(0, 0)->text() == QString::number(height + 1), 10000);
        activity.setClientModel(nullptr);
        activity.refresh();
    }

    const NodeRpc::WalletNameFn wallet_name{[] { return std::optional<QString>{QString{}}; }};
    const NodeRpc::WalletNameFn no_wallet{[] { return std::optional<QString>{}; }};

    // The block explorer fills itself when it shows; a double click on a block or a transaction shows it.
    {
        BlockExplorer explorer;
        explorer.setClientModel(&client_model);
        auto* blocks{explorer.findChild<QTableWidget*>("explorerBlocks")};
        QCOMPARE(blocks->rowCount(), 0);
        explorer.show();
        QVERIFY(blocks->rowCount() > 0);
        Q_EMIT blocks->cellDoubleClicked(1, 0);
        QVERIFY(explorer.findChild<QLabel*>("explorerTitle")->text().startsWith(QString("Block %1:").arg(height)));
        QString send_error;
        const auto sent{NodeRpc::Call(&client_model, "sendtoaddress", NodeRpc::Args({EncodeDestination(WitnessV0KeyHash{test.coinbaseKey.GetPubKey()}), 1}), send_error, QString{})};
        QVERIFY2(sent, qPrintable(send_error));
        explorer.findChild<QLineEdit*>("explorerSearch")->setText(QString::number(height));
        Q_EMIT explorer.findChild<QLineEdit*>("explorerSearch")->returnPressed();
        QVERIFY(explorer.findChild<QLabel*>("explorerTitle")->text().startsWith(QString("Block %1:").arg(height)));
        Button(explorer, "Refresh")->click();
        auto* mempool{explorer.findChild<QTableWidget*>("explorerMempool")};
        QCOMPARE(mempool->rowCount(), 1);
        Q_EMIT mempool->cellDoubleClicked(0, 0);
        QCOMPARE(explorer.findChild<QLabel*>("explorerTitle")->text(), "Transaction " + QString::fromStdString(sent->get_str()));
        QCOMPARE(explorer.findChild<QListWidget*>("explorerBlockTxs")->count(), 0);
        Button(explorer, "Search")->click();
        explorer.hide();
        MineBlock(test);
    }

    // A new address for the miner, from the open wallet; none without one.
    {
        MiningDialog mining(wallet_name);
        mining.setClientModel(&client_model);
        mining.show();
        auto* address{mining.findChild<QLineEdit*>("miningAddress")};
        address->clear();
        Button(mining, "New address")->click();
        QVERIFY2(address->text().startsWith(QString::fromStdString(Params().Bech32HRP() + "1")), qPrintable(address->text()));
        MiningDialog without(no_wallet);
        without.setClientModel(&client_model);
        QCOMPARE(AnswerDialogs({}, QMessageBox::Ok, [&] { Button(without, "New address")->click(); }), QStringList{"Open or create a wallet first, or enter an address."});
        mining.setClientModel(nullptr);
    }

    // The tabs of the crypto tools, each opened from the menu.
    {
        CryptoToolsDialog tools;
        auto* tabs{tools.findChild<QTabWidget*>()};
        for (const auto tab : {CryptoToolsDialog::MERKLE, CryptoToolsDialog::ADDRESS, CryptoToolsDialog::HASH}) {
            tools.showTab(tab);
            QCOMPARE(tabs->currentIndex(), int{tab});
            QVERIFY(tools.isVisible());
        }
        tools.hide();
    }

    // Copying a proof of funds.
    {
        ProofOfFundsDialog proof(wallet_name);
        proof.findChild<QPlainTextEdit*>("proofOutput")->setPlainText("a proof");
        Button(proof, "Copy proof")->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("a proof"));
    }

    // Multisig: keys of partners, addresses from a descriptor, removing either, copying them.
    {
        QSettings().remove("MultisigKeys");
        QSettings().remove("MultisigAddresses");
        MultisigDialog multisig(wallet_name);
        multisig.setClientModel(&client_model);
        multisig.show();
        auto* keys{multisig.findChild<QTableWidget*>("multisigKeys")};
        auto* name{multisig.findChild<QLineEdit*>("multisigKeyName")};
        auto* key{multisig.findChild<QLineEdit*>("multisigKey")};
        QCOMPARE(AnswerDialogs({}, QMessageBox::Ok, [&] { Button(multisig, "Add partner")->click(); }), QStringList{"Enter the name of the partner and their public key."});
        name->setText("Alice");
        key->setText("not a key");
        const QStringList refused{AnswerDialogs({}, QMessageBox::Ok, [&] { Button(multisig, "Add partner")->click(); })};
        QCOMPARE(refused.size(), 1);
        QVERIFY2(refused.at(0).startsWith("This is not a public key:"), qPrintable(refused.at(0)));
        const QString alice{QString::fromStdString(HexStr(GenerateRandomKey().GetPubKey()))};
        key->setText(alice);
        Button(multisig, "Add partner")->click();
        QCOMPARE(keys->rowCount(), 1);
        QCOMPARE(keys->item(0, 0)->text(), QString("Alice"));
        QVERIFY(name->text().isEmpty() && key->text().isEmpty());
        multisig.addOwnKey();
        QCOMPARE(keys->rowCount(), 2);
        keys->setCurrentCell(0, 0);
        Button(multisig, "Copy key")->click();
        QCOMPARE(QApplication::clipboard()->text(), alice);
        // (Both tabs have a Remove button: the one of the keys.)
        QPushButton* remove_key{Button(*keys->parentWidget(), "Remove")};
        QVERIFY(remove_key);
        remove_key->click();
        QCOMPARE(keys->rowCount(), 1);
        QCOMPARE(keys->item(0, 0)->text(), QString("Me (default wallet)"));
        keys->setCurrentCell(-1, -1);
        remove_key->click();
        QCOMPARE(keys->rowCount(), 1);

        auto* addresses{multisig.findChild<QTableWidget*>("multisigAddresses")};
        const QString bob{QString::fromStdString(HexStr(GenerateRandomKey().GetPubKey()))};
        const QString descriptor{QStringLiteral("wsh(sortedmulti(1,%1,%2))").arg(alice, bob)};
        // Not a descriptor of this window; cancelled at the name; then added; then once more.
        QCOMPARE(AnswerDialogs({"pkh(" + alice + ")"}, QMessageBox::Ok, [&] { multisig.importAddress(); }), QStringList{"This is not the descriptor of a shared address made in this window."});
        QVERIFY(AnswerDialogs({descriptor}, QMessageBox::Ok, [&] { multisig.importAddress(); }).isEmpty());
        QCOMPARE(addresses->rowCount(), 0);
        QVERIFY(AnswerDialogs({descriptor, "Imported"}, QMessageBox::Ok, [&] { multisig.importAddress(); }).isEmpty());
        QCOMPARE(addresses->rowCount(), 1);
        QCOMPARE(addresses->item(0, 0)->text(), QString("Imported"));
        const QStringList again{AnswerDialogs({descriptor, "Again"}, QMessageBox::Ok, [&] { multisig.importAddress(); })};
        QCOMPARE(again, QStringList{"This shared address is already in the list, as \"Imported\"."});
        addresses->setCurrentCell(0, 0);
        Button(multisig, "Copy address")->click();
        QCOMPARE(QApplication::clipboard()->text(), addresses->item(0, 3)->text());
        Button(multisig, "Copy descriptor")->click();
        QVERIFY(QApplication::clipboard()->text().startsWith(descriptor + "#"));
        multisig.findChild<QPlainTextEdit*>("multisigPsbt")->setPlainText("cHNidP8B");
        Button(multisig, "Copy")->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("cHNidP8B"));
        // Removing asks first.
        QCOMPARE(AnswerDialogs({}, QMessageBox::No, [&] { multisig.removeAddress(); }).size(), 1);
        QCOMPARE(addresses->rowCount(), 1);
        AnswerDialogs({}, QMessageBox::Yes, [&] { multisig.removeAddress(); });
        QCOMPARE(addresses->rowCount(), 0);
        multisig.removeAddress();
        QSettings().remove("MultisigKeys");
        QSettings().remove("MultisigAddresses");
    }

    // The transaction list filters only on what is asked for.
    {
        TransactionFilterProxy filter;
        filter.setSourceModel(wallet_model.getTransactionTableModel());
        const int all{filter.rowCount()};
        QVERIFY(all > 1);
        filter.setShowInactive(false);
        QCOMPARE(filter.rowCount(), all);
        filter.setShowInactive(true);
        const QString txid{filter.index(0, 0).data(TransactionTableModel::TxHashRole).toString()};
        filter.setSearchString(txid.toUpper());
        QVERIFY(filter.rowCount() >= 1 && filter.rowCount() < all);
        filter.setSearchString("no such thing");
        QCOMPARE(filter.rowCount(), 0);
        filter.setSearchString({});
        QCOMPARE(filter.rowCount(), all);
        filter.setDateRange(QDateTime::currentDateTime().addYears(10), std::nullopt);
        QCOMPARE(filter.rowCount(), 0);
        filter.setDateRange(std::nullopt, QDateTime::fromSecsSinceEpoch(0));
        QCOMPARE(filter.rowCount(), 0);
        filter.setDateRange(QDateTime::fromSecsSinceEpoch(0), std::nullopt);
        QCOMPARE(filter.rowCount(), all);
    }

    // Messages signed with a P2WPKH address in the window, and checked.
    {
        SignVerifyMessageDialog sign(platform_style, nullptr);
        sign.setModel(&wallet_model);
        const auto destination{wallet_model.wallet().getNewDestination(OutputType::BECH32, "")};
        QVERIFY(destination);
        const QString address{QString::fromStdString(EncodeDestination(*destination))};
        sign.findChild<QValidatedLineEdit*>("addressIn_SM")->setText(address);
        sign.findChild<QPlainTextEdit*>("messageIn_SM")->setPlainText("signed in the window");
        sign.findChild<QPushButton*>("signMessageButton_SM")->click();
        QCOMPARE(sign.findChild<QLabel*>("statusLabel_SM")->text(), QString("<nobr>Message signed.</nobr>"));
        const QString signature{sign.findChild<QLineEdit*>("signatureOut_SM")->text()};
        QVERIFY(!signature.isEmpty());
        sign.findChild<QValidatedLineEdit*>("addressIn_VM")->setText(address);
        sign.findChild<QPlainTextEdit*>("messageIn_VM")->setPlainText("signed in the window");
        sign.findChild<QValidatedLineEdit*>("signatureIn_VM")->setText(signature);
        sign.findChild<QPushButton*>("verifyMessageButton_VM")->click();
        QCOMPARE(sign.findChild<QLabel*>("statusLabel_VM")->text(), QString("<nobr>Message verified.</nobr>"));
        // An address of scripts stands for no single key, to sign or to verify with.
        const auto taproot{wallet_model.wallet().getNewDestination(OutputType::BECH32M, "")};
        QVERIFY(taproot);
        sign.findChild<QValidatedLineEdit*>("addressIn_SM")->setText(QString::fromStdString(EncodeDestination(*taproot)));
        sign.findChild<QPushButton*>("signMessageButton_SM")->click();
        QVERIFY(sign.findChild<QLabel*>("statusLabel_SM")->text().startsWith("The entered address does not refer to a single key."));
        sign.findChild<QValidatedLineEdit*>("addressIn_VM")->setText(QString::fromStdString(EncodeDestination(*taproot)));
        sign.findChild<QPushButton*>("verifyMessageButton_VM")->click();
        QVERIFY2(!sign.findChild<QLabel*>("statusLabel_VM")->text().contains("Message verified"), qPrintable(sign.findChild<QLabel*>("statusLabel_VM")->text()));
    }

    // The look, the font and the frame, set in the options.
    {
        const bool frame{Theme::SavedFrame()};
        OptionsDialog options(nullptr, /*enableWallet=*/true);
        options.setModel(&options_model);
        options.setCurrentTab(OptionsDialog::TAB_DISPLAY);
        auto* theme{options.findChild<QComboBox*>("theme")};
        theme->setCurrentIndex(theme->findData("midnight"));
        options.findChild<QSpinBox*>("fontSize")->setValue(3);
        options.findChild<QCheckBox*>("themedFrame")->setChecked(!frame);
        WithMessageBoxes(QMessageBox::Ok, [&] { options.findChild<QPushButton*>("okButton")->click(); });
        QCOMPARE(Theme::Saved(), QString("midnight"));
        QCOMPARE(Theme::SavedFontSizeAdjustment(), 3);
        QCOMPARE(Theme::SavedFrame(), !frame);

        // The frame of the main window in the colours of the theme: its button maximizes and restores the window.
        QMainWindow window;
        ThemedFrame themed(&window);
        Theme::SaveFrame(true);
        themed.apply();
        window.show();
        QToolButton* maximize{nullptr};
        for (QToolButton* button : window.findChildren<QToolButton*>()) {
            if (button->toolTip() == "Maximize") maximize = button;
        }
        QVERIFY(maximize);
        maximize->click();
        QTRY_VERIFY(window.isMaximized());
        maximize->click();
        QTRY_VERIFY(!window.isMaximized());
        window.hide();

        Theme::SaveFrame(frame);
        Theme::SaveFont({}, 0);
        Theme::Apply("system");
        Theme::Save("system");
    }
}

/** Run `work` on a thread of its own while the GUI thread goes on with its events (which the work may wait for). */
void OffGuiThread(const std::function<void()>& work)
{
    std::atomic<bool> finished{false};
    std::thread thread{[&] {
        work();
        finished = true;
    }};
    while (!finished) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    thread.join();
}

/** WalletController::getOrCreateWallet from the GUI thread and from others; the models it made. */
void TestWalletController(interfaces::Node& node, ClientModel& client_model, const PlatformStyle* platform_style, WalletContext& context, const std::shared_ptr<CWallet>& wallet)
{
    const auto make_wallet{[&](const std::string& name) {
        auto other{std::make_shared<CWallet>(node.context()->chain.get(), name, CreateMockableWalletDatabase())};
        {
            LOCK(other->cs_wallet);
            other->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            other->SetupDescriptorScriptPubKeyMans();
            const CBlockIndex* tip{WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip())};
            other->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
        }
        AddWallet(context, other);
        return other;
    }};
    std::vector<WalletModel*> announced;
    QPointer<WalletModel> late;
    std::shared_ptr<CWallet> third;
    {
        WalletController controller(client_model, platform_style, nullptr);
        QObject::connect(&controller, &WalletController::walletAdded, [&](WalletModel* model) { announced.push_back(model); });
        // The wallet loaded already has its model, the same whichever thread asks.
        const QPointer<WalletModel> first{controller.getOrCreateWallet(interfaces::MakeWallet(context, wallet))};
        QVERIFY(first);
        QCOMPARE(first->parent(), &controller);
        QCOMPARE(controller.getOrCreateWallet(interfaces::MakeWallet(context, wallet)).data(), first.data());
        QPointer<WalletModel> from_thread;
        OffGuiThread([&] { from_thread = controller.getOrCreateWallet(interfaces::MakeWallet(context, wallet)); });
        QCOMPARE(from_thread.data(), first.data());

        // A new wallet from another thread: announced, and given its parent, on the GUI thread.
        const auto second{make_wallet("second")};
        QPointer<WalletModel> second_model;
        OffGuiThread([&] { second_model = controller.getOrCreateWallet(interfaces::MakeWallet(context, second)); });
        QVERIFY(second_model);
        QTRY_COMPARE(second_model->parent(), &controller);
        QVERIFY(std::find(announced.begin(), announced.end(), second_model.data()) != announced.end());
        QCOMPARE(controller.getOrCreateWallet(interfaces::MakeWallet(context, second)).data(), second_model.data());
        // A model being unloaded is no longer the model of its wallet: loaded again, it gets a new one.
        second_model->setUnloading();
        const QPointer<WalletModel> reloaded{controller.getOrCreateWallet(interfaces::MakeWallet(context, second))};
        QVERIFY(reloaded && reloaded.data() != second_model.data());
        // Closing all wallets asks first: not now.
        QCOMPARE(WithMessageBoxes(QMessageBox::Cancel, [&] { controller.closeAllWallets(); }).size(), 1);

        // Registered from another thread, and the controller gone before the GUI thread got to it.
        third = make_wallet("third");
        // (The thread is waited for without the events of the GUI thread: the model stays without its parent.)
        std::thread{[&] { late = controller.getOrCreateWallet(interfaces::MakeWallet(context, third)); }}.join();
        QVERIFY(late);
        QVERIFY(late->parent() != &controller);
    }
    // The controller deleted the model it had not handed out yet.
    QVERIFY(!late);
    QCoreApplication::processEvents();
    RemoveWallet(context, third, /*load_on_start=*/std::nullopt);
    for (const char* name : {"second"}) {
        if (auto other{GetWallet(context, name)}) RemoveWallet(context, other, /*load_on_start=*/std::nullopt);
    }
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

    TestMoreWindows(test, client_model, wallet_model, options_model, platform_style.get());
    TestWalletController(node, client_model, platform_style.get(), context, wallet);

    RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt);
}

/**
 * A chain that is a sidechain, with a record of its mainchain the test writes, and a follower whose
 * mainchain node cannot be reached (it is never started): what the pages do that needs the mainchain
 * node fails, which they have to say; the rest works.
 */
struct SidechainChain {
    TestingSetup test{ChainType::REGTEST, {.extra_args = {"-sidechainslot=3"}}};
    CKey key{GenerateRandomKey()};
    CScript script{GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()})};

    SidechainChain()
    {
        sidechain::MainClient::Options options;
        // Nothing listens on port 1.
        options.port = 1;
        options.credentials = "user:password";
        test.m_node.follower = std::make_unique<sidechain::Follower>(test.m_node, std::move(options));
        Main();
    }
    ~SidechainChain() { test.m_node.follower.reset(); }

    sidechain::Mainchain& Record() { return *Assert(test.m_node.chainman->m_mainchain); }

    void Main(const std::function<void(sidechain::MainBlock&)>& fill = {})
    {
        sidechain::MainBlock block;
        block.hash = uint256{static_cast<uint8_t>(Record().Height() + 2)};
        block.prev_hash = Record().TipHash();
        if (fill) fill(block);
        QVERIFY(Record().Append(block));
    }

    /** A block of the mempool, committed to and connected. */
    CBlock Mine()
    {
        auto block_template{interfaces::MakeMining(test.m_node)->createNewBlock({.coinbase_output_script = CScript() << OP_TRUE}, /*cooldown=*/false)};
        CBlock block{block_template->getBlock()};
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, test.m_node.chainman->GetConsensus())) ++block.nNonce;
        Main([&](sidechain::MainBlock& b) { b.bmm = block.GetHash(); });
        test.m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/nullptr);
        return block;
    }

    CMutableTransaction Spend(const COutPoint& prevout, const CTxOut& spent, std::vector<CTxOut> outputs)
    {
        CMutableTransaction tx;
        tx.vin.emplace_back(prevout);
        tx.vout = std::move(outputs);
        FillableSigningProvider keystore;
        keystore.AddKey(key);
        std::map<int, bilingual_str> errors;
        SignTransaction(tx, &keystore, {{prevout, Coin{spent, 1, false}}}, SignOptions{.sighash_type = SIGHASH_ALL}, errors);
        return tx;
    }
};

/** The Mainchain page and the mining bar of the main window, on a sidechain. */
void TestSidechainPageOnASidechain(interfaces::Node& node)
{
    SidechainChain chain;
    TestingSetup& test{chain.test};
    // Coins for the key, from a deposit; then a withdrawal of them, refundable by the key.
    chain.Main([&](sidechain::MainBlock& b) {
        sidechain::MainDeposit deposit;
        deposit.destination = EncodeDestination(WitnessV0KeyHash{chain.key.GetPubKey()});
        deposit.amount = 10 * COIN;
        b.deposits.push_back(deposit);
    });
    const CBlock funding{chain.Mine()};
    QCOMPARE(funding.vtx[0]->vout[1].nValue, 10 * COIN);
    const CTxOut& coin{funding.vtx[0]->vout[1]};
    const CMutableTransaction withdrawal{chain.Spend(COutPoint{funding.vtx[0]->GetHash(), 1}, coin,
                                                     {CTxOut{2 * COIN, sidechain::WithdrawalScript(1000, chain.key.GetPubKey().GetID(), chain.script)},
                                                      CTxOut{coin.nValue - 2 * COIN - 10000, chain.script}})};
    QVERIFY(WITH_LOCK(::cs_main, return test.m_node.chainman->ProcessTransaction(MakeTransactionRef(withdrawal))).m_result_type == MempoolAcceptResult::ResultType::VALID);
    chain.Mine();
    QCOMPARE(WITH_LOCK(::cs_main, return test.m_node.chainman->ActiveChain().Height()), 2);

    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    node.setContext(&test.m_node);
    wallet_loader->registerRpcs();
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    // The calls off the GUI thread stopped when the node of AppTests shut down.
    NodeRpc::Restart();
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockableWalletDatabase());
    {
        LOCK(wallet->cs_wallet);
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet->SetupDescriptorScriptPubKeyMans();
        FlatSigningProvider provider;
        std::string error;
        auto descs = Parse("combo(" + EncodeSecret(chain.key) + ")", provider, error, /*require_checksum=*/false);
        QVERIFY(descs.size() == 1);
        WalletDescriptor w_desc(std::move(descs.at(0)), 0, 0, 1, 1);
        QVERIFY(wallet->AddWalletDescriptor(w_desc, provider, "", false));
        wallet->SetLastBlockProcessed(2, WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    }
    {
        wallet::WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        const auto result{wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, /*start_height=*/0, /*max_height=*/{}, reserver, /*save_progress=*/false)};
        QCOMPARE(result.status, CWallet::ScanResult::SUCCESS);
    }
    wallet->SetBroadcastTransactions(true);
    wallet->m_min_fee = CFeeRate{10000};
    wallet->m_fallback_fee = CFeeRate{10000};
    WalletContext& context = *node.walletLoader().context();
    AddWallet(context, wallet);
    // However the test ends: no call left under way on the node, and the wallet gone before the chain.
    struct Cleanup {
        std::function<void()> fn;
        ~Cleanup() { fn(); }
    } cleanup{[&] {
        NodeRpc::Stop();
        RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt);
    }};

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    OptionsModel options_model(node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    ClientModel client_model(node, &options_model);
    WalletModel wallet_model(interfaces::MakeWallet(context, wallet), client_model, platform_style.get());

    SidechainPage page(platform_style.get());
    page.resize(1000, 700);
    page.setClientModel(&client_model);
    page.setWalletModel(&wallet_model);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    const QString summary{page.findChild<QLabel*>("mainchainSummary")->text()};
    QVERIFY2(page.findChild<QTabWidget*>("mainchainTabs")->isEnabled(), qPrintable(summary));
    QVERIFY2(summary.contains("cannot be reached"), qPrintable(summary));
    QVERIFY(page.findChild<QLabel*>("bundleStatus")->text().contains("The next block can start a bundle of 1 withdrawal(s) paying 1.99999000"));

    // Deposit tab: an address of this wallet, which names the sidechain, and to the clipboard.
    page.findChild<QPushButton*>("depositNewAddress")->click();
    const QString deposit_address{page.findChild<QLineEdit*>("depositAddress")->text()};
    QVERIFY(deposit_address.startsWith("s3_"));
    for (QPushButton* button : page.findChildren<QPushButton*>()) {
        if (button->text() == "Copy") button->click();
    }
    QCOMPARE(QGuiApplication::clipboard()->text(), deposit_address);

    // Withdraw tab: what is not filled in right is said; a withdrawal needs the mainchain node.
    auto* withdraw_button{page.findChild<QPushButton*>("withdrawButton")};
    QStringList texts{WithMessageBoxes(QMessageBox::Yes, [&] { withdraw_button->click(); })};
    QVERIFY(texts.size() == 1 && texts[0].contains("Enter a mainchain address"));
    page.findChild<QLineEdit*>("withdrawAddress")->setText("mainaddress");
    page.findChild<QLineEdit*>("withdrawAmount")->setText("1.5");
    texts = WithMessageBoxes(QMessageBox::Yes, [&] {
        withdraw_button->click();
        QVERIFY(QTest::qWaitFor([&] { return withdraw_button->isEnabled(); }, 30000));
        QTest::qWait(200);
    });
    QVERIFY(texts.size() == 2 && texts[0].contains("Withdraw 1.5 to the mainchain address mainaddress"));
    QVERIFY(texts[1].contains("Cannot reach the mainchain node"));

    // The withdrawal of the key, and a refund of it.
    auto* withdrawals{page.findChild<QTableWidget*>("withdrawals")};
    QCOMPARE(withdrawals->rowCount(), 1);
    QCOMPARE(withdrawals->item(0, 5)->text(), QString::fromStdString(withdrawal.GetHash().GetHex()));
    auto* refund_button{page.findChild<QPushButton*>("refundButton")};
    withdrawals->setCurrentCell(-1, -1);
    texts = WithMessageBoxes(QMessageBox::Ok, [&] { refund_button->click(); });
    QVERIFY(texts.size() == 1 && texts[0].contains("Select a withdrawal in the list first"));
    withdrawals->selectRow(0);
    texts = WithMessageBoxes(QMessageBox::Ok, [&] {
        refund_button->click();
        QVERIFY(QTest::qWaitFor([&] { return refund_button->isEnabled(); }, 30000));
        QTest::qWait(200);
    });
    QVERIFY(texts.size() == 1 && texts[0].contains("2.00000000 will be paid back to " + QString::fromStdString(EncodeDestination(WitnessV0KeyHash{chain.key.GetPubKey()}))));
    QCOMPARE(test.m_node.mempool->size(), size_t{1});

    // Mine tab: mining on and off, with a new address of the wallet; one block by hand needs the mainchain node.
    auto* status{page.findChild<QLabel*>("bmmStatus")};
    page.findChild<QPushButton*>("bmmStart")->click();
    QVERIFY(!page.findChild<QLineEdit*>("bmmAddress")->text().isEmpty());
    QVERIFY(status->text().contains("Automatic mining is on"));
    QVERIFY(test.m_node.follower->GetMining().enabled);
    page.findChild<QPushButton*>("bmmStop")->click();
    QVERIFY(status->text().contains("Automatic mining is off"));
    auto* amount{page.findChild<QLineEdit*>("bmmAmount")};
    auto* once{page.findChild<QPushButton*>("bmmOnce")};
    amount->setText("lots");
    texts = WithMessageBoxes(QMessageBox::Ok, [&] { once->click(); });
    QVERIFY(texts.size() == 1 && texts[0].contains("Enter the offer as a number"));
    amount->setText("0.0001");
    texts = WithMessageBoxes(QMessageBox::Ok, [&] {
        once->click();
        QVERIFY(QTest::qWaitFor([&] { return once->isEnabled(); }, 30000));
        QTest::qWait(200);
    });
    QVERIFY(texts.size() == 1 && texts[0].contains("Cannot reach the mainchain node"));
    SaveScreenshot(page, "mainchain-page-sidechain");

    // The mining bar of the main window does the same.
    std::optional<QString> wallet_name{QString{}};
    SidebarMining bar([&] { return wallet_name; });
    bar.setClientModel(&client_model);
    QVERIFY(bar.isEnabled());
    auto* bar_auto{bar.findChild<QPushButton*>("sidebarAutoMining")};
    auto* bar_status{bar.findChild<QLabel*>("sidebarMiningStatus")};
    bar_auto->click();
    QVERIFY(bar_auto->isChecked());
    QVERIFY(test.m_node.follower->GetMining().enabled);
    // The page, shown, follows.
    QVERIFY(status->text().contains("Automatic mining is on"));
    bar_auto->click();
    QVERIFY(!bar_auto->isChecked());
    QVERIFY(!test.m_node.follower->GetMining().enabled);
    auto* bar_fee{bar.findChild<QLineEdit*>("sidebarMiningFee")};
    auto* bar_once{bar.findChild<QPushButton*>("sidebarMineOnce")};
    bar_fee->setText("0");
    bar_once->click();
    QVERIFY(bar_status->text().contains("Enter the fee as a number above zero"));
    bar_fee->setText("0.0001");
    bar_once->click();
    QVERIFY(QTest::qWaitFor([&] { return bar_once->isEnabled(); }, 30000));
    QVERIFY(bar_status->text().contains("Cannot reach the mainchain node"));
    // Without a wallet there is no address to mine to.
    wallet_name.reset();
    bar_auto->click();
    QVERIFY(!bar_auto->isChecked());
    QVERIFY(bar_status->text().contains("Open or create a wallet first"));
    bar_once->click();
    QVERIFY(bar_status->text().contains("Open or create a wallet first"));

    // Hidden, the page stops refreshing.
    page.hide();
    QVERIFY(!page.isVisible());
}

/** The dialog that asks for the data directory names this program. */
void TestIntro()
{
    Intro intro(nullptr, /*blockchain_size_gb=*/1, /*chain_state_size_gb=*/1);
    QVERIFY(intro.findChild<QLabel*>("lblExplanation1")->text().contains(CLIENT_NAME));
    QVERIFY(!intro.findChild<QLabel*>("lblExplanation1")->text().contains("Bitcoin"));
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
    // AppTests shut a node down before, which stopped the calls off the GUI thread.
    NodeRpc::Restart();
    TestChainActivityFetch();
    TestSidechainPage(m_node);
    TestSidechainPageOnASidechain(m_node);
    TestIntro();
}
