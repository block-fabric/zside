// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <qt/optionsdialog.h>
#include <qt/forms/ui_optionsdialog.h>

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/theme.h>

#include <common/system.h>
#include <interfaces/node.h>
#include <netbase.h>
#include <node/caches.h>
#include <node/chainstatemanager_args.h>
#include <util/strencodings.h>

#include <chrono>

#include <QComboBox>
#include <QCheckBox>
#include <QFontDatabase>
#include <QSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QApplication>
#include <QDataWidgetMapper>
#include <QDir>
#include <QFontDialog>
#include <QIntValidator>
#include <QLocale>
#include <QMessageBox>
#include <QSystemTrayIcon>
#include <QTimer>

int setFontChoice(QComboBox* cb, const OptionsModel::FontChoice& fc)
{
    int i;
    for (i = cb->count(); --i >= 0; ) {
        QVariant item_data = cb->itemData(i);
        if (!item_data.canConvert<OptionsModel::FontChoice>()) continue;
        if (item_data.value<OptionsModel::FontChoice>() == fc) {
            break;
        }
    }
    if (i == -1) {
        // New item needed
        QFont chosen_font = OptionsModel::getFontForChoice(fc);
        QSignalBlocker block_currentindexchanged_signal(cb);  // avoid triggering QFontDialog
        cb->insertItem(0, QFontInfo(chosen_font).family(), QVariant::fromValue(fc));
        i = 0;
    }

    cb->setCurrentIndex(i);
    return i;
}

void setupFontOptions(QComboBox* cb, QLabel* preview)
{
    QFont embedded_font{GUIUtil::fixedPitchFont(true)};
    QFont system_font{GUIUtil::fixedPitchFont(false)};
    cb->addItem(QObject::tr("Embedded \"%1\"").arg(QFontInfo(embedded_font).family()), QVariant::fromValue(OptionsModel::FontChoice{OptionsModel::FontChoiceAbstract::EmbeddedFont}));
    cb->addItem(QObject::tr("Default system font \"%1\"").arg(QFontInfo(system_font).family()), QVariant::fromValue(OptionsModel::FontChoice{OptionsModel::FontChoiceAbstract::BestSystemFont}));
    cb->addItem(QObject::tr("Custom…"));

    const auto& on_font_choice_changed = [cb, preview](int index) {
        static int previous_index = -1;
        QVariant item_data = cb->itemData(index);
        QFont f;
        if (item_data.canConvert<OptionsModel::FontChoice>()) {
            f = OptionsModel::getFontForChoice(item_data.value<OptionsModel::FontChoice>());
        } else {
            bool ok;
            f = QFontDialog::getFont(&ok, GUIUtil::fixedPitchFont(false), cb->parentWidget());
            if (!ok) {
                cb->setCurrentIndex(previous_index);
                return;
            }
            index = setFontChoice(cb, OptionsModel::FontChoice{f});
        }
        if (preview) {
            preview->setFont(f);
        }
        previous_index = index;
    };
    QObject::connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged), on_font_choice_changed);
    on_font_choice_changed(cb->currentIndex());
}

