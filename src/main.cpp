#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSettings>
#include <QStandardPaths>

#include <csignal>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "app/application.h"
#include "app/config_store.h"
#include "app/data_relocation.h"
#include "bootstrap/legacymigration.h"
#include "bootstrap/single_instance.h"
#include "bootstrap/startupinfo.h"
#include "common/logging.h"
#include "common/performance_metrics.h"
#include "data/manticore.h"
#include "librats/util/logger.h"
#include "mainwindow.h"
#include "migrationprogresswindow.h"
#include "services/migration_service.h"
#include "theme.h"
#include "version.h"

#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================================
// Data directory persistence (QSettings).
//
// Chicken-and-egg: the config lives inside the data directory, but we need to
// know the data directory before we can read config. So the chosen path is
// stored separately in QSettings.
// ============================================================================
static QString getSavedDataDirectory()
{
    return QSettings(QStringLiteral("RatsSearch"), QStringLiteral("RatsSearch"))
        .value(QStringLiteral("dataDirectory"))
        .toString();
}

static bool saveDataDirectory(const QString& path)
{
    QSettings settings(QStringLiteral("RatsSearch"), QStringLiteral("RatsSearch"));
    settings.setValue(QStringLiteral("dataDirectory"), path);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

#if defined(_WIN32) && !defined(NDEBUG)
// Attach a console on Windows so stdout/stderr are visible in Debug builds.
static void attachConsoleOnWindows()
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);
    std::ios::sync_with_stdio();
}
#endif

// Route Qt logging through the librats logger (single log file/sink).
static void customMessageHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    const std::string message = msg.toLocal8Bit().constData();
    auto& logger = librats::Logger::getInstance();
    switch (type) {
    case QtDebugMsg:
        logger.log(librats::LogLevel::DEBUG, "RatsSearch", message);
        break;
    case QtInfoMsg:
        logger.log(librats::LogLevel::INFO, "RatsSearch", message);
        break;
    case QtWarningMsg:
        logger.log(librats::LogLevel::WARN, "RatsSearch", message);
        break;
    case QtCriticalMsg:
        logger.log(librats::LogLevel::ERROR, "RatsSearch", message);
        break;
    case QtFatalMsg:
        logger.log(librats::LogLevel::ERROR, "RatsSearch", "[FATAL] " + message);
        abort();
    }
}

// ---- graceful Ctrl+C shutdown (console) ------------------------------------
static QCoreApplication* g_app = nullptr;
static bool g_shutdownRequested = false;

static void signalHandler(int)
{
    if (g_shutdownRequested)
        std::exit(1);
    g_shutdownRequested = true;
    std::cout << "\nShutting down..." << std::endl;
    if (g_app)
        QMetaObject::invokeMethod(g_app, []() { QCoreApplication::quit(); }, Qt::QueuedConnection);
}

// ============================================================================
// Shared startup, used identically by console and GUI modes.
// ============================================================================
// Parse an on/off style CLI value. Returns nullopt for anything unrecognised so
// the caller can reject it loudly instead of silently picking a default.
static std::optional<bool> parseBoolOption(const QString& value)
{
    const QString v = value.trimmed().toLower();
    if (v == QLatin1String("on") || v == QLatin1String("true") || v == QLatin1String("yes") || v == QLatin1String("1"))
        return true;
    if (v == QLatin1String("off") || v == QLatin1String("false") || v == QLatin1String("no") || v == QLatin1String("0"))
        return false;
    return std::nullopt;
}

