// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: 2020-2022 The Monero Project

#include "MainWindow.h"
#include "ui_MainWindow.h"

#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>

#include "config-feather.h"
#include "constants.h"
#include "dialog/AccountSwitcherDialog.h"
#include "dialog/BalanceDialog.h"
#include "dialog/DebugInfoDialog.h"
#include "dialog/PasswordDialog.h"
#include "dialog/TorInfoDialog.h"
#include "dialog/TxBroadcastDialog.h"
#include "dialog/TxConfAdvDialog.h"
#include "dialog/TxConfDialog.h"
#include "dialog/TxImportDialog.h"
#include "dialog/TxInfoDialog.h"
#include "dialog/ViewOnlyDialog.h"
#include "dialog/WalletInfoDialog.h"
#include "dialog/WalletCacheDebugDialog.h"
#include "dialog/UpdateDialog.h"
#include "libwalletqt/AddressBook.h"
#include "libwalletqt/CoinsInfo.h"
#include "libwalletqt/Transfer.h"
#include "utils/AppData.h"
#include "utils/AsyncTask.h"
#include "utils/ColorScheme.h"
#include "utils/Icons.h"
#include "utils/NetworkManager.h"
#include "utils/os/tails.h"
#include "utils/SemanticVersion.h"
#include "utils/TorManager.h"
#include "utils/Updater.h"
#include "utils/WebsocketNotifier.h"