OptionsDialog::OptionsDialog(QWidget* parent, bool enableWallet)
    : QDialog(parent, GUIUtil::dialog_flags | Qt::WindowMaximizeButtonHint),
      ui(new Ui::OptionsDialog)
{
    ui->setupUi(this);

    ui->verticalLayout->setStretchFactor(ui->tabWidget, 1);

    /* Main elements init */
    ui->databaseCache->setRange(MIN_DBCACHE_BYTES / 1_MiB, std::numeric_limits<int>::max());
    ui->threadsScriptVerif->setMinimum(-GetNumCores());
    ui->threadsScriptVerif->setMaximum(MAX_SCRIPTCHECK_THREADS);
    ui->pruneWarning->setVisible(false);
    ui->pruneWarning->setStyleSheet("QLabel { color: red; }");

    ui->pruneSize->setEnabled(false);
    connect(ui->prune, &QPushButton::toggled, ui->pruneSize, &QWidget::setEnabled);

    /* Network elements init */
    ui->proxyIp->setEnabled(false);
    ui->proxyPort->setEnabled(false);
    ui->proxyPort->setValidator(new QIntValidator(1, 65535, this));

    ui->proxyIpTor->setEnabled(false);
    ui->proxyPortTor->setEnabled(false);
    ui->proxyPortTor->setValidator(new QIntValidator(1, 65535, this));

    connect(ui->connectSocks, &QPushButton::toggled, ui->proxyIp, &QWidget::setEnabled);
    connect(ui->connectSocks, &QPushButton::toggled, ui->proxyPort, &QWidget::setEnabled);
    connect(ui->connectSocks, &QPushButton::toggled, this, &OptionsDialog::updateProxyValidationState);

    connect(ui->connectSocksTor, &QPushButton::toggled, ui->proxyIpTor, &QWidget::setEnabled);
    connect(ui->connectSocksTor, &QPushButton::toggled, ui->proxyPortTor, &QWidget::setEnabled);
    connect(ui->connectSocksTor, &QPushButton::toggled, this, &OptionsDialog::updateProxyValidationState);

    /* Window elements init */
#ifdef Q_OS_MACOS
    /* remove Window tab on Mac */
    ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->tabWindow));
    /* hide launch at startup option on macOS */
    ui->bitcoinAtStartup->setVisible(false);
    ui->verticalLayout_Main->removeWidget(ui->bitcoinAtStartup);
    ui->verticalLayout_Main->removeItem(ui->horizontalSpacer_0_Main);
#endif

    /* remove Wallet tab and 3rd party-URL textbox in case of -disablewallet */
    if (!enableWallet) {
        ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->tabWallet));
        ui->thirdPartyTxUrlsLabel->setVisible(false);
        ui->thirdPartyTxUrls->setVisible(false);
    }

#ifdef ENABLE_EXTERNAL_SIGNER
    ui->externalSignerPath->setToolTip(ui->externalSignerPath->toolTip().arg(CLIENT_NAME));
#else
    //: "External signing" means using devices such as hardware wallets.
    ui->externalSignerPath->setToolTip(tr("Compiled without external signing support (required for external signing)"));
    ui->externalSignerPath->setEnabled(false);
