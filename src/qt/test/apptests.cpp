// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <qt/test/apptests.h>

#include <chainparams.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/blockexplorer.h>
#include <qt/cryptotoolsdialog.h>
#include <qt/miningdialog.h>
#include <qt/multisigdialog.h>
#include <qt/proofoffundsdialog.h>
#include <qt/sidechainpage.h>
#include <qt/theme.h>
#include <qt/timestampdialog.h>

#include <key.h>
#include <logging.h>
#include <qt/bitcoin.h>
#include <qt/bitcoingui.h>
#include <qt/networkstyle.h>
#include <qt/rpcconsole.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QTimer>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QTextEdit>
#include <QtGlobal>
#include <QtTest/QtTestWidgets>
#include <QtTest/QtTestGui>

namespace {
//! Regex find a string group inside of the console output
QString FindInConsole(const QString& output, const QString& pattern)
{
    const QRegularExpression re(pattern);
    return re.match(output).captured(1);
}

//! Call getblockchaininfo RPC and check first field of JSON output.
void TestRpcCommand(RPCConsole* console)
{
    QTextEdit* messagesWidget = console->findChild<QTextEdit*>("messagesWidget");
    QLineEdit* lineEdit = console->findChild<QLineEdit*>("lineEdit");
    QSignalSpy mw_spy(messagesWidget, &QTextEdit::textChanged);
    QVERIFY(mw_spy.isValid());
    QTest::keyClicks(lineEdit, "getblockchaininfo");
    QTest::keyClick(lineEdit, Qt::Key_Return);
    QVERIFY(mw_spy.wait(1000));
    QCOMPARE(mw_spy.count(), 4);
    const QString output = messagesWidget->toPlainText();
    const QString pattern = QStringLiteral("\"chain\": \"(\\w+)\"");
    QCOMPARE(FindInConsole(output, pattern), QString("regtest"));
}
/** The visible window of type T, if one shows. */
template <typename T>
T* ShownWindow()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* window{qobject_cast<T*>(widget)}; window && window->isVisible()) return window;
    }
    return nullptr;
}

//! The menus and the sidebar of the main window: each opens what it names, or changes the look.
void TestWindowMenus(BitcoinGUI* window)
{
    const auto action{[&](const QString& menu, const QString& text) -> QAction* {
        for (QMenu* candidate : window->findChildren<QMenu*>()) {
            if (candidate->title() != menu) continue;
            for (QAction* item : candidate->actions()) {
                if (item->text() == text) return item;
            }
        }
        return nullptr;
    }};
    const auto opens{[&]<typename T>(const QString& menu, const QString& text) {
        QAction* item{action(menu, text)};
        QVERIFY2(item, qPrintable(menu + " / " + text));
        item->trigger();
        T* shown{ShownWindow<T>()};
        QVERIFY2(shown, qPrintable(text));
        shown->hide();
    }};
    opens.template operator()<TimestampDialog>(QStringLiteral("&Use %1").arg(CLIENT_NAME), "Timestamp &File");
    opens.template operator()<BlockExplorer>("Crypto &Tools", "&Block Explorer");
    opens.template operator()<CryptoToolsDialog>("Crypto &Tools", "&Hash Calculator");
    opens.template operator()<CryptoToolsDialog>("Crypto &Tools", "Merkle &Tree");
    opens.template operator()<CryptoToolsDialog>("Crypto &Tools", "&Address Decoder");
#ifdef ENABLE_WALLET
    opens.template operator()<ProofOfFundsDialog>("&Banking", "&Proof of Funds");
    opens.template operator()<MultisigDialog>("&Banking", "M&ultisig Lounge");

    // The window of the sidebar.
    for (QPushButton* button : window->findChildren<QPushButton*>()) {
        if (button->text() == "Block Explorer") button->click();
    }
    QVERIFY(ShownWindow<BlockExplorer>());
    ShownWindow<BlockExplorer>()->hide();
#endif

    // Looks and text sizes, from the menu and from the sidebar.
    const QString look_before{Theme::SavedLook()};
    const QString family_before{Theme::SavedFontFamily()};
    const int size_before{Theme::SavedFontSizeAdjustment()};
    QMenu* themes{window->findChild<QMenu*>("themeMenu")};
    QVERIFY(themes);
    Q_EMIT themes->aboutToShow();
    for (QAction* look : themes->actions()) {
        if (look->data().toString() == "midnight") look->trigger();
    }
    QCOMPARE(Theme::SavedLook(), QString("midnight"));
    const int size{Theme::SavedFontSizeAdjustment()};
    action("This &Node", "Increase font size")->trigger();
    QCOMPARE(Theme::SavedFontSizeAdjustment(), size + 1);
    action("This &Node", "Decrease font size")->trigger();
    QCOMPARE(Theme::SavedFontSizeAdjustment(), size);
#ifdef ENABLE_WALLET
    auto* looks{window->findChild<QComboBox*>("lookSelector")};
    QVERIFY(looks);
    Q_EMIT looks->activated(looks->findData("rose"));
    QCOMPARE(Theme::SavedLook(), QString("rose"));
    window->findChild<QToolButton*>("textLarger")->click();
    QCOMPARE(Theme::SavedFontSizeAdjustment(), size + 1);
    window->findChild<QToolButton*>("textSmaller")->click();
    QCOMPARE(Theme::SavedFontSizeAdjustment(), size);
#endif
    // As it was (SidechainTests checks the default font size).
    Theme::SaveLook(look_before.isEmpty() ? QString{"system"} : look_before);
    Theme::SaveFont(family_before, size_before);
}
} // namespace

