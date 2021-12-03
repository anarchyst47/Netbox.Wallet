// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2019 The PIVX developers
// Copyright (c) 2018-2021 Netbox.Global
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/nbx-config.h"
#endif

#include "bitcoingui.h"

#include "clientmodel.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "intro.h"
#include "net.h"
#include "networkstyle.h"
#include "optionsmodel.h"
#include "rpcconsole.h"
#include "splashscreen.h"
#include "utilitydialog.h"
#include "winshutdownmonitor.h"

#ifdef ENABLE_WALLET
#include "paymentserver.h"
#include "walletmodel.h"
#endif
#include "masternodeconfig.h"

#include "init.h"
#include "main.h"
#include "rpc/server.h"
#include "guiinterface.h"
#include "util.h"

#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#ifdef Q_OS_MAC
#include "macdockiconhandler.h"
#endif

#include <signal.h>
#include <stdint.h>

#include <boost/filesystem/operations.hpp>
#include <boost/thread.hpp>

#include <QApplication>
#include <QDebug>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QTranslator>

#if defined(QT_STATICPLUGIN)
#include <QtPlugin>
#if QT_VERSION < 0x050400
Q_IMPORT_PLUGIN(AccessibleFactory)
#endif
#if defined(QT_QPA_PLATFORM_XCB)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_WINDOWS)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_COCOA)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#endif
#endif

// Declare meta types used for QMetaObject::invokeMethod
Q_DECLARE_METATYPE(bool*)
Q_DECLARE_METATYPE(CAmount)

extern bool appInitialized;

static void InitMessage(const std::string& message)
{
    LogPrintf("init message: %s\n", message);
}

/*
   Translate string to current locale using Qt.
 */
static std::string Translate(const char* psz)
{
    return QCoreApplication::translate("core", psz).toStdString();
}

static QString GetLangTerritory()
{
    QSettings settings;
    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = QLocale::system().name();
    // 2) Language from QSettings
    QString lang_territory_qsettings = settings.value("language", "").toString();
    if (!lang_territory_qsettings.isEmpty())
        lang_territory = lang_territory_qsettings;
    // 3) -lang command line argument
    lang_territory = QString::fromStdString(GetArg("-lang", lang_territory.toStdString()));
    return lang_territory;
}