#endif
    /* Display elements init */
    QDir translations(":translations");

    ui->bitcoinAtStartup->setToolTip(ui->bitcoinAtStartup->toolTip().arg(CLIENT_NAME));
    ui->bitcoinAtStartup->setText(ui->bitcoinAtStartup->text().arg(CLIENT_NAME));

    ui->openBitcoinConfButton->setToolTip(ui->openBitcoinConfButton->toolTip().arg(CLIENT_NAME));

    ui->lang->setToolTip(ui->lang->toolTip().arg(CLIENT_NAME));
    ui->lang->addItem(QString("(") + tr("default") + QString(")"), QVariant(""));
    for (const QString &langStr : translations.entryList())
    {
        QLocale locale(langStr);

        /** check if the locale name consists of 2 parts (language_country) */
        if(langStr.contains("_"))
        {
            /** display language strings as "native language - native country/territory (locale name)", e.g. "Deutsch - Deutschland (de)" */
            ui->lang->addItem(locale.nativeLanguageName() + QString(" - ") +
                              locale.nativeTerritoryName() +
                              QString(" (") + langStr + QString(")"), QVariant(langStr));

        }
        else
        {
            /** display language strings as "native language (locale name)", e.g. "Deutsch (de)" */
            ui->lang->addItem(locale.nativeLanguageName() + QString(" (") + langStr + QString(")"), QVariant(langStr));
        }
    }
    ui->unit->setModel(new BitcoinUnits(this));

    /* Theme, which is applied as soon as the dialog is accepted. */
    auto* theme_row{new QHBoxLayout};
    auto* theme_label{new QLabel(tr("&Colours:"), this)};
    m_theme = new QComboBox(this);
    m_theme->setObjectName("theme");
    m_theme->setToolTip(tr("Colours of the windows. Any colours go with any font here; the sidebar has ready-made combinations."));
    for (const auto& theme : Theme::Available()) m_theme->addItem(theme.name, theme.id);
    m_theme->setCurrentIndex(std::max(0, m_theme->findData(Theme::Saved())));
    theme_label->setBuddy(m_theme);
    theme_row->addWidget(theme_label);
    theme_row->addWidget(m_theme, 1);
    ui->verticalLayout_Display->insertLayout(1, theme_row);

    /* Font of the whole interface. */
    auto* font_row{new QHBoxLayout};
    auto* font_label{new QLabel(tr("&Font:"), this)};
    m_font_family = new QComboBox(this);
    m_font_family->setObjectName("fontFamily");
    m_font_family->addItem(tr("(as on the desktop)"), QString{});
    for (const QString& family : QFontDatabase::families()) m_font_family->addItem(family, family);
    m_font_family->setCurrentIndex(std::max(0, m_font_family->findData(Theme::SavedFontFamily())));
    font_label->setBuddy(m_font_family);
    m_font_size = new QSpinBox(this);
    m_font_size->setObjectName("fontSize");
    m_font_size->setRange(Theme::MIN_FONT_SIZE_ADJUSTMENT, Theme::MAX_FONT_SIZE_ADJUSTMENT);
    m_font_size->setPrefix(tr("size "));
    m_font_size->setSuffix(tr(" pt"));
    m_font_size->setToolTip(tr("How many points larger, or smaller if negative, than on the desktop."));
    m_font_size->setValue(Theme::SavedFontSizeAdjustment());
    font_row->addWidget(font_label);
    font_row->addWidget(m_font_family, 1);
    font_row->addWidget(m_font_size);
    ui->verticalLayout_Display->insertLayout(2, font_row);

    /* Frame of the main window. */
    m_themed_frame = new QCheckBox(tr("Frame the main &window in the colours of the theme"), this);
    m_themed_frame->setObjectName("themedFrame");
    m_themed_frame->setToolTip(tr("Draw the title bar and the border of the main window in the colours of the theme, with the menus in the title bar, in place of the frame of the desktop. Has no effect with the colours of the desktop."));
    m_themed_frame->setChecked(Theme::SavedFrame());
    ui->verticalLayout_Display->insertWidget(3, m_themed_frame);

    /* Widget-to-option mapper */
    mapper = new QDataWidgetMapper(this);
    mapper->setSubmitPolicy(QDataWidgetMapper::ManualSubmit);
    mapper->setOrientation(Qt::Vertical);

    GUIUtil::ItemDelegate* delegate = new GUIUtil::ItemDelegate(mapper);
    connect(delegate, &GUIUtil::ItemDelegate::keyEscapePressed, this, &OptionsDialog::reject);
    mapper->setItemDelegate(delegate);

    /* setup/change UI elements when proxy IPs are invalid/valid */
    ui->proxyIp->setCheckValidator(new ProxyAddressValidator(parent));
    ui->proxyIpTor->setCheckValidator(new ProxyAddressValidator(parent));
    connect(ui->proxyIp, &QValidatedLineEdit::validationDidChange, this, &OptionsDialog::updateProxyValidationState);
    connect(ui->proxyIpTor, &QValidatedLineEdit::validationDidChange, this, &OptionsDialog::updateProxyValidationState);
    connect(ui->proxyPort, &QLineEdit::textChanged, this, &OptionsDialog::updateProxyValidationState);
    connect(ui->proxyPortTor, &QLineEdit::textChanged, this, &OptionsDialog::updateProxyValidationState);

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        ui->showTrayIcon->setChecked(false);
        ui->showTrayIcon->setEnabled(false);
        ui->minimizeToTray->setChecked(false);
        ui->minimizeToTray->setEnabled(false);
    }

    setupFontOptions(ui->moneyFont, ui->moneyFont_preview);

    GUIUtil::handleCloseWindowShortcut(this);
}

OptionsDialog::~OptionsDialog()
{
    delete ui;
}

void OptionsDialog::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
}