static void addCommonOptions(QCommandLineParser& parser, QCommandLineOption& port, QCommandLineOption& dhtPort,
    QCommandLineOption& dataDir, QCommandLineOption& maxPeers, QCommandLineOption& spider, QCommandLineOption& console,
    QCommandLineOption& shareDb)
{
    parser.setApplicationDescription(QStringLiteral(RATSSEARCH_PRODUCT_NAME " - BitTorrent P2P Search Engine"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(console);
    parser.addOption(port);
    parser.addOption(dhtPort);
    parser.addOption(dataDir);
    parser.addOption(QCommandLineOption("no-remember-data-dir",QStringLiteral("Do not remember the explicit data directory (isolated/portable runs)")));
    parser.addOption(maxPeers);
    parser.addOption(spider);
    parser.addOption(shareDb);
}

static QString resolveDataDirectory(QCommandLineParser& parser, const QCommandLineOption& dataDirOption,
    const QString& defaultDataDirectory)
{
    QString dataDir;
    if (parser.isSet(dataDirOption)) {
        dataDir = parser.value(dataDirOption);
        if(!parser.isSet("no-remember-data-dir"))saveDataDirectory(dataDir);
    } else {
        dataDir = getSavedDataDirectory();
        if (dataDir.isEmpty())
            dataDir = defaultDataDirectory;
    }
    return dataDir;
}

static void configureLogging(const QString& dataDir)
{
    auto& logger = librats::Logger::getInstance();
    const QString logFilePath = dataDir + QStringLiteral("/rats-search.log");
    logger.set_log_file_path(logFilePath.toStdString());
    // Bounded from the first line on: config lives inside the data directory and
    // is not loaded yet, so start on the default budget. Application::applyConfig()
    // re-applies the stored logMaxSizeMb moments later, and on every later change.
    rats::common::applyLogSizeBudget(rats::common::kDefaultLogMaxSizeMb);
    logger.set_rotate_on_startup(true); // must precede set_file_logging_enabled()
    logger.set_file_logging_enabled(true);
#ifdef NDEBUG
    logger.set_log_level(librats::LogLevel::INFO);
#else
    logger.set_log_level(librats::LogLevel::DEBUG);
#endif
    if (qEnvironmentVariableIntValue("RATS_VERBOSE_CRAWLER") == 1) {
        logger.set_log_level(librats::LogLevel::DEBUG);
        QLoggingCategory::setFilterRules(QString::fromUtf8(qgetenv("QT_LOGGING_RULES"))
            + QStringLiteral("\nrats.crawler.detail.debug=true"));
    }
    qInfo() << "Log file:" << logFilePath;
    logStartupInfo(dataDir);
}

// Print the blocking pre-start migrations to stdout. They run inside
// Application::start() and can take minutes on a large index; a daemon that
// prints nothing for that long looks wedged. Only the sync migrations are drawn
// — the background ones report through the same signal, but they run while the
// daemon is serving and would scribble over its regular output.
static void reportMigrationsToConsole(rats::app::Application* application)
{
    auto* migrations = application->migrations();
    if (!migrations)
        return;

    struct DrawState {
        bool active = false;
        int lastPercent = -1;
    };
    auto state = std::make_shared<DrawState>();

    QObject::connect(migrations, &rats::service::MigrationService::syncMigrationStarted, application,
        [state](const QString&, const QString& description) {
            state->active = true;
            state->lastPercent = -1;
            std::cout << "Data migration: " << description.toStdString() << std::endl;
        });

    QObject::connect(migrations, &rats::service::MigrationService::migrationProgress, application,
        [state](const QString&, qint64 current, qint64 total) {
            if (!state->active || total <= 0)
                return;
            const int percent = static_cast<int>((qMin(current, total) * 100) / total);
            if (percent == state->lastPercent)
                return;
            state->lastPercent = percent;
            constexpr int kBarWidth = 30;
            const int filled = percent * kBarWidth / 100;
            std::cout << "\r  [" << std::string(filled, '#') << std::string(kBarWidth - filled, '.') << "] " << percent
                      << "%  " << current << '/' << total << "   " << std::flush;
        });

    QObject::connect(migrations, &rats::service::MigrationService::syncMigrationsFinished, application, [state]() {
        if (state->lastPercent >= 0)
            std::cout << std::endl;
        state->active = false;
        std::cout << "Data migration finished." << std::endl;
    });
}

// Console mode on the new rats:: architecture: build the composition root,
// start it, and run the event loop until interrupted. No widgets.
static int runConsoleApplication(QCoreApplication& app, rats::app::Application::Options options,
    const std::function<bool(bool)>& completeRelocation)
{
    options.headless = true;
    g_app = &app;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    auto application = std::make_unique<rats::app::Application>(std::move(options));
    reportMigrationsToConsole(application.get());
    const bool started = application->start();
    if (!completeRelocation(started) || !started) {
        qCritical() << "Failed to start application";
        return 1;
    }
    qInfo() << RATSSEARCH_PRODUCT_NAME " (console) running. Press Ctrl+C to quit.";
    QObject::connect(&app, &QCoreApplication::aboutToQuit, application.get(), [a = application.get()]() { a->stop(); });
    return app.exec();
}

int main(int argc, char* argv[])
{
#if defined(_WIN32) && !defined(NDEBUG)
    attachConsoleOnWindows();
#endif
    qInstallMessageHandler(customMessageHandler);

    // QApplication vs QCoreApplication must be chosen before construction, so we
    // pre-scan argv for the console flag.
    bool consoleMode = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--console") || arg == QLatin1String("-c")) {
            consoleMode = true;
            break;
        }
    }

    std::unique_ptr<QCoreApplication> qapp;
    if (consoleMode)
        qapp = std::make_unique<QCoreApplication>(argc, argv);
    else
        qapp = std::make_unique<QApplication>(argc, argv);

    QCoreApplication::setOrganizationName(QString()); // empty avoids a nested folder
    // Resolve the existing default profile before setting the display brand.
    QCoreApplication::setApplicationName(QStringLiteral("Rats Search"));
    const QString defaultDataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QCoreApplication::setApplicationName(QStringLiteral(RATSSEARCH_PRODUCT_NAME));
    if (!consoleMode)
        QApplication::setApplicationDisplayName(QStringLiteral(RATSSEARCH_PRODUCT_NAME));
    QCoreApplication::setApplicationVersion(QStringLiteral(RATSSEARCH_VERSION_STRING));

    QCommandLineOption consoleOption(QStringList() << "c" << "console", QStringLiteral("Run without a GUI"));
    QCommandLineOption portOption(QStringList() << "p" << "port", QStringLiteral("P2P listen port"), "port");
    QCommandLineOption dhtPortOption(QStringList() << "d" << "dht-port", QStringLiteral("DHT port"), "dht-port");
    QCommandLineOption dataDirOption(QStringList() << "data-dir", QStringLiteral("Data directory"), "path");
    QCommandLineOption maxPeersOption(QStringList() << "m" << "max-peers", QStringLiteral("Max P2P connections"), "n");
    QCommandLineOption spiderOption(QStringList() << "s" << "spider", QStringLiteral("Force-enable the DHT spider"));
    QCommandLineOption shareDbOption(QStringList() << "share-db",
        QStringLiteral("Serve the whole database to peers that ask: on|off "
                       "(overrides the databaseSharing config key for this run)"),
        "on|off");
    QCommandLineParser parser;
    addCommonOptions(
        parser, portOption, dhtPortOption, dataDirOption, maxPeersOption, spiderOption, consoleOption, shareDbOption);
    parser.process(*qapp);

    QString dataDir = resolveDataDirectory(parser, dataDirOption, defaultDataDirectory);
    if (!QDir().mkpath(dataDir)) {
        qCritical() << "Failed to create data directory:" << dataDir;
        return 1;
    }
    // Refuse to run twice on one data directory (shared Manticore, RT tables and
    // rats.json). Must precede configureLogging(): log rotation on startup would
    // otherwise roll the running instance's log file out from under it.
    auto instanceGuard = std::make_unique<rats::bootstrap::SingleInstanceGuard>(dataDir);
    if (!instanceGuard->tryAcquire()) {
        const QString holder = instanceGuard->runningInstanceInfo();
        qInfo().noquote() << QStringLiteral(RATSSEARCH_PRODUCT_NAME " is already running on %1%2 - activating it")
                                 .arg(dataDir, holder.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(holder));
        if (!instanceGuard->notifyRunningInstance())
            qWarning() << "Could not reach the running instance (it may still be starting up)";
        return 0;
    }

    namespace relocation = rats::app::relocation;
    const QString originalDataDir = dataDir;
    const auto request = relocation::pending(originalDataDir);
    const auto relocationMessage = [consoleMode](const QString& message) {
        qWarning().noquote() << message;
        if (!consoleMode)
            QMessageBox::warning(nullptr, QCoreApplication::translate("DataRelocation", "Data migration"), message);
    };
    std::unique_ptr<rats::bootstrap::SingleInstanceGuard> sourceGuard;
    if (!request.destination.isEmpty()) {
        QString error;
        if (!instanceGuard->holdsLock() || rats::data::Manticore::hasLiveDaemon(originalDataDir)) {
            error = QCoreApplication::translate("DataRelocation", "The source database is still in use.");
        } else {
            std::unique_ptr<QProgressDialog> dialog;
            if (!consoleMode) {
                dialog = std::make_unique<QProgressDialog>(
                    QCoreApplication::translate("DataRelocation", "Copying database and profile…"),
                    QCoreApplication::translate("DataRelocation", "Cancel"), 0, 100);
                dialog->setMinimumDuration(0);
                dialog->setAutoClose(false);
                dialog->setAutoReset(false);
            } else {
                std::cout << "Copying database and profile..." << std::endl;
            }
            QElapsedTimer ticks;
            ticks.start();
            error = relocation::prepare(originalDataDir, request, [&](qint64 copied, qint64 total) {
                if (dialog && (copied == 0 || ticks.elapsed() >= 100)) {
                    dialog->setValue(total ? static_cast<int>(100.0 * copied / total) : 0);
                    QCoreApplication::processEvents();
                    ticks.restart();
                }
                return !dialog || !dialog->wasCanceled();
            });
            if (error.isEmpty()) {
                auto destinationGuard = std::make_unique<rats::bootstrap::SingleInstanceGuard>(request.destination);
                if (destinationGuard->tryAcquire() && destinationGuard->holdsLock()) {
                    sourceGuard = std::move(instanceGuard);
                    instanceGuard = std::move(destinationGuard);
                    dataDir = request.destination;
                } else {
                    error = QCoreApplication::translate("DataRelocation", "The destination is in use or cannot be locked.");
                }
            }
        }
        if (!error.isEmpty()) {
            relocationMessage(error);
            if (const auto clearError = relocation::clear(originalDataDir); !clearError.isEmpty())
                relocationMessage(clearError);
        }
    }
    const auto completeRelocation = [&](bool started) {
        if (!sourceGuard)
            return true;
        if (!started) {
            relocationMessage(QCoreApplication::translate("DataRelocation",
                "The copied database could not be opened. The original location is still selected."));
            relocation::clear(originalDataDir);
            return false;
        }
        if (!parser.isSet("no-remember-data-dir") && !saveDataDirectory(dataDir)) {
            relocationMessage(QCoreApplication::translate("DataRelocation", "Could not save the new data directory."));
            return false;
        }
        relocation::finish(originalDataDir, request);
        sourceGuard.reset();
        qInfo().noquote() << "Data migration completed:" << dataDir;
        return true;
    };

    if (!sourceGuard)
        migrateLegacyDatabase(dataDir); // one-time v1.x -> v2.0 import
    configureLogging(dataDir);

    rats::app::Application::Options options;
    options.dataDirectory = dataDir;
    options.clientVersion = QCoreApplication::applicationVersion();
    options.p2pPort = parser.isSet(portOption) ? parser.value(portOption).toInt() : 0;
    options.dhtPort = parser.isSet(dhtPortOption) ? parser.value(dhtPortOption).toInt() : 0;
    options.maxPeers = parser.isSet(maxPeersOption) ? parser.value(maxPeersOption).toInt() : 0;
    options.forceSpider = parser.isSet(spiderOption);
    if (parser.isSet(shareDbOption)) {
        options.shareDatabase = parseBoolOption(parser.value(shareDbOption));
        if (!options.shareDatabase) {
            qCritical() << "Invalid --share-db value:" << parser.value(shareDbOption) << "- expected on or off";
            return 1;
        }
    }

    if (consoleMode)
        return runConsoleApplication(*qapp, std::move(options), completeRelocation);

    // ---- GUI mode --------------------------------------------------------
    auto application = std::make_unique<rats::app::Application>(std::move(options));
    // Blocking pre-start migrations run inside start(), before MainWindow exists
    // and before the event loop is entered — the splash draws itself from inside
    // that call, and stays hidden when there is no migration to run.
    // The splash is the first styled window, so the user's font is set here; the
    // Theme keeps it for every window after.
    rats::ui::Theme::instance().setFont(application->config()->uiFontFamily(), application->config()->uiFontScale());
    MigrationProgressWindow migrationSplash(application->migrations(), application->config()->darkMode());
    const bool started = application->start();
    if (!completeRelocation(started) || !started) {
        qCritical() << "Failed to start application";
        return 1;
    }

    MainWindow window(application.get());
    // A second launch is the user asking for the window, even when this instance
    // is sitting minimized in the tray.
    QObject::connect(instanceGuard.get(), &rats::bootstrap::SingleInstanceGuard::secondInstanceStarted, &window,
        &MainWindow::bringToFront);
    if (!application->config()->startMinimized())
        window.show();

    const int rc = qapp->exec();
    application->stop();
    return rc;
}