//! Entry point for BitcoinApplication tests.
void AppTests::appTests()
{
    qRegisterMetaType<interfaces::BlockAndHeaderTipInfo>("interfaces::BlockAndHeaderTipInfo");
    m_app.parameterSetup();
    QVERIFY(m_app.createOptionsModel(/*resetSettings=*/true));
    QScopedPointer<const NetworkStyle> style(NetworkStyle::instantiate(Params().GetChainType()));
    m_app.setupPlatformStyle();
    m_app.createWindow(style.data());
    connect(&m_app, &BitcoinApplication::windowShown, this, &AppTests::guiTests);
    expectCallback("guiTests");
    m_app.baseInitialize();
    m_app.requestInitialize();
    m_app.exec();
    m_app.requestShutdown();
    m_app.exec();

    // Reset global state to avoid interfering with later tests.
    LogInstance().DisconnectTestLogger();
}

//! Entry point for BitcoinGUI tests.
void AppTests::guiTests(BitcoinGUI* window)
{
    HandleCallback callback{"guiTests", *this};
    TestWindowMenus(window);
    // zSide: the Private action shows the shielded page (enabled here by hand, with its group of tabs,
    // as a loaded wallet would: none is).
    QAction* shielded{nullptr};
    for (QAction* a : window->findChildren<QAction*>()) {
        if (a->text() == "&Private" && a->isCheckable()) {
            shielded = a;
            break;
        }
    }
    QVERIFY(shielded);
    QActionGroup* tabs{shielded->actionGroup()};
    QVERIFY(tabs);
    const bool tabs_enabled{tabs->isEnabled()};
    tabs->setEnabled(true);
    shielded->setEnabled(true);
    QVERIFY(shielded->isEnabled());
    shielded->trigger();
    QVERIFY(shielded->isChecked());
    shielded->setEnabled(false);
    tabs->setEnabled(tabs_enabled);
    connect(window, &BitcoinGUI::consoleShown, this, &AppTests::consoleTests);
    expectCallback("consoleTests");
    QAction* action = window->findChild<QAction*>("openRPCConsoleAction");
    action->activate(QAction::Trigger);
}

//! Entry point for RPCConsole tests.
void AppTests::consoleTests(RPCConsole* console)
{
    HandleCallback callback{"consoleTests", *this};
    TestRpcCommand(console);
}

//! Destructor to shut down after the last expected callback completes.
AppTests::HandleCallback::~HandleCallback()
{
    auto& callbacks = m_app_tests.m_callbacks;
    auto it = callbacks.find(m_callback);
    assert(it != callbacks.end());
    callbacks.erase(it);
    if (callbacks.empty()) {
        m_app_tests.m_app.exit(0);
    }
}