void OptionsDialog::setModel(OptionsModel *_model)
{
    this->model = _model;

    if(_model)
    {
        /* check if client restart is needed and show persistent message */
        if (_model->isRestartRequired())
            showRestartWarning(true);

        // Prune values are in GB to be consistent with intro.cpp
        static constexpr uint64_t nMinDiskSpace = (MIN_DISK_SPACE_FOR_BLOCK_FILES / GB_BYTES) + (MIN_DISK_SPACE_FOR_BLOCK_FILES % GB_BYTES) ? 1 : 0;
        ui->pruneSize->setRange(nMinDiskSpace, std::numeric_limits<int>::max());

        QString strLabel = _model->getOverriddenByCommandLine();
        if (strLabel.isEmpty())
            strLabel = tr("none");
        ui->overriddenByCommandLineLabel->setText(strLabel);

        mapper->setModel(_model);
        setMapper();
        mapper->toFirst();

        const auto& font_for_money = _model->data(_model->index(OptionsModel::FontForMoney, 0), Qt::EditRole).value<OptionsModel::FontChoice>();
        setFontChoice(ui->moneyFont, font_for_money);

        updateDefaultProxyNets();
    }

    /* warn when one of the following settings changes by user action (placed here so init via mapper doesn't trigger them) */

    /* Main */
    connect(ui->prune, &QCheckBox::clicked, this, &OptionsDialog::showRestartWarning);
    connect(ui->prune, &QCheckBox::clicked, this, &OptionsDialog::togglePruneWarning);
    connect(ui->pruneSize, qOverload<int>(&QSpinBox::valueChanged), this, &OptionsDialog::showRestartWarning);
    connect(ui->databaseCache, qOverload<int>(&QSpinBox::valueChanged), this, &OptionsDialog::showRestartWarning);
    connect(ui->externalSignerPath, &QLineEdit::textChanged, [this]{ showRestartWarning(); });
    connect(ui->threadsScriptVerif, qOverload<int>(&QSpinBox::valueChanged), this, &OptionsDialog::showRestartWarning);
    /* Wallet */
    connect(ui->spendZeroConfChange, &QCheckBox::clicked, this, &OptionsDialog::showRestartWarning);
    /* Network */
    connect(ui->allowIncoming, &QCheckBox::clicked, this, &OptionsDialog::showRestartWarning);
    connect(ui->enableServer, &QCheckBox::clicked, this, &OptionsDialog::showRestartWarning);
    connect(ui->connectSocks, &QCheckBox::clicked, this, &OptionsDialog::showRestartWarning);
    connect(ui->connectSocksTor, &QCheckBox::clicked, this, &OptionsDialog::showRestartWarning);
    /* Display */
    connect(ui->lang, qOverload<>(&QValueComboBox::valueChanged), [this]{ showRestartWarning(); });
    connect(ui->thirdPartyTxUrls, &QLineEdit::textChanged, [this]{ showRestartWarning(); });
}

void OptionsDialog::setCurrentTab(OptionsDialog::Tab tab)
{
    QWidget *tab_widget = nullptr;
    if (tab == OptionsDialog::Tab::TAB_NETWORK) tab_widget = ui->tabNetwork;
    if (tab == OptionsDialog::Tab::TAB_MAIN) tab_widget = ui->tabMain;
    if (tab == OptionsDialog::Tab::TAB_DISPLAY) tab_widget = ui->tabDisplay;
    if (tab_widget && ui->tabWidget->currentWidget() != tab_widget) {
        ui->tabWidget->setCurrentWidget(tab_widget);
    }
}

void OptionsDialog::setMapper()
{
    /* Main */
    mapper->addMapping(ui->bitcoinAtStartup, OptionsModel::StartAtStartup);
    mapper->addMapping(ui->threadsScriptVerif, OptionsModel::ThreadsScriptVerif);
    mapper->addMapping(ui->databaseCache, OptionsModel::DatabaseCache);
    mapper->addMapping(ui->prune, OptionsModel::Prune);
    mapper->addMapping(ui->pruneSize, OptionsModel::PruneSize);

    /* Wallet */
    mapper->addMapping(ui->spendZeroConfChange, OptionsModel::SpendZeroConfChange);
    mapper->addMapping(ui->coinControlFeatures, OptionsModel::CoinControlFeatures);
    mapper->addMapping(ui->subFeeFromAmount, OptionsModel::SubFeeFromAmount);
    mapper->addMapping(ui->externalSignerPath, OptionsModel::ExternalSignerPath);
    mapper->addMapping(ui->m_enable_psbt_controls, OptionsModel::EnablePSBTControls);

    /* Network */
    mapper->addMapping(ui->mapPortNatpmp, OptionsModel::MapPortNatpmp);
    mapper->addMapping(ui->allowIncoming, OptionsModel::Listen);
    mapper->addMapping(ui->enableServer, OptionsModel::Server);

    mapper->addMapping(ui->connectSocks, OptionsModel::ProxyUse);
    mapper->addMapping(ui->proxyIp, OptionsModel::ProxyIP);
    mapper->addMapping(ui->proxyPort, OptionsModel::ProxyPort);

    mapper->addMapping(ui->connectSocksTor, OptionsModel::ProxyUseTor);
    mapper->addMapping(ui->proxyIpTor, OptionsModel::ProxyIPTor);
    mapper->addMapping(ui->proxyPortTor, OptionsModel::ProxyPortTor);

    /* Window */
#ifndef Q_OS_MACOS
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        mapper->addMapping(ui->showTrayIcon, OptionsModel::ShowTrayIcon);
        mapper->addMapping(ui->minimizeToTray, OptionsModel::MinimizeToTray);
    }
    mapper->addMapping(ui->minimizeOnClose, OptionsModel::MinimizeOnClose);