/** Set up translations */
static void initTranslations(QTranslator& qtTranslatorBase, QTranslator& qtTranslator, QTranslator& translatorBase, QTranslator& translator)
{
    // Remove old translators
    QApplication::removeTranslator(&qtTranslatorBase);
    QApplication::removeTranslator(&qtTranslator);
    QApplication::removeTranslator(&translatorBase);
    QApplication::removeTranslator(&translator);

    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = GetLangTerritory();

    // Convert to "de" only by truncating "_DE"
    QString lang = lang_territory;
    lang.truncate(lang_territory.lastIndexOf('_'));

    // Load language files for configured locale:
    // - First load the translator for the base language, without territory
    // - Then load the more specific locale translator

    // Load e.g. qt_de.qm
    if (qtTranslatorBase.load("qt_" + lang, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslatorBase);

    // Load e.g. qt_de_DE.qm
    if (qtTranslator.load("qt_" + lang_territory, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslator);

    // Load e.g. nbx_de.qm (shortcut "de" needs to be defined in nbx.qrc)
    if (translatorBase.load(lang, ":/translations/"))
        QApplication::installTranslator(&translatorBase);

    // Load e.g. nbx_de_DE.qm (shortcut "de_DE" needs to be defined in nbx.qrc)
    if (translator.load(lang_territory, ":/translations/"))
        QApplication::installTranslator(&translator);
}

/* qDebug() message handler --> debug.log */
void DebugMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    Q_UNUSED(context);
    const char* category = (type == QtDebugMsg) ? "qt" : NULL;
    LogPrint(category, "GUI: %s\n", msg.toStdString());
}

/** Class encapsulating startup and shutdown.
 * Allows running startup and shutdown in a different thread from the UI thread.
 */
class BitcoinCore : public QObject
{
    Q_OBJECT
public:
    explicit BitcoinCore();

public slots:
    void initialize();
    void shutdown();
    void restart(QStringList args);

signals:
    void initializeResult(int retval);
    void runawayException(const QString& message);

private:
    /// Flag indicating a restart
    bool execute_restart;

    /// Pass fatal exception message to UI thread
    void handleRunawayException(std::exception* e);
};

/** Main application object */
class BitcoinApplication : public QApplication
{
    Q_OBJECT
public:
    explicit BitcoinApplication(int& argc, char** argv);
    ~BitcoinApplication();

#ifdef ENABLE_WALLET
    /// Create payment server
    void createPaymentServer();
#endif
    /// Create options model
    void createOptionsModel();
    /// Create main window
    void createWindow(const NetworkStyle* networkStyle);
    /// Create splash screen
    void createSplashScreen(const NetworkStyle* networkStyle);

    /// Request core initialization
    void requestInitialize();

    /// Get process return value
    int getReturnValue() { return returnValue; }

    /// Get window identifier of QMainWindow (BitcoinGUI)
    WId getMainWinId() const;

#if defined(Q_OS_WIN)
    /// Set shutdown monitor
    void setShutdownMonitor(WinShutdownMonitor* shutdownMonitor);
#endif

public slots:
    void initializeResult(int retval);
    /// Handle runaway exceptions. Shows a message box with the problem and quits the program.
    void handleRunawayException(const QString& message);
    /// Request core shutdown
    void requestShutdown();

signals:
    void requestedInitialize();
    void requestedRestart(QStringList args);
    void requestedShutdown();
    void stopThread();
    void splashFinished(QWidget* window);
    void applicationInitialized();

private:
    QThread* coreThread;
    OptionsModel* optionsModel;
    ClientModel* clientModel;
    BitcoinGUI* window;
    QTimer* pollShutdownTimer;
#if defined(Q_OS_WIN)
    WinShutdownMonitor* shutdownMonitor;
#endif
#ifdef ENABLE_WALLET
    PaymentServer* paymentServer;
    WalletModel* walletModel;
#endif
    bool shutdownAllowed;
    int returnValue;

    void startThread();
};

#include "netboxwallet.moc"

BitcoinCore::BitcoinCore() : QObject()
{
}

void BitcoinCore::handleRunawayException(std::exception* e)
{
    PrintExceptionContinue(e, "Runaway exception");
    emit runawayException(QString::fromStdString(strMiscWarning));
}

void BitcoinCore::initialize()
{
    execute_restart = true;

    try {
        qDebug() << __func__ << ": Running AppInit2 in thread";
        int rv = AppInit2();
        if (ResyncNeeded()) {
            if (!ShutdownRequested())
                restart(RPCConsole::buildParameterlist("-resync"));
        } else
            emit initializeResult(rv);
    } catch (std::exception& e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(NULL);
    }
}

void BitcoinCore::restart(QStringList args)
{
    if (execute_restart) { // Only restart 1x, no matter how often a user clicks on a restart-button
        execute_restart = false;
        try {
            qDebug() << __func__ << ": Running Restart in thread";
            Interrupt();
            PrepareShutdown();
            qDebug() << __func__ << ": Shutdown finished";
            CExplicitNetCleanup::callCleanup();
            QProcess::startDetached(QApplication::applicationFilePath(), args);
            qDebug() << __func__ << ": Restart initiated...";
            qDebug() << __func__ << ": Shutdown result: " << 1;
            qApp->quit(); // Exit main loop after shutdown finished
#if defined(Q_OS_WIN)
            WinShutdownMonitor::shutdownCompleted();
#endif
        } catch (std::exception& e) {
            handleRunawayException(&e);
        } catch (...) {
            handleRunawayException(NULL);
        }
    }
}

void BitcoinCore::shutdown()
{
    if (RestartRequested())
        return;
    try {
        qDebug() << __func__ << ": Running Shutdown in thread";
        Interrupt();
        Shutdown();
        qDebug() << __func__ << ": Shutdown finished";
        qDebug() << __func__ << ": Shutdown result: " << 1;
        qApp->quit(); // Exit main loop after shutdown finished
#if defined(Q_OS_WIN)
        WinShutdownMonitor::shutdownCompleted();
#endif
    } catch (std::exception& e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(NULL);
    }
}

BitcoinApplication::BitcoinApplication(int& argc, char** argv) : QApplication(argc, argv),
                                                                 coreThread(0),
                                                                 optionsModel(0),
                                                                 clientModel(0),
                                                                 window(0),
                                                                 pollShutdownTimer(0),
#if defined(Q_OS_WIN)
                                                                 shutdownMonitor(0),
#endif
#ifdef ENABLE_WALLET
                                                                 paymentServer(0),
                                                                 walletModel(0),
#endif
                                                                 shutdownAllowed(true),
                                                                 returnValue(0)
{
    setQuitOnLastWindowClosed(false);
}

BitcoinApplication::~BitcoinApplication()
{
    if (coreThread) {
        qDebug() << __func__ << ": Stopping thread";
        emit stopThread();
        coreThread->wait();
        qDebug() << __func__ << ": Stopped thread";
    }

    delete window;
    window = 0;
#ifdef ENABLE_WALLET
    delete paymentServer;
    paymentServer = 0;
#endif
    // Delete Qt-settings if user clicked on "Reset Options"
    QSettings settings;
    if (optionsModel && optionsModel->resetSettings) {
        settings.clear();
        settings.sync();
    }
    delete optionsModel;
    optionsModel = 0;
}

#ifdef ENABLE_WALLET
void BitcoinApplication::createPaymentServer()
{
    paymentServer = new PaymentServer(this);
}
#endif

void BitcoinApplication::createOptionsModel()
{
    optionsModel = new OptionsModel();
}

void BitcoinApplication::createWindow(const NetworkStyle* networkStyle)
{
    window = new BitcoinGUI(networkStyle, 0);

    pollShutdownTimer = new QTimer(window);
    connect(pollShutdownTimer, SIGNAL(timeout()), window, SLOT(detectShutdown()));
    connect(window, SIGNAL(requestedShutdown()), this, SLOT(requestShutdown()));
#if defined(Q_OS_WIN)
    connect(shutdownMonitor, SIGNAL(requestedShutdown()), window, SLOT(requestShutdown()));
#endif
    pollShutdownTimer->start(200);
}

void BitcoinApplication::createSplashScreen(const NetworkStyle* networkStyle)
{
    SplashScreen* splash = new SplashScreen(0, networkStyle);
    // We don't hold a direct pointer to the splash screen after creation, so use
    // Qt::WA_DeleteOnClose to make sure that the window will be deleted eventually.
    splash->setAttribute(Qt::WA_DeleteOnClose);
    splash->show();
    connect(this, SIGNAL(splashFinished(QWidget*)), splash, SLOT(slotFinish(QWidget*)));
}

void BitcoinApplication::startThread()
{
    if (coreThread)
        return;
    coreThread = new QThread(this);
    BitcoinCore* executor = new BitcoinCore();
    executor->moveToThread(coreThread);

    /*  communication to and from thread */
    connect(executor, SIGNAL(initializeResult(int)), this, SLOT(initializeResult(int)));
    connect(executor, SIGNAL(runawayException(QString)), this, SLOT(handleRunawayException(QString)));
    connect(this, SIGNAL(applicationInitialized()), window, SLOT(applicationInitialized()));
    connect(this, SIGNAL(requestedInitialize()), executor, SLOT(initialize()));
    connect(this, SIGNAL(requestedShutdown()), executor, SLOT(shutdown()));
    connect(window, SIGNAL(requestedRestart(QStringList)), executor, SLOT(restart(QStringList)));
    /*  make sure executor object is deleted in its own thread */
    connect(this, SIGNAL(stopThread()), executor, SLOT(deleteLater()));
    connect(this, SIGNAL(stopThread()), coreThread, SLOT(quit()));

    coreThread->start();
}

void BitcoinApplication::requestInitialize()
{
    qDebug() << __func__ << ": Requesting initialize";
    startThread();
    emit requestedInitialize();
}

void BitcoinApplication::requestShutdown()
{
    qDebug() << __func__ << ": Requesting shutdown";

    while (!shutdownAllowed) {
        processEvents();
        MilliSleep(100);
    }

    startThread();
#ifdef Q_OS_MAC
    if (GetBoolArg("-hide", false))
        MacDockIconHandler::toggleForegroundApp(false);
#endif
    window->hide();
    window->setClientModel(0);
    pollShutdownTimer->stop();

#ifdef ENABLE_WALLET
    window->removeAllWallets();
    delete walletModel;
    walletModel = 0;
#endif
    delete clientModel;
    clientModel = 0;

    // Show a simple window indicating shutdown status
#if defined(Q_OS_WIN)
    if (!WinShutdownMonitor::isShuttingDown())
        ShutdownWindow::showShutdownWindow(window);
    else
        LogPrintf("System shutdown\n");
#else
    ShutdownWindow::showShutdownWindow(window);
#endif

    // Request shutdown from core thread
    emit requestedShutdown();
}

void BitcoinApplication::initializeResult(int retval)
{
    shutdownAllowed = false;

    if (ShutdownRequested()) {
        shutdownAllowed = true;
        return;
    }

    qDebug() << __func__ << ": Initialization result: " << retval;
    // Set exit result: 0 if successful, 1 if failure
    returnValue = retval ? 0 : 1;
    if (retval) {
#ifdef ENABLE_WALLET
        PaymentServer::LoadRootCAs();
        paymentServer->setOptionsModel(optionsModel);
#endif

        clientModel = new ClientModel(optionsModel);
        window->setClientModel(clientModel);

#ifdef ENABLE_WALLET
        if (pwalletMain) {
            walletModel = new WalletModel(pwalletMain, optionsModel);

            window->addWallet(BitcoinGUI::DEFAULT_WALLET, walletModel);
            window->setCurrentWallet(BitcoinGUI::DEFAULT_WALLET);

            connect(walletModel, SIGNAL(coinsSent(CWallet*, SendCoinsRecipient, QByteArray)),
                paymentServer, SLOT(fetchPaymentACK(CWallet*, const SendCoinsRecipient&, QByteArray)));
        }
#endif

        emit applicationInitialized();

        // If -min option passed, start window minimized.
        if (GetBoolArg("-min", false)) {
            window->showMinimized();
        } else {
            if (!GetBoolArg("-hide", false))
                window->show();
        }
        emit splashFinished(window);

#ifdef ENABLE_WALLET
        // Now that initialization/startup is done, process any command-line
        // NBX: URIs or payment requests:
        connect(paymentServer, SIGNAL(receivedPaymentRequest(SendCoinsRecipient)),
            window, SLOT(handlePaymentRequest(SendCoinsRecipient)));
        connect(paymentServer, SIGNAL(receivedShowRequest()),
            window, SLOT(showNormalIfMinimized()));
        connect(window, SIGNAL(receivedURI(QString)),
            paymentServer, SLOT(handleURIOrFile(QString)));
        connect(paymentServer, SIGNAL(message(QString, QString, unsigned int)),
            window, SLOT(message(QString, QString, unsigned int)));
        QTimer::singleShot(100, paymentServer, SLOT(uiReady()));
#endif
    } else {
        StartShutdown();
    }
    shutdownAllowed = true;
}

void showErrorMessage(const QString& message){
#ifdef WIN32
    MessageBoxW(0, message.toStdWString().c_str(), L"Netbox.Wallet exception", 0);
#else
    QMessageBox::critical(0, "Netbox.Wallet exception", message);
#endif
}

void BitcoinApplication::handleRunawayException(const QString& message)
{
    showErrorMessage(message);
    exit(EXIT_FAILURE);
}

WId BitcoinApplication::getMainWinId() const
{
    if (!window)
        return 0;

    return window->winId();
}

#if defined(Q_OS_WIN)
void BitcoinApplication::setShutdownMonitor(WinShutdownMonitor* shutdownMonitor)
{
    this->shutdownMonitor = shutdownMonitor;
}
#endif

#ifdef WIN32
LONG WINAPI exceptionHandlerGui(PEXCEPTION_POINTERS ExceptionInfo)
{
    std::runtime_error exception(parseWinException(ExceptionInfo));
    PrintExceptionContinue(&exception, "", false);
    showErrorMessage(QString::fromStdString(strMiscWarning));
    ExitProcess(EXIT_FAILURE);
}
#else
void segFaultHandlerGui(int signum)
{
    std::runtime_error exception(parseSegFault(signum));
    PrintExceptionContinue(&exception, "");
    showErrorMessage(QString::fromStdString(strMiscWarning));
    _exit(EXIT_FAILURE);
}
#endif

int rpcShow()
{
#ifndef ENABLE_WALLET
    return 3;
#else
    if (!PaymentServer::ipcSendCommand("nbx:show"))
        return 2;
    if (!appInitialized)
        return 1;
    return 0;
#endif
}

#ifndef BITCOIN_QT_TEST
int main(int argc, char* argv[])
{
    // set exception handling
#ifdef WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(exceptionHandlerGui);
#else
    // catch SIGSEGV
    struct sigaction sa_segv;
    sa_segv.sa_handler = segFaultHandlerGui;
    sigemptyset(&sa_segv.sa_mask);
    sa_segv.sa_flags = 0;
    sigaction(SIGSEGV, &sa_segv, NULL);
    sigaction(SIGFPE, &sa_segv, NULL);
    sigaction(SIGILL, &sa_segv, NULL);
#endif

    SetupEnvironment();

    /// 1. Parse command-line options. These take precedence over anything else.
    // Command-line options take precedence:
    ParseParameters(argc, argv);

// Do not refer to data directory yet, this can be overridden by Intro::pickDataDirectory

/// 2. Basic Qt initialization (not dependent on parameters or configuration)
    Q_INIT_RESOURCE(nbx_locale);
    Q_INIT_RESOURCE(nbx);

#ifdef Q_OS_MAC
    if (GetBoolArg("-hide", false))
        qputenv("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM", "1");
#endif

    BitcoinApplication app(argc, argv);
#if QT_VERSION > 0x050100
    // Generate high-dpi pixmaps
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
#if QT_VERSION >= 0x050600
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
#ifdef Q_OS_MAC
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus);
#endif

    // Register meta types used for QMetaObject::invokeMethod
    qRegisterMetaType<bool*>();
    //   Need to pass name here as CAmount is a typedef (see http://qt-project.org/doc/qt-5/qmetatype.html#qRegisterMetaType)
    //   IMPORTANT if it is no longer a typedef use the normal variant above
    qRegisterMetaType<CAmount>("CAmount");

    /// 3. Application identification
    // must be set before OptionsModel is initialized or translations are loaded,
    // as it is used to locate QSettings
    QApplication::setOrganizationName(QAPP_ORG_NAME);
    QApplication::setOrganizationDomain(QAPP_ORG_DOMAIN);
    QApplication::setApplicationName(QAPP_APP_NAME_DEFAULT);
    GUIUtil::SubstituteFonts(GetLangTerritory());

    /// 4. Initialization of translations, so that intro dialog is in user's language
    // Now that QSettings are accessible, initialize translations
    QTranslator qtTranslatorBase, qtTranslator, translatorBase, translator;
    initTranslations(qtTranslatorBase, qtTranslator, translatorBase, translator);
    uiInterface.Translate.connect(Translate);

    // Show help message immediately after parsing command-line options (for "-lang") and setting locale,
    // but before showing splash screen.
    if (mapArgs.count("-?") || mapArgs.count("-help") || mapArgs.count("-version")) {
        HelpMessageDialog help(NULL, mapArgs.count("-version"));
        help.showOrPrint();
        return 1;
    }

    /// 5. Now that settings and translations are available, ask user for data directory
    // User language is set up: pick a data directory
    if (!Intro::pickDataDirectory())
        return 0;

    /// 6. Determine availability of data directory and parse nbx.conf
    /// - Do not call GetDataDir(true) before this step finishes
    if (!boost::filesystem::is_directory(GetDataDir(false))) {
        QMessageBox::critical(0, QObject::tr("Netbox.Wallet"),
            QObject::tr("Error: Specified data directory \"%1\" does not exist.").arg(QString::fromStdString(mapArgs["-datadir"])));
        return 1;
    }
    try {
        ReadConfigFile(mapArgs, mapMultiArgs);
    } catch (std::exception& e) {
        QMessageBox::critical(0, QObject::tr("Netbox.Wallet"),
            QObject::tr("Error: Cannot parse configuration file: %1. Only use key=value syntax.").arg(e.what()));
        return 0;
    }

    /// 7. Determine network (and switch to network specific options)
    // - Do not call Params() before this step
    // - Do this after parsing the configuration file, as the network can be switched there
    // - QSettings() will use the new application name after this, resulting in network-specific settings
    // - Needs to be done before createOptionsModel

    // Check for -testnet or -regtest parameter (Params() calls are only valid after this clause)
    if (!SelectParamsFromCommandLine()) {
        QMessageBox::critical(0, QObject::tr("Netbox.Wallet"), QObject::tr("Error: Invalid combination of -regtest and -testnet."));
        return 1;
    }

#ifdef ENABLE_WALLET
    // Parse URIs on command line -- this can affect Params()
    PaymentServer::ipcParseCommandLine(argc, argv);
#endif

    QScopedPointer<const NetworkStyle> networkStyle(NetworkStyle::instantiate(QString::fromStdString(Params().NetworkIDString())));
    assert(!networkStyle.isNull());
    // Allow for separate UI settings for testnets
    QApplication::setApplicationName(networkStyle->getAppName());
    // Re-initialize translations after changing application name (language in network-specific settings can be different)
    initTranslations(qtTranslatorBase, qtTranslator, translatorBase, translator);

#ifdef ENABLE_WALLET
    /// 7a. parse masternode.conf
    std::string strErr;
    if (!masternodeConfig.read(strErr)) {
        QMessageBox::critical(0, QObject::tr("Netbox.Wallet"),
            QObject::tr("Error reading masternode configuration file: %1").arg(strErr.c_str()));
        return 0;
    }

    /// 8. URI IPC sending
    // - Do this early as we don't want to bother initializing if we are just calling IPC
    // - Do this *after* setting up the data directory, as the data directory hash is used in the name
    // of the server.
    // - Do this after creating app and setting up translations, so errors are
    // translated properly.
    if (PaymentServer::ipcSendCommandLine())
        exit(0);

    // try to show another already running instance
#ifdef WIN32
    AllowSetForegroundWindow(ReadPidFile(GetPidFile()) ?: ASFW_ANY);
#endif
    if (PaymentServer::ipcSendCommand("nbx:show"))
        exit(0);

    // Start up the payment server early, too, so impatient users that click on
    // nbx: links repeatedly have their payment requests routed to this process:
    app.createPaymentServer();
#endif

    /// 9. Main GUI initialization
    // Install global event filter that makes sure that long tooltips can be word-wrapped
    app.installEventFilter(new GUIUtil::ToolTipToRichTextFilter(TOOLTIP_WRAP_THRESHOLD, &app));
#if defined(Q_OS_WIN)
    // Install global event filter for processing Windows session related Windows messages (WM_QUERYENDSESSION and WM_ENDSESSION)
    WinShutdownMonitor* shutdownMonitor = new WinShutdownMonitor();
    app.setShutdownMonitor(shutdownMonitor);
    qApp->installNativeEventFilter(shutdownMonitor);
#endif
    // Install qDebug() message handler to route to debug.log
    qInstallMessageHandler(DebugMessageHandler);
    // Load GUI settings from QSettings
    app.createOptionsModel();

    // Subscribe to global signals from core
    uiInterface.InitMessage.connect(InitMessage);

    if (GetBoolArg("-splash", true) && !GetBoolArg("-min", false))
        app.createSplashScreen(networkStyle.data());

    try {
        app.createWindow(networkStyle.data());
        app.requestInitialize();
#if defined(Q_OS_WIN)
        WinShutdownMonitor::registerShutdownBlockReason(QObject::tr("Netbox.Wallet didn't yet exit safely..."), (HWND)app.getMainWinId());
#endif
        app.exec();
    } catch (std::exception& e) {
        PrintExceptionContinue(&e, "main");
        app.handleRunawayException(QString::fromStdString(strMiscWarning));
    } catch (...) {
        PrintExceptionContinue(NULL, "main");
        app.handleRunawayException(QString::fromStdString(strMiscWarning));
    }
    return app.getReturnValue();
}
#endif // BITCOIN_QT_TEST