MainWindow::MainWindow(WindowManager *windowManager, Wallet *wallet, QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_windowManager(windowManager)
    , m_ctx(new AppContext(wallet))
{
    ui->setupUi(this);

    // Ensure the destructor is called after closeEvent()
    setAttribute(Qt::WA_DeleteOnClose);

    m_windowCalc = new CalcWindow(this);
    m_splashDialog = new SplashDialog(this);

    this->restoreGeo();

    this->initStatusBar();
    this->initWidgets();
    this->initMenu();
    this->initHome();
    this->initWalletContext();

    // Websocket notifier
    connect(websocketNotifier(), &WebsocketNotifier::CCSReceived, ui->ccsWidget->model(), &CCSModel::updateEntries);
    connect(websocketNotifier(), &WebsocketNotifier::BountyReceived, ui->bountiesWidget->model(), &BountiesModel::updateBounties);
    connect(websocketNotifier(), &WebsocketNotifier::RedditReceived, ui->redditWidget->model(), &RedditModel::updatePosts);
    connect(websocketNotifier(), &WebsocketNotifier::RevuoReceived, ui->revuoWidget, &RevuoWidget::updateItems);
    connect(websocketNotifier(), &WebsocketNotifier::UpdatesReceived, this, &MainWindow::onUpdatesAvailable);
#ifdef HAS_XMRIG
    connect(websocketNotifier(), &WebsocketNotifier::XMRigDownloadsReceived, m_xmrig, &XMRigWidget::onDownloads);
#endif
    websocketNotifier()->emitCache(); // Get cached data

    connect(m_windowManager, &WindowManager::websocketStatusChanged, this, &MainWindow::onWebsocketStatusChanged);
    this->onWebsocketStatusChanged(!config()->get(Config::disableWebsocket).toBool());

    connect(m_windowManager, &WindowManager::torSettingsChanged, m_ctx.get(), &AppContext::onTorSettingsChanged);
    connect(torManager(), &TorManager::connectionStateChanged, this, &MainWindow::onTorConnectionStateChanged);
    this->onTorConnectionStateChanged(torManager()->torConnected);

    ColorScheme::updateFromWidget(this);
    QTimer::singleShot(1, [this]{this->updateWidgetIcons();});

    // Timers
    connect(&m_updateBytes, &QTimer::timeout, this, &MainWindow::updateNetStats);
    connect(&m_txTimer, &QTimer::timeout, [this]{
        m_statusLabelStatus->setText("Constructing transaction" + this->statusDots());
    });

    config()->set(Config::firstRun, false);

    this->onWalletOpened();

#ifdef DONATE_BEG
    this->donationNag();
#endif

    connect(m_windowManager->eventFilter, &EventFilter::userActivity, this, &MainWindow::userActivity);
    connect(&m_checkUserActivity, &QTimer::timeout, this, &MainWindow::checkUserActivity);
    m_checkUserActivity.setInterval(5000);
    m_checkUserActivity.start();
}

void MainWindow::initStatusBar() {
#if defined(Q_OS_WIN)
    // No seperators between statusbar widgets
    this->statusBar()->setStyleSheet("QStatusBar::item {border: None;}");
#endif

#if defined(Q_OS_MACOS)
    this->patchStylesheetMac();
#endif

    this->statusBar()->setFixedHeight(30);

    m_statusLabelStatus = new QLabel("Idle", this);
    m_statusLabelStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->statusBar()->addWidget(m_statusLabelStatus);

    m_statusLabelNetStats = new QLabel("", this);
    m_statusLabelNetStats->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->statusBar()->addWidget(m_statusLabelNetStats);

    m_statusUpdateAvailable = new QPushButton(this);
    m_statusUpdateAvailable->setFlat(true);
    m_statusUpdateAvailable->setCursor(Qt::PointingHandCursor);
    m_statusUpdateAvailable->setIcon(icons()->icon("tab_party.png"));
    m_statusUpdateAvailable->hide();
    this->statusBar()->addPermanentWidget(m_statusUpdateAvailable);

    m_statusLabelBalance = new ClickableLabel(this);
    m_statusLabelBalance->setText("Balance: 0 XMR");
    m_statusLabelBalance->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLabelBalance->setCursor(Qt::PointingHandCursor);
    this->statusBar()->addPermanentWidget(m_statusLabelBalance);
    connect(m_statusLabelBalance, &ClickableLabel::clicked, this, &MainWindow::showBalanceDialog);

    m_statusBtnConnectionStatusIndicator = new StatusBarButton(icons()->icon("status_disconnected.svg"), "Connection status", this);
    connect(m_statusBtnConnectionStatusIndicator, &StatusBarButton::clicked, [this](){
        this->onShowSettingsPage(2);
    });
    this->statusBar()->addPermanentWidget(m_statusBtnConnectionStatusIndicator);

    m_statusAccountSwitcher = new StatusBarButton(icons()->icon("change_account.png"), "Account switcher", this);
    connect(m_statusAccountSwitcher, &StatusBarButton::clicked, this, &MainWindow::showAccountSwitcherDialog);
    this->statusBar()->addPermanentWidget(m_statusAccountSwitcher);

    m_statusBtnPassword = new StatusBarButton(icons()->icon("lock.svg"), "Password", this);
    connect(m_statusBtnPassword, &StatusBarButton::clicked, this, &MainWindow::showPasswordDialog);
    this->statusBar()->addPermanentWidget(m_statusBtnPassword);

    m_statusBtnPreferences = new StatusBarButton(icons()->icon("preferences.svg"), "Settings", this);
    connect(m_statusBtnPreferences, &StatusBarButton::clicked, this, &MainWindow::menuSettingsClicked);
    this->statusBar()->addPermanentWidget(m_statusBtnPreferences);

    m_statusBtnSeed = new StatusBarButton(icons()->icon("seed.png"), "Seed", this);
    connect(m_statusBtnSeed, &StatusBarButton::clicked, this, &MainWindow::showSeedDialog);
    this->statusBar()->addPermanentWidget(m_statusBtnSeed);

    m_statusBtnTor = new StatusBarButton(icons()->icon("tor_logo_disabled.png"), "Tor settings", this);
    connect(m_statusBtnTor, &StatusBarButton::clicked, this, &MainWindow::menuTorClicked);
    this->statusBar()->addPermanentWidget(m_statusBtnTor);

    m_statusBtnHwDevice = new StatusBarButton(this->hardwareDevicePairedIcon(), this->getHardwareDevice(), this);
    connect(m_statusBtnHwDevice, &StatusBarButton::clicked, this, &MainWindow::menuHwDeviceClicked);
    this->statusBar()->addPermanentWidget(m_statusBtnHwDevice);
    m_statusBtnHwDevice->hide();
}

void MainWindow::initWidgets() {
    int homeWidget = config()->get(Config::homeWidget).toInt();
    ui->tabHomeWidget->setCurrentIndex(TabsHome(homeWidget));

    // [History]
    m_historyWidget = new HistoryWidget(m_ctx, this);
    ui->historyWidgetLayout->addWidget(m_historyWidget);
    connect(m_historyWidget, &HistoryWidget::viewOnBlockExplorer, this, &MainWindow::onViewOnBlockExplorer);
    connect(m_historyWidget, &HistoryWidget::resendTransaction, this, &MainWindow::onResendTransaction);

    // [Send]
    m_sendWidget = new SendWidget(m_ctx, this);
    ui->sendWidgetLayout->addWidget(m_sendWidget);
    // --------------
    m_contactsWidget = new ContactsWidget(m_ctx, this);
    ui->contactsWidgetLayout->addWidget(m_contactsWidget);

    // [Receive]
    m_receiveWidget = new ReceiveWidget(m_ctx, this);
    ui->receiveWidgetLayout->addWidget(m_receiveWidget);
    connect(m_receiveWidget, &ReceiveWidget::showTransactions, [this](const QString &text) {
        m_historyWidget->setSearchText(text);
        ui->tabWidget->setCurrentIndex(Tabs::HISTORY);
    });
    connect(m_contactsWidget, &ContactsWidget::fillAddress, m_sendWidget, &SendWidget::fillAddress);

    // [Coins]
    m_coinsWidget = new CoinsWidget(m_ctx, this);
    ui->coinsWidgetLayout->addWidget(m_coinsWidget);

#ifdef HAS_LOCALMONERO
    m_localMoneroWidget = new LocalMoneroWidget(this, m_ctx);
    ui->localMoneroLayout->addWidget(m_localMoneroWidget);
#else
    ui->tabWidgetExchanges->setTabVisible(0, false);
#endif

#ifdef HAS_XMRIG
    m_xmrig = new XMRigWidget(m_ctx, this);
    ui->xmrRigLayout->addWidget(m_xmrig);

    connect(m_xmrig, &XMRigWidget::miningStarted, [this]{ this->updateTitle(); });
    connect(m_xmrig, &XMRigWidget::miningEnded, [this]{ this->updateTitle(); });
#else
    ui->tabWidget->setTabVisible(Tabs::XMRIG, false);
#endif

#if defined(Q_OS_MACOS)
    ui->line->hide();
#endif

    ui->frame_coinControl->setVisible(false);
    connect(ui->btn_resetCoinControl, &QPushButton::clicked, [this]{
       m_ctx->setSelectedInputs({});
    });
}

void MainWindow::initMenu() {
    // TODO: Rename actions to follow style
    // [File]
    connect(ui->actionOpen,        &QAction::triggered, this, &MainWindow::menuOpenClicked);
    connect(ui->actionNew_Restore, &QAction::triggered, this, &MainWindow::menuNewRestoreClicked);
    connect(ui->actionClose,       &QAction::triggered, this, &MainWindow::menuWalletCloseClicked); // Close current wallet
    connect(ui->actionQuit,        &QAction::triggered, this, &MainWindow::menuQuitClicked);        // Quit application
    connect(ui->actionSettings,    &QAction::triggered, this, &MainWindow::menuSettingsClicked);

    // [File] -> [Recently open]
    m_clearRecentlyOpenAction = new QAction("Clear history", ui->menuFile);
    connect(m_clearRecentlyOpenAction, &QAction::triggered, this, &MainWindow::menuClearHistoryClicked);

    // [Wallet]
    connect(ui->actionInformation,  &QAction::triggered, this, &MainWindow::showWalletInfoDialog);
    connect(ui->actionAccount,      &QAction::triggered, this, &MainWindow::showAccountSwitcherDialog);
    connect(ui->actionPassword,     &QAction::triggered, this, &MainWindow::showPasswordDialog);
    connect(ui->actionSeed,         &QAction::triggered, this, &MainWindow::showSeedDialog);
    connect(ui->actionKeys,         &QAction::triggered, this, &MainWindow::showKeysDialog);
    connect(ui->actionViewOnly,     &QAction::triggered, this, &MainWindow::showViewOnlyDialog);

    // [Wallet] -> [Advanced]
    connect(ui->actionStore_wallet,          &QAction::triggered, this, &MainWindow::tryStoreWallet);
    connect(ui->actionUpdate_balance,        &QAction::triggered, [this]{m_ctx->updateBalance();});
    connect(ui->actionRefresh_tabs,          &QAction::triggered, [this]{m_ctx->refreshModels();});
    connect(ui->actionRescan_spent,          &QAction::triggered, this, &MainWindow::rescanSpent);
    connect(ui->actionWallet_cache_debug,    &QAction::triggered, this, &MainWindow::showWalletCacheDebugDialog);

    // [Wallet] -> [Advanced] -> [Export]
    connect(ui->actionExportOutputs,   &QAction::triggered, this, &MainWindow::exportOutputs);
    connect(ui->actionExportKeyImages, &QAction::triggered, this, &MainWindow::exportKeyImages);

    // [Wallet] -> [Advanced] -> [Import]
    connect(ui->actionImportOutputs,   &QAction::triggered, this, &MainWindow::importOutputs);
    connect(ui->actionImportKeyImages, &QAction::triggered, this, &MainWindow::importKeyImages);

    // [Wallet] -> [History]
    connect(ui->actionExport_CSV, &QAction::triggered, this, &MainWindow::onExportHistoryCSV);

    // [Wallet] -> [Contacts]
    connect(ui->actionExportContactsCSV, &QAction::triggered, this, &MainWindow::onExportContactsCSV);
    connect(ui->actionImportContactsCSV, &QAction::triggered, this, &MainWindow::importContacts);

    // [View]
    m_tabShowHideSignalMapper = new QSignalMapper(this);
    connect(ui->actionShow_Searchbar, &QAction::toggled, this, &MainWindow::toggleSearchbar);
    ui->actionShow_Searchbar->setChecked(config()->get(Config::showSearchbar).toBool());

    // Show/Hide Home
    connect(ui->actionShow_Home, &QAction::triggered, m_tabShowHideSignalMapper, QOverload<>::of(&QSignalMapper::map));
    m_tabShowHideMapper["Home"] = new ToggleTab(ui->tabHome, "Home", "Home", ui->actionShow_Home, Config::showTabHome);
    m_tabShowHideSignalMapper->setMapping(ui->actionShow_Home, "Home");

    // Show/Hide Coins
    connect(ui->actionShow_Coins, &QAction::triggered, m_tabShowHideSignalMapper, QOverload<>::of(&QSignalMapper::map));
    m_tabShowHideMapper["Coins"] = new ToggleTab(ui->tabCoins, "Coins", "Coins", ui->actionShow_Coins, Config::showTabCoins);
    m_tabShowHideSignalMapper->setMapping(ui->actionShow_Coins, "Coins");

    // Show/Hide Calc
    connect(ui->actionShow_calc, &QAction::triggered, m_tabShowHideSignalMapper, QOverload<>::of(&QSignalMapper::map));
    m_tabShowHideMapper["Calc"] = new ToggleTab(ui->tabCalc, "Calc", "Calc", ui->actionShow_calc, Config::showTabCalc);
    m_tabShowHideSignalMapper->setMapping(ui->actionShow_calc, "Calc");

    // Show/Hide Exchange
#if defined(HAS_LOCALMONERO)
    connect(ui->actionShow_Exchange, &QAction::triggered, m_tabShowHideSignalMapper, QOverload<>::of(&QSignalMapper::map));
    m_tabShowHideMapper["Exchange"] = new ToggleTab(ui->tabExchange, "Exchange", "Exchange", ui->actionShow_Exchange, Config::showTabExchange);
    m_tabShowHideSignalMapper->setMapping(ui->actionShow_Exchange, "Exchange");
#else
    ui->actionShow_Exchange->setVisible(false);
    ui->tabWidget->setTabVisible(Tabs::EXCHANGES, false);
#endif

    // Show/Hide Mining
#if defined(HAS_XMRIG)
    connect(ui->actionShow_XMRig, &QAction::triggered, m_tabShowHideSignalMapper, QOverload<>::of(&QSignalMapper::map));
    m_tabShowHideMapper["Mining"] = new ToggleTab(ui->tabXmrRig, "Mining", "Mining", ui->actionShow_XMRig, Config::showTabXMRig);
    m_tabShowHideSignalMapper->setMapping(ui->actionShow_XMRig, "Mining");
#else
    ui->actionShow_XMRig->setVisible(false);
#endif

    for (const auto &key: m_tabShowHideMapper.keys()) {
        const auto toggleTab = m_tabShowHideMapper.value(key);
        const bool show = config()->get(toggleTab->configKey).toBool();
        toggleTab->menuAction->setText((show ? QString("Hide ") : QString("Show ")) + toggleTab->name);
        ui->tabWidget->setTabVisible(ui->tabWidget->indexOf(toggleTab->tab), show);
    }
    connect(m_tabShowHideSignalMapper, &QSignalMapper::mappedString, this, &MainWindow::menuToggleTabVisible);

    // [Tools]
    connect(ui->actionSignVerify,                  &QAction::triggered, this, &MainWindow::menuSignVerifyClicked);
    connect(ui->actionVerifyTxProof,               &QAction::triggered, this, &MainWindow::menuVerifyTxProof);
    connect(ui->actionLoadUnsignedTxFromFile,      &QAction::triggered, this, &MainWindow::loadUnsignedTx);
    connect(ui->actionLoadUnsignedTxFromClipboard, &QAction::triggered, this, &MainWindow::loadUnsignedTxFromClipboard);
    connect(ui->actionLoadSignedTxFromFile,        &QAction::triggered, this, &MainWindow::loadSignedTx);
    connect(ui->actionLoadSignedTxFromText,        &QAction::triggered, this, &MainWindow::loadSignedTxFromText);
    connect(ui->actionImport_transaction,          &QAction::triggered, this, &MainWindow::importTransaction);
    connect(ui->actionPay_to_many,                 &QAction::triggered, this, &MainWindow::payToMany);
    connect(ui->actionAddress_checker,             &QAction::triggered, this, &MainWindow::showAddressChecker);
    connect(ui->actionCalculator,                  &QAction::triggered, this, &MainWindow::showCalcWindow);
    connect(ui->actionCreateDesktopEntry,          &QAction::triggered, this, &MainWindow::onCreateDesktopEntry);

    // TODO: Allow creating desktop entry on Windows and Mac
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    ui->actionCreateDesktopEntry->setDisabled(true);
#endif

#ifndef SELF_CONTAINED
    ui->actionCreateDesktopEntry->setVisible(false);
#endif

    // [Help]
    connect(ui->actionAbout,             &QAction::triggered, this, &MainWindow::menuAboutClicked);
    connect(ui->actionOfficialWebsite,   &QAction::triggered, [this](){Utils::externalLinkWarning(this, "https://featherwallet.org");});
    connect(ui->actionDonate_to_Feather, &QAction::triggered, this, &MainWindow::donateButtonClicked);
    connect(ui->actionDocumentation,     &QAction::triggered, this, &MainWindow::onShowDocumentaton);
    connect(ui->actionReport_bug,        &QAction::triggered, this, &MainWindow::onReportBug);
    connect(ui->actionShow_debug_info,   &QAction::triggered, this, &MainWindow::showDebugInfo);


    // Setup shortcuts
    ui->actionStore_wallet->setShortcut(QKeySequence("Ctrl+S"));
    ui->actionRefresh_tabs->setShortcut(QKeySequence("Ctrl+R"));
    ui->actionOpen->setShortcut(QKeySequence("Ctrl+O"));
    ui->actionNew_Restore->setShortcut(QKeySequence("Ctrl+N"));
    ui->actionClose->setShortcut(QKeySequence("Ctrl+W"));
    ui->actionShow_debug_info->setShortcut(QKeySequence("Ctrl+D"));
    ui->actionSettings->setShortcut(QKeySequence("Ctrl+Alt+S"));
    ui->actionUpdate_balance->setShortcut(QKeySequence("Ctrl+U"));
    ui->actionShow_Searchbar->setShortcut(QKeySequence("Ctrl+F"));
    ui->actionDocumentation->setShortcut(QKeySequence("F1"));
}

void MainWindow::initHome() {
    // Ticker widgets
    m_tickerWidgets.append(new PriceTickerWidget(this, m_ctx, "XMR"));
    m_tickerWidgets.append(new PriceTickerWidget(this, m_ctx, "BTC"));
    m_tickerWidgets.append(new RatioTickerWidget(this, m_ctx, "XMR", "BTC"));
    for (const auto &widget : m_tickerWidgets) {
        ui->tickerLayout->addWidget(widget);
    }

    m_balanceTickerWidget = new BalanceTickerWidget(this, m_ctx, false);
    ui->fiatTickerLayout->addWidget(m_balanceTickerWidget);

    connect(ui->ccsWidget, &CCSWidget::selected, this, &MainWindow::showSendScreen);
    connect(ui->bountiesWidget, &BountiesWidget::donate, this, &MainWindow::fillSendTab);
    connect(ui->redditWidget, &RedditWidget::setStatusText, this, &MainWindow::setStatusText);
    connect(ui->revuoWidget, &RevuoWidget::donate, [this](const QString &address, const QString &description){
        m_sendWidget->fill(address, description);
        ui->tabWidget->setCurrentIndex(Tabs::SEND);
    });
}

void MainWindow::initWalletContext() {
    connect(m_ctx.get(), &AppContext::balanceUpdated,           this, &MainWindow::onBalanceUpdated);
    connect(m_ctx.get(), &AppContext::synchronized,             this, &MainWindow::onSynchronized);
    connect(m_ctx.get(), &AppContext::blockchainSync,           this, &MainWindow::onBlockchainSync);
    connect(m_ctx.get(), &AppContext::refreshSync,              this, &MainWindow::onRefreshSync);
    connect(m_ctx.get(), &AppContext::createTransactionError,   this, &MainWindow::onCreateTransactionError);
    connect(m_ctx.get(), &AppContext::createTransactionSuccess, this, &MainWindow::onCreateTransactionSuccess);
    connect(m_ctx.get(), &AppContext::transactionCommitted,     this, &MainWindow::onTransactionCommitted);
    connect(m_ctx.get(), &AppContext::deviceError,              this, &MainWindow::onDeviceError);
    connect(m_ctx.get(), &AppContext::deviceButtonRequest,      this, &MainWindow::onDeviceButtonRequest);
    connect(m_ctx.get(), &AppContext::deviceButtonPressed,      this, &MainWindow::onDeviceButtonPressed);
    connect(m_ctx.get(), &AppContext::initiateTransaction,      this, &MainWindow::onInitiateTransaction);
    connect(m_ctx.get(), &AppContext::endTransaction,           this, &MainWindow::onEndTransaction);
    connect(m_ctx.get(), &AppContext::keysCorrupted,            this, &MainWindow::onKeysCorrupted);
    connect(m_ctx.get(), &AppContext::selectedInputsChanged,    this, &MainWindow::onSelectedInputsChanged);

    // Nodes
    connect(m_ctx->nodes, &Nodes::nodeExhausted,   this, &MainWindow::showNodeExhaustedMessage);
    connect(m_ctx->nodes, &Nodes::WSNodeExhausted, this, &MainWindow::showWSNodeExhaustedMessage);

    // Wallet
    connect(m_ctx->wallet, &Wallet::connectionStatusChanged, this, &MainWindow::onConnectionStatusChanged);
    connect(m_ctx->wallet, &Wallet::currentSubaddressAccountChanged, this, &MainWindow::updateTitle);
    connect(m_ctx->wallet, &Wallet::walletPassphraseNeeded, this, &MainWindow::onWalletPassphraseNeeded);
}

void MainWindow::menuToggleTabVisible(const QString &key){
    const auto toggleTab = m_tabShowHideMapper[key];
    bool show = config()->get(toggleTab->configKey).toBool();
    show = !show;
    config()->set(toggleTab->configKey, show);
    ui->tabWidget->setTabVisible(ui->tabWidget->indexOf(toggleTab->tab), show);
    toggleTab->menuAction->setText((show ? QString("Hide ") : QString("Show ")) + toggleTab->name);
}

void MainWindow::menuClearHistoryClicked() {
    config()->remove(Config::recentlyOpenedWallets);
    this->updateRecentlyOpenedMenu();
}

QString MainWindow::walletName() {
    return QFileInfo(m_ctx->wallet->cachePath()).fileName();
}

QString MainWindow::walletCachePath() {
    return m_ctx->wallet->cachePath();
}

QString MainWindow::walletKeysPath() {
    return m_ctx->wallet->keysPath();
}

void MainWindow::displayWalletErrorMsg(const QString &err) {
    QString errMsg = err;
    if (err.contains("No device found")) {
        errMsg += "\n\nThis wallet is backed by a hardware device. Make sure the Monero app is opened on the device.\n"
                  "You may need to restart Feather before the device can get detected.";
    }
    if (errMsg.contains("Unable to open device")) {
        errMsg += "\n\nThe device might be in use by a different application.";
    }

    if (errMsg.contains("SW_CLIENT_NOT_SUPPORTED")) {
        errMsg += "\n\nIncompatible version: you may need to upgrade the Monero app on the Ledger device to the latest version.";
    }
    else if (errMsg.contains("Wrong Device Status")) {
        errMsg += "\n\nThe device may need to be unlocked.";
    }
    else if (errMsg.contains("Wrong Channel")) {
        errMsg += "\n\nRestart the hardware device and try again.";
    }

    QMessageBox::warning(this, "Wallet error", errMsg);
}

void MainWindow::onWalletOpened() {
    qDebug() << Q_FUNC_INFO;
    m_splashDialog->hide();

    m_ctx->wallet->setRingDatabase(Utils::ringDatabasePath());

    m_ctx->updateBalance();
    if (m_ctx->wallet->isHwBacked()) {
        m_statusBtnHwDevice->show();
    }

    this->bringToFront();
    this->setEnabled(true);

    // receive page
    m_ctx->wallet->subaddress()->refresh(m_ctx->wallet->currentSubaddressAccount());
    if (m_ctx->wallet->subaddress()->count() == 1) {
        for (int i = 0; i < 10; i++) {
            m_ctx->wallet->subaddress()->addRow(m_ctx->wallet->currentSubaddressAccount(), "");
        }
    }
    m_ctx->wallet->subaddressModel()->setCurrentSubaddressAcount(m_ctx->wallet->currentSubaddressAccount());

    // history page
    m_ctx->wallet->history()->refresh(m_ctx->wallet->currentSubaddressAccount());

    // coins page
    m_ctx->wallet->coins()->refresh(m_ctx->wallet->currentSubaddressAccount());
    m_coinsWidget->setModel(m_ctx->wallet->coinsModel(), m_ctx->wallet->coins());
    m_ctx->wallet->coinsModel()->setCurrentSubaddressAccount(m_ctx->wallet->currentSubaddressAccount());

    // Coin labeling uses set_tx_note, so we need to refresh history too
    connect(m_ctx->wallet->coins(), &Coins::descriptionChanged, [this] {
        m_ctx->wallet->history()->refresh(m_ctx->wallet->currentSubaddressAccount());
    });
    // Vice versa
    connect(m_ctx->wallet->history(), &TransactionHistory::txNoteChanged, [this] {
        m_ctx->wallet->coins()->refresh(m_ctx->wallet->currentSubaddressAccount());
    });

    this->updatePasswordIcon();
    this->updateTitle();
    m_ctx->nodes->connectToNode();
    m_updateBytes.start(250);

    this->addToRecentlyOpened(m_ctx->wallet->cachePath());
}

void MainWindow::onBalanceUpdated(quint64 balance, quint64 spendable) {
    bool hide = config()->get(Config::hideBalance).toBool();
    int displaySetting = config()->get(Config::balanceDisplay).toInt();
    int decimals = config()->get(Config::amountPrecision).toInt();

    QString balance_str = "Balance: ";
    if (hide) {
        balance_str += "HIDDEN";
    }
    else if (displaySetting == Config::totalBalance) {
        balance_str += QString("%1 XMR").arg(WalletManager::displayAmount(balance, false, decimals));
    }
    else if (displaySetting == Config::spendable || displaySetting == Config::spendablePlusUnconfirmed) {
        balance_str += QString("%1 XMR").arg(WalletManager::displayAmount(spendable, false, decimals));

        if (displaySetting == Config::spendablePlusUnconfirmed && balance > spendable) {
            balance_str += QString(" (+%1 XMR unconfirmed)").arg(WalletManager::displayAmount(balance - spendable, false, decimals));
        }
    }

    m_statusLabelBalance->setToolTip("Click for details");
    m_statusLabelBalance->setText(balance_str);
    m_balanceTickerWidget->setHidden(hide);
}

void MainWindow::setStatusText(const QString &text, bool override, int timeout) {
    if (override) {
        m_statusOverrideActive = true;
        m_statusLabelStatus->setText(text);
        QTimer::singleShot(timeout, [this]{
            m_statusOverrideActive = false;
            this->setStatusText(m_statusText);
        });
        return;
    }

    m_statusText = text;

    if (!m_statusOverrideActive && !m_constructingTransaction) {
        m_statusLabelStatus->setText(text);
    }
}

void MainWindow::tryStoreWallet() {
    if (m_ctx->wallet->connectionStatus() == Wallet::ConnectionStatus::ConnectionStatus_Synchronizing) {
        QMessageBox::warning(this, "Save wallet", "Unable to save wallet during synchronization.\n\n"
                                                  "Wait until synchronization is finished and try again.");
        return;
    }

    m_ctx->wallet->store();
}

void MainWindow::onWebsocketStatusChanged(bool enabled) {
    ui->actionShow_Home->setVisible(enabled);
    ui->actionShow_calc->setVisible(enabled);
    ui->actionShow_Exchange->setVisible(enabled);

    ui->tabWidget->setTabVisible(Tabs::HOME, enabled && config()->get(Config::showTabHome).toBool());
    ui->tabWidget->setTabVisible(Tabs::CALC, enabled && config()->get(Config::showTabCalc).toBool());
    ui->tabWidget->setTabVisible(Tabs::EXCHANGES, enabled && config()->get(Config::showTabExchange).toBool());

    m_historyWidget->setWebsocketEnabled(enabled);

#ifdef HAS_XMRIG
    m_xmrig->setDownloadsTabEnabled(enabled);
#endif
}

void MainWindow::onSynchronized() {
    this->updateNetStats();
    this->setStatusText("Synchronized");
}

void MainWindow::onBlockchainSync(int height, int target) {
    QString blocks = (target >= height) ? QString::number(target - height) : "?";
    QString heightText = QString("Blockchain sync: %1 blocks remaining").arg(blocks);
    this->setStatusText(heightText);
}

void MainWindow::onRefreshSync(int height, int target) {
    QString blocks = (target >= height) ? QString::number(target - height) : "?";
    QString heightText = QString("Wallet sync: %1 blocks remaining").arg(blocks);
    this->setStatusText(heightText);
}

void MainWindow::onConnectionStatusChanged(int status)
{
    qDebug() << "Wallet connection status changed " << Utils::QtEnumToString(static_cast<Wallet::ConnectionStatus>(status));

    // Update connection info in status bar.

    QIcon icon;
    switch(status){
        case Wallet::ConnectionStatus_Disconnected:
            icon = icons()->icon("status_disconnected.svg");
            this->setStatusText("Disconnected");
            break;
        case Wallet::ConnectionStatus_Connecting:
            icon = icons()->icon("status_lagging.svg");
            this->setStatusText("Connecting to node");
            break;
        case Wallet::ConnectionStatus_WrongVersion:
            icon = icons()->icon("status_disconnected.svg");
            this->setStatusText("Incompatible node");
            break;
        case Wallet::ConnectionStatus_Synchronizing:
            icon = icons()->icon("status_waiting.svg");
            break;
        case Wallet::ConnectionStatus_Synchronized:
            icon = icons()->icon("status_connected.svg");
            break;
        default:
            icon = icons()->icon("status_disconnected.svg");
            break;
    }

    m_statusBtnConnectionStatusIndicator->setIcon(icon);
}

void MainWindow::onCreateTransactionSuccess(PendingTransaction *tx, const QVector<QString> &address) {
    QString err{"Can't create transaction: "};
    if (tx->status() != PendingTransaction::Status_Ok) {
        QString tx_err = tx->errorString();
        qCritical() << tx_err;

        if (m_ctx->wallet->connectionStatus() == Wallet::ConnectionStatus_WrongVersion)
            err = QString("%1 Wrong node version: %2").arg(err, tx_err);
        else
            err = QString("%1 %2").arg(err, tx_err);

        if (tx_err.contains("Node response did not include the requested real output")) {
            QString currentNode = m_ctx->nodes->connection().toAddress();

            err += QString("\nYou are currently connected to: %1\n\n"
                           "This node may be acting maliciously. You are strongly recommended to disconnect from this node."
                           "Please report this incident to dev@featherwallet.org, #feather on OFTC or /r/FeatherWallet.").arg(currentNode);
        }

        qDebug() << Q_FUNC_INFO << err;
        this->displayWalletErrorMsg(err);
        m_ctx->wallet->disposeTransaction(tx);
        return;
    }
    else if (tx->txCount() == 0) {
        err = QString("%1 %2").arg(err, "No unmixable outputs to sweep.");
        qDebug() << Q_FUNC_INFO << err;
        this->displayWalletErrorMsg(err);
        m_ctx->wallet->disposeTransaction(tx);
        return;
    }
    else if (tx->txCount() > 1) {
        err = QString("%1 %2").arg(err, "Split transactions are not supported. Try sending a smaller amount.");
        qDebug() << Q_FUNC_INFO << err;
        this->displayWalletErrorMsg(err);
        m_ctx->wallet->disposeTransaction(tx);
        return;
    }

    // This is a weak check to see if we send to all specified destination addresses
    // This is here to catch rare memory corruption errors during transaction construction
    // TODO: also check that amounts match
    tx->refresh();
    QSet<QString> outputAddresses;
    for (const auto &output : tx->transaction(0)->outputs()) {
        outputAddresses.insert(WalletManager::baseAddressFromIntegratedAddress(output->address(), constants::networkType));
    }
    QSet<QString> destAddresses;
    for (const auto &addr : address) {
        // TODO: Monero core bug, integrated address is not added to dests for transactions spending ALL
        destAddresses.insert(WalletManager::baseAddressFromIntegratedAddress(addr, constants::networkType));
    }
    if (!outputAddresses.contains(destAddresses)) {
        err = QString("%1 %2").arg(err, "Constructed transaction doesn't appear to send to (all) specified destination address(es). Try creating the transaction again.");
        qDebug() << Q_FUNC_INFO << err;
        this->displayWalletErrorMsg(err);
        m_ctx->wallet->disposeTransaction(tx);
        return;
    }

    m_ctx->addCacheTransaction(tx->txid()[0], tx->signedTxToHex(0));

    // Show advanced dialog on multi-destination transactions
    if (address.size() > 1 || m_ctx->wallet->viewOnly()) {
        TxConfAdvDialog dialog_adv{m_ctx, m_ctx->tmpTxDescription, this};
        dialog_adv.setTransaction(tx, !m_ctx->wallet->viewOnly());
        dialog_adv.exec();
        return;
    }

    TxConfDialog dialog{m_ctx, tx, address[0], m_ctx->tmpTxDescription, this};
    switch (dialog.exec()) {
        case QDialog::Rejected:
        {
            if (!dialog.showAdvanced)
                m_ctx->onCancelTransaction(tx, address);
            break;
        }
        case QDialog::Accepted:
            m_ctx->commitTransaction(tx, m_ctx->tmpTxDescription);
            break;
    }

    if (dialog.showAdvanced) {
        TxConfAdvDialog dialog_adv{m_ctx, m_ctx->tmpTxDescription, this};
        dialog_adv.setTransaction(tx);
        dialog_adv.exec();
    }
}

void MainWindow::onTransactionCommitted(bool status, PendingTransaction *tx, const QStringList& txid) {
    if (status) { // success
        QMessageBox msgBox{this};
        QPushButton *showDetailsButton = msgBox.addButton("Show details", QMessageBox::ActionRole);
        msgBox.addButton(QMessageBox::Ok);
        QString body = QString("Successfully sent %1 transaction(s).").arg(txid.count());
        msgBox.setText(body);
        msgBox.setWindowTitle("Transaction sent");
        msgBox.setIcon(QMessageBox::Icon::Information);
        msgBox.exec();
        if (msgBox.clickedButton() == showDetailsButton) {
            this->showHistoryTab();
            TransactionInfo *txInfo = m_ctx->wallet->history()->transaction(txid.first());
            auto *dialog = new TxInfoDialog(m_ctx, txInfo, this);
            connect(dialog, &TxInfoDialog::resendTranscation, this, &MainWindow::onResendTransaction);
            dialog->show();
            dialog->setAttribute(Qt::WA_DeleteOnClose);
        }

        m_sendWidget->clearFields();
    } else {
        auto err = tx->errorString();
        QString body = QString("Error committing transaction: %1").arg(err);
        QMessageBox::warning(this, "Transaction failed", body);
    }
}

void MainWindow::onCreateTransactionError(const QString &message) {
    auto msg = QString("Error while creating transaction: %1").arg(message);

    if (msg.contains("failed to get random outs")) {
        msg += "\n\nYour transaction has too many inputs. Try sending a lower amount.";
    }

    QMessageBox::warning(this, "Transaction failed", msg);
}

void MainWindow::showWalletInfoDialog() {
    WalletInfoDialog dialog{m_ctx, this};
    dialog.exec();
}

void MainWindow::showSeedDialog() {
    if (m_ctx->wallet->isHwBacked()) {
        QMessageBox::information(this, "Information", "Seed unavailable: Wallet keys are stored on hardware device.");
        return;
    }

    if (m_ctx->wallet->viewOnly()) {
        QMessageBox::information(this, "Information", "Wallet is view-only and has no seed.\n\nTo obtain wallet keys go to Wallet -> View-Only");
        return;
    }

    if (!m_ctx->wallet->isDeterministic()) {
        QMessageBox::information(this, "Information", "Wallet is non-deterministic and has no seed.\n\nTo obtain wallet keys go to Wallet -> Keys");
        return;
    }

    if (!this->verifyPassword()) {
        return;
    }

    SeedDialog dialog{m_ctx, this};
    dialog.exec();
}

void MainWindow::showPasswordDialog() {
    PasswordChangeDialog dialog{this, m_ctx->wallet};
    dialog.exec();
    this->updatePasswordIcon();
}

void MainWindow::updatePasswordIcon() {
    QIcon icon = m_ctx->wallet->getPassword().isEmpty() ? icons()->icon("unlock.svg") : icons()->icon("lock.svg");
    m_statusBtnPassword->setIcon(icon);
}

void MainWindow::showKeysDialog() {
    if (!this->verifyPassword()) {
        return;
    }

    KeysDialog dialog{m_ctx, this};
    dialog.exec();
}

void MainWindow::showViewOnlyDialog() {
    ViewOnlyDialog dialog{m_ctx, this};
    dialog.exec();
}

void MainWindow::menuTorClicked() {
    auto *dialog = new TorInfoDialog(m_ctx, this);
    connect(dialog, &TorInfoDialog::torSettingsChanged, m_windowManager, &WindowManager::onTorSettingsChanged);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::menuHwDeviceClicked() {
    QMessageBox::information(this, "Hardware Device", QString("This wallet is backed by a %1 hardware device.").arg(this->getHardwareDevice()));
}

void MainWindow::menuOpenClicked() {
    m_windowManager->wizardOpenWallet();
}

void MainWindow::menuNewRestoreClicked() {
    m_windowManager->showWizard(WalletWizard::Page_Menu);
}

void MainWindow::menuQuitClicked() {
    this->close();
}

void MainWindow::menuWalletCloseClicked() {
    m_windowManager->showWizard(WalletWizard::Page_Menu);
    this->close();
}

void MainWindow::menuAboutClicked() {
    AboutDialog dialog{this};
    dialog.exec();
}

void MainWindow::menuSettingsClicked() {
    Settings settings{m_ctx, this};
    for (const auto &widget: m_tickerWidgets) {
        connect(&settings, &Settings::preferredFiatCurrencyChanged, widget, &TickerWidgetBase::updateDisplay);
    }
    connect(&settings, &Settings::preferredFiatCurrencyChanged, m_balanceTickerWidget, &BalanceTickerWidget::updateDisplay);
    connect(&settings, &Settings::preferredFiatCurrencyChanged, m_sendWidget, QOverload<>::of(&SendWidget::onPreferredFiatCurrencyChanged));
    connect(&settings, &Settings::skinChanged, this, &MainWindow::skinChanged);
    connect(&settings, &Settings::websocketStatusChanged, m_windowManager, &WindowManager::onWebsocketStatusChanged);
    settings.exec();
}

void MainWindow::menuSignVerifyClicked() {
    SignVerifyDialog dialog{m_ctx->wallet, this};
    dialog.exec();
}

void MainWindow::menuVerifyTxProof() {
    VerifyProofDialog dialog{m_ctx->wallet, this};
    dialog.exec();
}

void MainWindow::onShowSettingsPage(int page) {
    config()->set(Config::lastSettingsPage, page);
    this->menuSettingsClicked();
}

void MainWindow::skinChanged(const QString &skinName) {
    m_windowManager->changeSkin(skinName);
    ColorScheme::updateFromWidget(this);
    this->updateWidgetIcons();

#if defined(Q_OS_MACOS)
    this->patchStylesheetMac();
#endif
}

void MainWindow::updateWidgetIcons() {
    m_sendWidget->skinChanged();
#ifdef HAS_LOCALMONERO
    m_localMoneroWidget->skinChanged();
#endif
    ui->conversionWidget->skinChanged();
    ui->revuoWidget->skinChanged();

    m_statusBtnHwDevice->setIcon(this->hardwareDevicePairedIcon());
}

QIcon MainWindow::hardwareDevicePairedIcon() {
    QString filename;
    if (m_ctx->wallet->isLedger())
        filename = "ledger.png";
    else if (m_ctx->wallet->isTrezor())
        filename = ColorScheme::darkScheme ? "trezor_white.png" : "trezor.png";
    return icons()->icon(filename);
}

QIcon MainWindow::hardwareDeviceUnpairedIcon() {
    QString filename;
    if (m_ctx->wallet->isLedger())
        filename = "ledger_unpaired.png";
    else if (m_ctx->wallet->isTrezor())
        filename = ColorScheme::darkScheme ? "trezor_unpaired_white.png" : "trezor_unpaired.png";
    return icons()->icon(filename);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    qDebug() << Q_FUNC_INFO;

    if (!this->cleanedUp) {
        this->cleanedUp = true;

        config()->set(Config::homeWidget, ui->tabHomeWidget->currentIndex());

        m_historyWidget->resetModel();

        m_updateBytes.stop();
        m_txTimer.stop();
        m_ctx->stopTimers();

        // Wallet signal may fire after AppContext is gone, causing segv
        m_ctx->wallet->disconnect();

        this->saveGeo();
        m_windowManager->closeWindow(this);
    }

    event->accept();
}

void MainWindow::donateButtonClicked() {
    m_sendWidget->fill(constants::donationAddress, "Donation to the Feather development team");
    ui->tabWidget->setCurrentIndex(Tabs::SEND);
}

void MainWindow::showHistoryTab() {
    this->raise();
    ui->tabWidget->setCurrentIndex(Tabs::HISTORY);
}

void MainWindow::showSendTab() {
    this->raise();
    ui->tabWidget->setCurrentIndex(Tabs::SEND);
}

void MainWindow::fillSendTab(const QString &address, const QString &description) {
    m_sendWidget->fill(address, description);
    ui->tabWidget->setCurrentIndex(Tabs::SEND);
}

void MainWindow::showCalcWindow() {
    m_windowCalc->show();
}

void MainWindow::payToMany() {
    ui->tabWidget->setCurrentIndex(Tabs::SEND);
    m_sendWidget->payToMany();
    QMessageBox::information(this, "Pay to many", "Enter a list of outputs in the 'Pay to' field.\n"
                                                  "One output per line.\n"
                                                  "Format: address, amount\n"
                                                  "A maximum of 16 addresses may be specified.");
}

void MainWindow::showSendScreen(const CCSEntry &entry) { // TODO: rename this function
    m_sendWidget->fill(entry.address, QString("CCS: %1").arg(entry.title));
    ui->tabWidget->setCurrentIndex(Tabs::SEND);
}

void MainWindow::onViewOnBlockExplorer(const QString &txid) {
    QString blockExplorerLink = Utils::blockExplorerLink(config()->get(Config::blockExplorer).toString(), constants::networkType, txid);
    Utils::externalLinkWarning(this, blockExplorerLink);
}

void MainWindow::onResendTransaction(const QString &txid) {
    QString txHex = m_ctx->getCacheTransaction(txid);
    if (txHex.isEmpty()) {
        QMessageBox::warning(this, "Unable to resend transaction", "Transaction was not found in transaction cache. Unable to resend.");
        return;
    }

    // Connect to a different node so chances of successful relay are higher
    m_ctx->nodes->autoConnect(true);

    TxBroadcastDialog dialog{this, m_ctx, txHex};
    dialog.exec();
}

void MainWindow::importContacts() {
    const QString targetFile = QFileDialog::getOpenFileName(this, "Import CSV file", QDir::homePath(), "CSV Files (*.csv)");
    if(targetFile.isEmpty()) return;

    auto *model = m_ctx->wallet->addressBookModel();
    QMapIterator<QString, QString> i(model->readCSV(targetFile));
    int inserts = 0;
    while (i.hasNext()) {
        i.next();
        bool addressValid = WalletManager::addressValid(i.value(), m_ctx->wallet->nettype());
        if(addressValid) {
            m_ctx->wallet->addressBook()->addRow(i.value(), "", i.key());
            inserts++;
        }
    }

    QMessageBox::information(this, "Contacts imported", QString("Total contacts imported: %1").arg(inserts));
}

void MainWindow::saveGeo() {
    config()->set(Config::geometry, QString(saveGeometry().toBase64()));
    config()->set(Config::windowState, QString(saveState().toBase64()));
}

void MainWindow::restoreGeo() {
    bool geo = this->restoreGeometry(QByteArray::fromBase64(config()->get(Config::geometry).toByteArray()));
    bool windowState = this->restoreState(QByteArray::fromBase64(config()->get(Config::windowState).toByteArray()));
    qDebug() << "Restored window state: " << geo << " " << windowState;
}

void MainWindow::showDebugInfo() {
    DebugInfoDialog dialog{m_ctx, this};
    dialog.exec();
}

void MainWindow::showWalletCacheDebugDialog() {
    if (!this->verifyPassword()) {
        return;
    }

    WalletCacheDebugDialog dialog{m_ctx, this};
    dialog.exec();
}

void MainWindow::showAccountSwitcherDialog() {
    AccountSwitcherDialog dialog{m_ctx, this};
    dialog.exec();
}

void MainWindow::showAddressChecker() {
    QString address = QInputDialog::getText(this, "Address Checker", "Address:                                      ");
    if (address.isEmpty()) {
        return;
    }

    if (!WalletManager::addressValid(address, constants::networkType)) {
        QMessageBox::warning(this, "Address Checker", "Invalid address.");
        return;
    }

    SubaddressIndex index = m_ctx->wallet->subaddressIndex(address);
    if (!index.isValid()) {
        // TODO: probably mention lookahead here
        QMessageBox::warning(this, "Address Checker", "This address does not belong to this wallet.");
        return;
    } else {
        QMessageBox::information(this, "Address Checker", QString("This address belongs to Account #%1").arg(index.major));
    }
}

void MainWindow::showNodeExhaustedMessage() {
    // Spawning dialogs inside a lambda can cause system freezes on linux so we have to do it this way ¯\_(ツ)_/¯

    auto msg = "Feather is in 'custom node connection mode' but could not "
               "find an eligible node to connect to. Please go to Settings->Node "
               "and enter a node manually.";
    QMessageBox::warning(this, "Could not connect to a node", msg);
}

void MainWindow::showWSNodeExhaustedMessage() {
    auto msg = "Feather is in 'automatic node connection mode' but the "
               "websocket server returned no available nodes. Please go to Settings->Node "
               "and enter a node manually.";
    QMessageBox::warning(this, "Could not connect to a node", msg);
}

void MainWindow::exportKeyImages() {
    QString fn = QFileDialog::getSaveFileName(this, "Save key images to file", QString("%1/%2_%3").arg(QDir::homePath(), this->walletName(), QString::number(QDateTime::currentSecsSinceEpoch())), "Key Images (*_keyImages)");
    if (fn.isEmpty()) return;
    if (!fn.endsWith("_keyImages")) fn += "_keyImages";
    bool r = m_ctx->wallet->exportKeyImages(fn, true);
    if (!r) {
        QMessageBox::warning(this, "Key image export", QString("Failed to export key images.\nReason: %1").arg(m_ctx->wallet->errorString()));
    } else {
        QMessageBox::information(this, "Key image export", "Successfully exported key images.");
    }
}

void MainWindow::importKeyImages() {
    QString fn = QFileDialog::getOpenFileName(this, "Import key image file", QDir::homePath(), "Key Images (*_keyImages)");
    if (fn.isEmpty()) return;
    bool r = m_ctx->wallet->importKeyImages(fn);
    if (!r) {
        QMessageBox::warning(this, "Key image import", QString("Failed to import key images.\n\n%1").arg(m_ctx->wallet->errorString()));
    } else {
        QMessageBox::information(this, "Key image import", "Successfully imported key images");
        m_ctx->refreshModels();
    }
}

void MainWindow::exportOutputs() {
    QString fn = QFileDialog::getSaveFileName(this, "Save outputs to file", QString("%1/%2_%3").arg(QDir::homePath(), this->walletName(), QString::number(QDateTime::currentSecsSinceEpoch())), "Outputs (*_outputs)");
    if (fn.isEmpty()) return;
    if (!fn.endsWith("_outputs")) fn += "_outputs";
    bool r = m_ctx->wallet->exportOutputs(fn, true);
    if (!r) {
        QMessageBox::warning(this, "Outputs export", QString("Failed to export outputs.\nReason: %1").arg(m_ctx->wallet->errorString()));
    } else {
        QMessageBox::information(this, "Outputs export", "Successfully exported outputs.");
    }
}

void MainWindow::importOutputs() {
    QString fn = QFileDialog::getOpenFileName(this, "Import outputs file", QDir::homePath(), "Outputs (*_outputs)");
    if (fn.isEmpty()) return;
    bool r = m_ctx->wallet->importOutputs(fn);
    if (!r) {
        QMessageBox::warning(this, "Outputs import", QString("Failed to import outputs.\n\n%1").arg(m_ctx->wallet->errorString()));
    } else {
        QMessageBox::information(this, "Outputs import", "Successfully imported outputs");
        m_ctx->refreshModels();
    }
}

void MainWindow::loadUnsignedTx() {
    QString fn = QFileDialog::getOpenFileName(this, "Select transaction to load", QDir::homePath(), "Transaction (*unsigned_monero_tx)");
    if (fn.isEmpty()) return;
    UnsignedTransaction *tx = m_ctx->wallet->loadTxFile(fn);
    auto err = m_ctx->wallet->errorString();
    if (!err.isEmpty()) {
        QMessageBox::warning(this, "Load transaction from file", QString("Failed to load transaction.\n\n%1").arg(err));
        return;
    }

    this->createUnsignedTxDialog(tx);
}

void MainWindow::loadUnsignedTxFromClipboard() {
    QString unsigned_tx = Utils::copyFromClipboard();
    if (unsigned_tx.isEmpty()) {
        QMessageBox::warning(this, "Load unsigned transaction from clipboard", "Clipboard is empty");
        return;
    }
    UnsignedTransaction *tx = m_ctx->wallet->loadTxFromBase64Str(unsigned_tx);
    auto err = m_ctx->wallet->errorString();
    if (!err.isEmpty()) {
        QMessageBox::warning(this, "Load unsigned transaction from clipboard", QString("Failed to load transaction.\n\n%1").arg(err));
        return;
    }

    this->createUnsignedTxDialog(tx);
}

void MainWindow::loadSignedTx() {
    QString fn = QFileDialog::getOpenFileName(this, "Select transaction to load", QDir::homePath(), "Transaction (*signed_monero_tx)");
    if (fn.isEmpty()) return;
    PendingTransaction *tx = m_ctx->wallet->loadSignedTxFile(fn);
    auto err = m_ctx->wallet->errorString();
    if (!err.isEmpty()) {
        QMessageBox::warning(this, "Load signed transaction from file", err);
        return;
    }

    TxConfAdvDialog dialog{m_ctx, "", this};
    dialog.setTransaction(tx);
    dialog.exec();
}

void MainWindow::loadSignedTxFromText() {
    TxBroadcastDialog dialog{this, m_ctx};
    dialog.exec();
}

void MainWindow::createUnsignedTxDialog(UnsignedTransaction *tx) {
    TxConfAdvDialog dialog{m_ctx, "", this};
    dialog.setUnsignedTransaction(tx);
    dialog.exec();
}

void MainWindow::importTransaction() {
    if (config()->get(Config::torPrivacyLevel).toInt() == Config::allTorExceptNode) {
        // TODO: don't show if connected to local node

        auto result = QMessageBox::warning(this, "Warning", "Using this feature may allow a remote node to associate the transaction with your IP address.\n"
                                                            "\n"
                                                            "Connect to a trusted node or run Feather over Tor if network level metadata leakage is included in your threat model.",
                                           QMessageBox::Ok | QMessageBox::Cancel);
        if (result != QMessageBox::Ok) {
            return;
        }
    }

    TxImportDialog dialog(this, m_ctx);
    dialog.exec();
}

void MainWindow::onDeviceError(const QString &error) {
    if (m_showDeviceError) {
        return;
    }

    m_statusBtnHwDevice->setIcon(this->hardwareDeviceUnpairedIcon());
    while (true) {
        m_showDeviceError = true;
        auto result = QMessageBox::question(this, "Hardware device", "Lost connection to hardware device. Attempt to reconnect?");
        if (result == QMessageBox::Yes) {
            bool r = m_ctx->wallet->reconnectDevice();
            if (r) {
                break;
            }
        }
        if (result == QMessageBox::No) {
            this->menuWalletCloseClicked();
            return;
        }
    }
    m_statusBtnHwDevice->setIcon(this->hardwareDevicePairedIcon());
    m_ctx->wallet->startRefresh();
    m_showDeviceError = false;
}

void MainWindow::onDeviceButtonRequest(quint64 code) {
    qDebug() << "DeviceButtonRequest, code: " << code;

    if (m_ctx->wallet->isTrezor()) {
        switch (code) {
            case 1:
            {
                m_splashDialog->setMessage("Action required on device: Enter your PIN to continue");
                m_splashDialog->setIcon(QPixmap(":/assets/images/key.png"));
                m_splashDialog->show();
                m_splashDialog->setEnabled(true);
                break;
            }
            case 8:
            default:
            {
                // Annoyingly, this code is used for a variety of actions, including:
                // Confirm refresh: Do you really want to start refresh?
                // Confirm export: Do you really want to export tx_key?

                if (m_constructingTransaction) { // This code is also used when signing a tx, we handle this elsewhere
                    break;
                }

                m_splashDialog->setMessage("Confirm action on device to proceed");
                m_splashDialog->setIcon(QPixmap(":/assets/images/confirmed.png"));
                m_splashDialog->show();
                m_splashDialog->setEnabled(true);
                break;
            }
        }
    }
}

void MainWindow::onDeviceButtonPressed() {
    if (m_constructingTransaction) {
        return;
    }

    m_splashDialog->hide();
}

void MainWindow::onWalletPassphraseNeeded(bool on_device) {
    auto button = QMessageBox::question(nullptr, "Wallet Passphrase Needed", "Enter passphrase on hardware wallet?\n\n"
                                                                             "It is recommended to enter passphrase on "
                                                                             "the hardware wallet for better security.",
                                        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (button == QMessageBox::Yes) {
        m_ctx->wallet->onPassphraseEntered("", true, false);
        return;
    }

    bool ok;
    QString passphrase = QInputDialog::getText(nullptr, "Wallet Passphrase Needed", "Enter passphrase:", QLineEdit::EchoMode::Password, "", &ok);
    if (ok) {
        m_ctx->wallet->onPassphraseEntered(passphrase, false, false);
    } else {
        m_ctx->wallet->onPassphraseEntered(passphrase, false, true);
    }
}

void MainWindow::updateNetStats() {
    if (!m_ctx->wallet || m_ctx->wallet->connectionStatus() == Wallet::ConnectionStatus_Disconnected
                       || m_ctx->wallet->connectionStatus() == Wallet::ConnectionStatus_Synchronized)
    {
        m_statusLabelNetStats->hide();
        return;
    }

    m_statusLabelNetStats->show();
    m_statusLabelNetStats->setText(QString("(D: %1)").arg(Utils::formatBytes(m_ctx->wallet->getBytesReceived())));
}

void MainWindow::rescanSpent() {
    if (!m_ctx->wallet->rescanSpent()) {
        QMessageBox::warning(this, "Rescan spent", m_ctx->wallet->errorString());
    } else {
        QMessageBox::information(this, "Rescan spent", "Successfully rescanned spent outputs.");
    }
}

void MainWindow::showBalanceDialog() {
    BalanceDialog dialog{this, m_ctx->wallet};
    dialog.exec();
}

QString MainWindow::statusDots() {
    m_statusDots++;
    m_statusDots = m_statusDots % 4;
    return QString(".").repeated(m_statusDots);
}

void MainWindow::showOrHide() {
    if (this->isHidden())
        this->bringToFront();
    else
        this->hide();
}

void MainWindow::bringToFront() {
    ensurePolished();
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    show();
    raise();
    activateWindow();
}

void MainWindow::onTorConnectionStateChanged(bool connected) {
    if (connected)
        m_statusBtnTor->setIcon(icons()->icon("tor_logo.png"));
    else
        m_statusBtnTor->setIcon(icons()->icon("tor_logo_disabled.png"));
}

void MainWindow::onCheckUpdatesComplete(const QString &version, const QString &binaryFilename,
                                        const QString &hash, const QString &signer) {
    QString versionDisplay{version};
    versionDisplay.replace("beta", "Beta");
    QString updateText = QString("Update to Feather %1 is available").arg(versionDisplay);
    m_statusUpdateAvailable->setText(updateText);
    m_statusUpdateAvailable->setToolTip("Click to Download update.");
    m_statusUpdateAvailable->show();

    m_statusUpdateAvailable->disconnect();
    connect(m_statusUpdateAvailable, &StatusBarButton::clicked, [this, version, binaryFilename, hash, signer] {
        this->onShowUpdateCheck(version, binaryFilename, hash, signer);
    });
}

void MainWindow::onShowUpdateCheck(const QString &version, const QString &binaryFilename,
                                   const QString &hash, const QString &signer) {
    QString platformTag = this->getPlatformTag();
    QString downloadUrl = QString("https://featherwallet.org/files/releases/%1/%2").arg(platformTag, binaryFilename);

    UpdateDialog updateDialog{this, version, downloadUrl, hash, signer, platformTag};
    connect(&updateDialog, &UpdateDialog::restartWallet, m_windowManager, &WindowManager::restartApplication);
    updateDialog.exec();
}

void MainWindow::onUpdatesAvailable(const QJsonObject &updates) {
    QString featherVersionStr{FEATHER_VERSION};

    auto featherVersion = SemanticVersion::fromString(featherVersionStr);

    QString platformTag = getPlatformTag();
    if (platformTag.isEmpty()) {
        qWarning() << "Unsupported platform, unable to fetch update";
        return;
    }

    QJsonObject platformData = updates["platform"].toObject()[platformTag].toObject();
    if (platformData.isEmpty()) {
        qWarning() << "Unable to find current platform in updates data";
        return;
    }

    QString newVersion = platformData["version"].toString();
    if (SemanticVersion::fromString(newVersion) <= featherVersion) {
        return;
    }

    // Hooray! New update available

    QString hashesUrl = QString("%1/files/releases/hashes-%2-plain.txt").arg(constants::websiteUrl, newVersion);

    UtilsNetworking network{getNetworkTor()};
    QNetworkReply *reply = network.get(hashesUrl);

    connect(reply, &QNetworkReply::finished, this, std::bind(&MainWindow::onSignedHashesReceived, this, reply, platformTag, newVersion));
}

void MainWindow::onSignedHashesReceived(QNetworkReply *reply, const QString &platformTag, const QString &version) {
    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "Unable to fetch signed hashes: " << reply->errorString();
        return;
    }

    QByteArray armoredSignedHashes = reply->readAll();
    reply->deleteLater();

    const QString binaryFilename = QString("feather-%1-%2.zip").arg(version, platformTag);
    QString signer;
    QByteArray signedHash = AsyncTask::runAndWaitForFuture([armoredSignedHashes, binaryFilename, &signer]{
        try {
            return Updater().verifyParseSignedHashes(armoredSignedHashes, binaryFilename, signer);
        }
        catch (const std::exception &e) {
            qWarning() << "Failed to fetch and verify signed hash: " << e.what();
            return QByteArray{};
        }
    });
    if (signedHash.isEmpty()) {
        return;
    }

    QString hash = signedHash.toHex();
    qInfo() << "Update found: " << binaryFilename << hash << "signed by:" << signer;
    this->onCheckUpdatesComplete(version, binaryFilename, hash, signer);
}

void MainWindow::onInitiateTransaction() {
    m_statusDots = 0;
    m_constructingTransaction = true;
    m_txTimer.start(1000);

    if (m_ctx->wallet->isHwBacked()) {
        QString message = "Constructing transaction: action may be required on device.";
        m_splashDialog->setMessage(message);
        m_splashDialog->setIcon(QPixmap(":/assets/images/unconfirmed.png"));
        m_splashDialog->show();
        m_splashDialog->setEnabled(true);
    }
}

void MainWindow::onEndTransaction() {
    // Todo: endTransaction can fail to fire when the node is switched during tx creation
    m_constructingTransaction = false;
    m_txTimer.stop();
    this->setStatusText(m_statusText);

    if (m_ctx->wallet->isHwBacked()) {
        m_splashDialog->hide();
    }
}

void MainWindow::onKeysCorrupted() {
    if (!m_criticalWarningShown) {
        m_criticalWarningShown = true;
        QMessageBox::warning(this, "Critical error", "WARNING!\n\nThe wallet keys are corrupted.\n\nTo prevent LOSS OF FUNDS do NOT continue to use this wallet file.\n\nRestore your wallet from seed.\n\nPlease report this incident to the Feather developers.\n\nWARNING!");
        m_sendWidget->disableSendButton();
    }
}

void MainWindow::onSelectedInputsChanged(const QStringList &selectedInputs) {
    int numInputs = selectedInputs.size();

    ui->frame_coinControl->setStyleSheet(ColorScheme::GREEN.asStylesheet(true));
    ui->frame_coinControl->setVisible(numInputs > 0);

    if (numInputs > 0) {
        quint64 totalAmount = 0;
        auto coins = m_ctx->wallet->coins()->coinsFromKeyImage(selectedInputs);
        for (const auto coin : coins) {
            totalAmount += coin->amount();
        }

        QString text = QString("Coin control active: %1 selected outputs, %2 XMR").arg(QString::number(numInputs), WalletManager::displayAmount(totalAmount));
        ui->label_coinControl->setText(text);
    }
}

void MainWindow::onExportHistoryCSV(bool checked) {
    if (m_ctx->wallet == nullptr)
        return;
    QString fn = QFileDialog::getSaveFileName(this, "Save CSV file", QDir::homePath(), "CSV (*.csv)");
    if (fn.isEmpty())
        return;
    if (!fn.endsWith(".csv"))
        fn += ".csv";
    m_ctx->wallet->history()->writeCSV(fn);
    QMessageBox::information(this, "CSV export", QString("Transaction history exported to %1").arg(fn));
}

void MainWindow::onExportContactsCSV(bool checked) {
    if (m_ctx->wallet == nullptr) return;
    auto *model = m_ctx->wallet->addressBookModel();
    if (model->rowCount() <= 0){
        QMessageBox::warning(this, "Error", "Addressbook empty");
        return;
    }

    const QString targetDir = QFileDialog::getExistingDirectory(this, "Select CSV output directory ", QDir::homePath(), QFileDialog::ShowDirsOnly);
    if(targetDir.isEmpty()) return;

    qint64 now = QDateTime::currentDateTime().currentMSecsSinceEpoch();
    QString fn = QString("%1/monero-contacts_%2.csv").arg(targetDir, QString::number(now / 1000));
    if(model->writeCSV(fn))
        QMessageBox::information(this, "Address book exported", QString("Address book exported to %1").arg(fn));
}

void MainWindow::onCreateDesktopEntry(bool checked) {
    auto msg = Utils::xdgDesktopEntryRegister() ? "Desktop entry created" : "Desktop entry not created due to an error.";
    QMessageBox::information(this, "Desktop entry", msg);
}

void MainWindow::onShowDocumentaton() {
    Utils::externalLinkWarning(this, "https://docs.featherwallet.org");
}

void MainWindow::onReportBug(bool checked) {
    Utils::externalLinkWarning(this, "https://docs.featherwallet.org/guides/report-an-issue");
}

QString MainWindow::getPlatformTag() {
#ifdef Q_OS_MACOS
    return "mac";
#endif
#ifdef Q_OS_WIN
#ifdef PLATFORM_INSTALLER
    return "win-installer";
#endif
    return "win";
#endif
#ifdef Q_OS_LINUX
    if (!qEnvironmentVariableIsEmpty("APPIMAGE")) {
        return "linux-appimage";
    }
    return "linux";
#endif
    return "";
}

QString MainWindow::getHardwareDevice() {
    if (!m_ctx->wallet->isHwBacked())
        return "";
    if (m_ctx->wallet->isTrezor())
        return "Trezor";
    if (m_ctx->wallet->isLedger())
        return "Ledger";
    return "Unknown";
}

void MainWindow::updateTitle() {
    QString title = QString("%1 (#%2)").arg(this->walletName(), QString::number(m_ctx->wallet->currentSubaddressAccount()));

    if (m_ctx->wallet->viewOnly())
        title += " [view-only]";
#ifdef HAS_XMRIG
    if (m_xmrig->isMining())
        title += " [mining]";
#endif

    title += " - Feather";

    this->setWindowTitle(title);
}

void MainWindow::donationNag() {
    if (m_ctx->networkType != NetworkType::Type::MAINNET)
        return;

    if (m_ctx->wallet->viewOnly())
        return;

    if (m_ctx->wallet->balanceAll() == 0)
        return;

    auto donationCounter = config()->get(Config::donateBeg).toInt();
    if (donationCounter == -1)
        return;

    donationCounter++;
    if (donationCounter % constants::donationBoundary == 0) {
        auto msg = "Feather is a 100% community-sponsored endeavor. Please consider supporting "
                   "the project financially. Get rid of this message by donating any amount.";
        int ret = QMessageBox::information(this, "Donate to Feather", msg, QMessageBox::Yes, QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            this->donateButtonClicked();
        }
    }
    config()->set(Config::donateBeg, donationCounter);
}

void MainWindow::addToRecentlyOpened(const QString &keysFile) {
    auto recent = config()->get(Config::recentlyOpenedWallets).toList();

    if (recent.contains(keysFile)) {
        recent.removeOne(keysFile);
    }
    recent.insert(0, keysFile);

    QList<QVariant> recent_;
    int count = 0;
    for (const auto &file : recent) {
        if (Utils::fileExists(file.toString())) {
            recent_.append(file);
            count++;
        }
        if (count >= 5) {
            break;
        }
    }

    config()->set(Config::recentlyOpenedWallets, recent_);

    this->updateRecentlyOpenedMenu();
}

void MainWindow::updateRecentlyOpenedMenu() {
    ui->menuRecently_open->clear();
    const QStringList recentWallets = config()->get(Config::recentlyOpenedWallets).toStringList();
    for (const auto &walletPath : recentWallets) {
        QFileInfo fileInfo{walletPath};
        ui->menuRecently_open->addAction(fileInfo.fileName(), m_windowManager, std::bind(&WindowManager::tryOpenWallet, m_windowManager, walletPath, ""));
    }
    ui->menuRecently_open->addSeparator();
    ui->menuRecently_open->addAction(m_clearRecentlyOpenAction);
}

bool MainWindow::verifyPassword(bool sensitive) {
    bool incorrectPassword = false;
    while (true) {
        PasswordDialog passwordDialog{this->walletName(), incorrectPassword, sensitive, this};
        int ret = passwordDialog.exec();
        if (ret == QDialog::Rejected) {
            return false;
        }
        if (passwordDialog.password != m_ctx->wallet->getPassword()) {
            incorrectPassword = true;
            continue;
        }
        break;
    }
    return true;
}

void MainWindow::patchStylesheetMac() {
    auto patch = Utils::fileOpenQRC(":assets/macStylesheet.patch");
    auto patch_text = Utils::barrayToString(patch);

    QString styleSheet = qApp->styleSheet() + patch_text;
    qApp->setStyleSheet(styleSheet);
}

void MainWindow::userActivity() {
    m_userLastActive = QDateTime::currentSecsSinceEpoch();
}

void MainWindow::checkUserActivity() {
    if (!config()->get(Config::inactivityLockEnabled).toBool()) {
        return;
    }

    if (m_constructingTransaction) {
        return;
    }

    if ((m_userLastActive + (config()->get(Config::inactivityLockTimeout).toInt()*60)) < QDateTime::currentSecsSinceEpoch()) {
        m_checkUserActivity.stop();
        qInfo() << "Locking wallet for inactivity";
        ui->tabWidget->hide();
        this->statusBar()->hide();
        this->menuBar()->hide();
        if (!this->verifyPassword(false)) {
            this->setEnabled(false);
            this->close();
            // This doesn't close the wallet immediately.
            // FIXME
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            do {
#endif
                QApplication::processEvents();
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            // Because running it a single time is apparently not enough.
            // TODO: Qt bug? Need proper fix for this.
            } while (QApplication::hasPendingEvents());
#endif
        } else {
            ui->tabWidget->show();
            this->statusBar()->show();
            this->menuBar()->show();
            m_checkUserActivity.start();
        }
    }
}

void MainWindow::toggleSearchbar(bool visible) {
    config()->set(Config::showSearchbar, visible);

    m_historyWidget->setSearchbarVisible(visible);
    m_receiveWidget->setSearchbarVisible(visible);
    m_contactsWidget->setSearchbarVisible(visible);
    m_coinsWidget->setSearchbarVisible(visible);

    int currentTab = ui->tabWidget->currentIndex();
    if (currentTab == Tabs::HISTORY)
        m_historyWidget->focusSearchbar();
    else if (currentTab == Tabs::SEND)
        m_contactsWidget->focusSearchbar();
    else if (currentTab == Tabs::RECEIVE)
        m_receiveWidget->focusSearchbar();
    else if (currentTab == Tabs::COINS)
        m_coinsWidget->focusSearchbar();
}

MainWindow::~MainWindow() = default;