#endif

    /* Display */
    mapper->addMapping(ui->lang, OptionsModel::Language);
    mapper->addMapping(ui->unit, OptionsModel::DisplayUnit);
    mapper->addMapping(ui->thirdPartyTxUrls, OptionsModel::ThirdPartyTxUrls);
}

void OptionsDialog::setOkButtonState(bool fState)
{
    ui->okButton->setEnabled(fState);
}

void OptionsDialog::on_resetButton_clicked()
{
    if (model) {
        // confirmation dialog
        /*: Text explaining that the settings changed will not come into effect
            until the client is restarted. */
        QString reset_dialog_text = tr("Client restart required to activate changes.") + "<br><br>";
        /*: Text explaining to the user that the client's current settings
            will be backed up at a specific location. %1 is a stand-in
            argument for the backup location's path. */
        reset_dialog_text.append(tr("Current settings will be backed up at \"%1\".").arg(m_client_model->dataDir()) + "<br><br>");
        /*: Text asking the user to confirm if they would like to proceed
            with a client shutdown. */
        reset_dialog_text.append(tr("Client will be shut down. Do you want to proceed?"));
        //: Window title text of pop-up window shown when the user has chosen to reset options.
        QMessageBox::StandardButton btnRetVal = QMessageBox::question(this, tr("Confirm options reset"),
            reset_dialog_text, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);

        if (btnRetVal == QMessageBox::Cancel)
            return;

        /* reset all options and close GUI */
        model->Reset();
        close();
        Q_EMIT quitOnReset();
    }
}

void OptionsDialog::on_openBitcoinConfButton_clicked()
{
    QMessageBox config_msgbox(this);
    config_msgbox.setIcon(QMessageBox::Information);
    //: Window title text of pop-up box that allows opening up of configuration file.
    config_msgbox.setWindowTitle(tr("Configuration options"));
    /*: Explanatory text about the priority order of instructions considered by client.
        The order from high to low being: command-line, configuration file, GUI settings. */
    config_msgbox.setText(tr("The configuration file is used to specify advanced user options which override GUI settings. "
                             "Additionally, any command-line options will override this configuration file."));

    QPushButton* open_button = config_msgbox.addButton(tr("Continue"), QMessageBox::ActionRole);
    config_msgbox.addButton(tr("Cancel"), QMessageBox::RejectRole);
    open_button->setDefault(true);

    config_msgbox.exec();

    if (config_msgbox.clickedButton() != open_button) return;

    /* show an error if there was some problem opening the file */
    if (!GUIUtil::openBitcoinConf())
        QMessageBox::critical(this, tr("Error"), tr("The configuration file could not be opened."));
}

void OptionsDialog::on_okButton_clicked()
{
    model->setData(model->index(OptionsModel::FontForMoney, 0), ui->moneyFont->itemData(ui->moneyFont->currentIndex()));

    mapper->submit();
    if (m_theme->currentData().toString() != Theme::Saved()) Theme::Save(m_theme->currentData().toString());
    if (m_font_family->currentData().toString() != Theme::SavedFontFamily() || m_font_size->value() != Theme::SavedFontSizeAdjustment()) {
        Theme::SaveFont(m_font_family->currentData().toString(), m_font_size->value());
    }
    if (m_themed_frame->isChecked() != Theme::SavedFrame()) Theme::SaveFrame(m_themed_frame->isChecked());
    accept();
    updateDefaultProxyNets();
}

void OptionsDialog::on_cancelButton_clicked()
{
    reject();
}

void OptionsDialog::on_showTrayIcon_stateChanged(int state)
{
    if (state == Qt::Checked) {
        ui->minimizeToTray->setEnabled(true);
    } else {
        ui->minimizeToTray->setChecked(false);
        ui->minimizeToTray->setEnabled(false);
    }
}

void OptionsDialog::togglePruneWarning(bool enabled)
{
    ui->pruneWarning->setVisible(!ui->pruneWarning->isVisible());
}

void OptionsDialog::showRestartWarning(bool fPersistent)
{
    ui->statusLabel->setStyleSheet("QLabel { color: red; }");

    if(fPersistent)
    {
        ui->statusLabel->setText(tr("Client restart required to activate changes."));
    }
    else
    {
        ui->statusLabel->setText(tr("This change would require a client restart."));
        // clear non-persistent status label after 10 seconds
        // Todo: should perhaps be a class attribute, if we extend the use of statusLabel
        QTimer::singleShot(10s, this, &OptionsDialog::clearStatusLabel);
    }
}

void OptionsDialog::clearStatusLabel()
{
    ui->statusLabel->clear();
    if (model && model->isRestartRequired()) {
        showRestartWarning(true);
    }
}

void OptionsDialog::updateProxyValidationState()
{
    QValidatedLineEdit *pUiProxyIp = ui->proxyIp;
    QValidatedLineEdit *otherProxyWidget = (pUiProxyIp == ui->proxyIpTor) ? ui->proxyIp : ui->proxyIpTor;
    if (pUiProxyIp->isValid() && (!ui->proxyPort->isEnabled() || ui->proxyPort->text().toInt() > 0) && (!ui->proxyPortTor->isEnabled() || ui->proxyPortTor->text().toInt() > 0))
    {
        setOkButtonState(otherProxyWidget->isValid()); //only enable ok button if both proxies are valid
        clearStatusLabel();
    }
    else
    {
        setOkButtonState(false);
        ui->statusLabel->setStyleSheet("QLabel { color: red; }");
        ui->statusLabel->setText(tr("The supplied proxy address is invalid."));
    }
}

void OptionsDialog::updateDefaultProxyNets()
{
    std::string proxyIpText{ui->proxyIp->text().toStdString()};
    if (!IsUnixSocketPath(proxyIpText)) {
        const std::optional<CNetAddr> ui_proxy_netaddr{LookupHost(proxyIpText, /*fAllowLookup=*/false)};
        const CService ui_proxy{ui_proxy_netaddr.value_or(CNetAddr{}), ui->proxyPort->text().toUShort()};
        proxyIpText = ui_proxy.ToStringAddrPort();
    }

    const auto proxy_ipv4 = model->node().getProxy(NET_IPV4);
    ui->proxyReachIPv4->setChecked(proxy_ipv4 && proxy_ipv4->ToString() == proxyIpText);

    const auto proxy_ipv6 = model->node().getProxy(NET_IPV6);
    ui->proxyReachIPv6->setChecked(proxy_ipv6 && proxy_ipv6->ToString() == proxyIpText);

    const auto proxy_onion = model->node().getProxy(NET_ONION);
    ui->proxyReachTor->setChecked(proxy_onion && proxy_onion->ToString() == proxyIpText);
}

ProxyAddressValidator::ProxyAddressValidator(QObject *parent) :
QValidator(parent)
{
}

QValidator::State ProxyAddressValidator::validate(QString &input, int &pos) const
{
    Q_UNUSED(pos);
    uint16_t port{0};
    std::string hostname;
    if (!SplitHostPort(input.toStdString(), port, hostname) || port != 0) return QValidator::Invalid;

    CService serv(LookupNumeric(input.toStdString(), DEFAULT_GUI_PROXY_PORT));
    Proxy addrProxy = Proxy(serv, /*tor_stream_isolation=*/true);
    if (addrProxy.IsValid())
        return QValidator::Acceptable;

    return QValidator::Invalid;
}
