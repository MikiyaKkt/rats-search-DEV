#include "downloadadvancedwidgets.h"
#include "downloadtextbinder.h"
#include "settingsdialog.h"
#include "app/application.h"
#include "app/data_relocation.h"
#include "app/config_store.h"
#include "app/search_history_store.h"
#include "app/translation_manager.h"
#include "autostartmanager.h"
#include "common/logging.h"
#include "common/crawler_limits.h"
#include "net/crawler.h"
#include <QTimer>
#include "services/download_service.h"
#include "rest/api_router.h"
#include <QApplication>

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QScrollArea>
#include <QFileInfo>
#include <QSettings>
#include <QStyle>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(rats::app::Application* app, QWidget* parent)
    : QDialog(parent), app_(app), dataDirectory_(app ? app->options().dataDirectory : QString())
{
    setWindowTitle(tr("Settings"));
    setMinimumSize(600, 550);
    resize(650, 620);

    setupUi();
    loadSettings();
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::setupUi()
{
    QVBoxLayout* dialogLayout = new QVBoxLayout(this);
    dialogLayout->setSpacing(12);
    dialogLayout->setContentsMargins(16, 16, 16, 16);

    // Title
    QLabel* titleLabel = new QLabel(tr("Rats Search Adv Settings"));
    titleLabel->setObjectName("headerLabel");
    dialogLayout->addWidget(titleLabel);

    // Tab widget
    tabWidget_ = new QTabWidget(this);
    tabWidget_->addTab(createGeneralTab(), tr("⚙️ General"));
    tabWidget_->addTab(createNetworkTab(), tr("🌐 Network"));
    tabWidget_->addTab(createIndexerTab(), tr("🕷️ Indexer"));
    tabWidget_->addTab(createFiltersTab(), tr("🔍 Filters"));
    tabWidget_->addTab(createStorageTab(), tr("💾 Storage"));
    tabWidget_->addTab(createDownloadsTab(), tr("Downloads"));
    dialogLayout->addWidget(tabWidget_, 1);

    // Buttons
    QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    dialogLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccepted);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

// =============================================================================
// Helper: wrap any content widget in a QScrollArea
// =============================================================================
QWidget* SettingsDialog::wrapInScrollArea(QWidget* content)
{
    QScrollArea* scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(content);

    QWidget* wrapper = new QWidget();
    QVBoxLayout* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scrollArea);

    // Prevent scroll-wheel from changing spinboxes/combos/sliders
    // when user just scrolls the page
    installScrollGuard(content);

    return wrapper;
}

// =============================================================================
// Helper: install event filter on all scrollable input widgets
// =============================================================================
void SettingsDialog::installScrollGuard(QWidget* container)
{
    // Find all QSpinBox, QComboBox, QSlider children
    const auto spinBoxes = container->findChildren<QSpinBox*>();
    for (auto* w : spinBoxes) {
        w->setFocusPolicy(Qt::StrongFocus);
        w->installEventFilter(this);
    }
    const auto comboBoxes = container->findChildren<QComboBox*>();
    for (auto* w : comboBoxes) {
        w->setFocusPolicy(Qt::StrongFocus);
        w->installEventFilter(this);
    }
    const auto sliders = container->findChildren<QSlider*>();
    for (auto* w : sliders) {
        w->setFocusPolicy(Qt::StrongFocus);
        w->installEventFilter(this);
    }
}

// =============================================================================
// Event filter: block wheel events on unfocused input widgets
// =============================================================================
bool SettingsDialog::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::Wheel) {
        auto* widget = qobject_cast<QWidget*>(obj);
        if (widget && !widget->hasFocus()) {
            // Forward the wheel event to the parent scroll area instead
            event->ignore();
            return true;
        }
    }
    return QDialog::eventFilter(obj, event);
}

// =============================================================================
// Tab: General
// =============================================================================
QWidget* SettingsDialog::createGeneralTab()
{
    QWidget* tab = new QWidget();
    QVBoxLayout* tabLayout = new QVBoxLayout(tab);
    tabLayout->setSpacing(16);
    tabLayout->setContentsMargins(12, 12, 12, 12);

    // --- Appearance ---
    QGroupBox* appearanceGroup = new QGroupBox(tr("Appearance"));
    QFormLayout* appearanceLayout = new QFormLayout(appearanceGroup);
    appearanceLayout->setSpacing(10);

    languageCombo_ = new QComboBox();
    for (const auto& lang : rats::app::TranslationManager::instance().availableLanguages())
        languageCombo_->addItem(lang.flagEmoji + QStringLiteral(" ") + lang.nativeName, lang.code);
    appearanceLayout->addRow(tr("Language:"), languageCombo_);

    darkModeCheck_ = new QCheckBox(tr("Dark mode"));
    appearanceLayout->addRow(darkModeCheck_);

    // An empty family means the theme's own font stack, which picks a sensible
    // font on every platform — so that, not a concrete family, is the default.
    fontFamilyCombo_ = new QComboBox();
    fontFamilyCombo_->addItem(tr("Default"), QString());
    for (const QString& family : QFontDatabase::families()) {
        if (!QFontDatabase::isPrivateFamily(family)) {
            fontFamilyCombo_->addItem(family, family);
        }
    }
    fontFamilyCombo_->setToolTip(tr("Font used throughout the interface"));
    appearanceLayout->addRow(tr("Font:"), fontFamilyCombo_);

    fontScaleSpin_ = new QSpinBox();
    fontScaleSpin_->setRange(rats::app::ConfigStore::kMinUiFontScale, rats::app::ConfigStore::kMaxUiFontScale);
    fontScaleSpin_->setSingleStep(10);
    fontScaleSpin_->setSuffix(QStringLiteral("%"));
    fontScaleSpin_->setToolTip(tr("Size of the interface text relative to the default"));
    appearanceLayout->addRow(tr("Font size:"), fontScaleSpin_);

    tabLayout->addWidget(appearanceGroup);

    // --- Startup & Tray ---
    QGroupBox* startupGroup = new QGroupBox(tr("Startup && System Tray"));
    QFormLayout* startupLayout = new QFormLayout(startupGroup);
    startupLayout->setSpacing(10);

    autoStartCheck_ = new QCheckBox(tr("Start with system (autostart)"));
    autoStartCheck_->setToolTip(tr("Automatically start Rats Search Adv when you log in to your computer"));
    startupLayout->addRow(autoStartCheck_);

    startMinimizedCheck_ = new QCheckBox(tr("Start minimized"));
    startupLayout->addRow(startMinimizedCheck_);

    minimizeToTrayCheck_ = new QCheckBox(tr("Hide to tray on minimize"));
    startupLayout->addRow(minimizeToTrayCheck_);

    closeToTrayCheck_ = new QCheckBox(tr("Hide to tray on close"));
    startupLayout->addRow(closeToTrayCheck_);

    confirmExitCheck_ = new QCheckBox(tr("Ask for confirmation before exiting"));
    startupLayout->addRow(confirmExitCheck_);

    // When autostart is enabled, suggest starting minimized
    connect(autoStartCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && !startMinimizedCheck_->isChecked()) {
            startMinimizedCheck_->setChecked(true);
        }
    });

    tabLayout->addWidget(startupGroup);

    // --- Search ---
    QGroupBox* searchGroup = new QGroupBox(tr("Search"));
    QFormLayout* searchLayout = new QFormLayout(searchGroup);
    searchLayout->setSpacing(10);

    searchHistoryCheck_ = new QCheckBox(tr("Remember search history"));
    searchHistoryCheck_->setToolTip(
        tr("Store recent search queries and offer them in the search field. Turning this off stops new queries from "
           "being recorded; already stored ones are kept until you clear them."));
    searchLayout->addRow(searchHistoryCheck_);

    clearSearchHistoryButton_ = new QPushButton(tr("Clear search history"));
    // Neutral, like the other in-place actions on this page: the accent is for
    // the dialog's own accept button, not for a side action.
    clearSearchHistoryButton_->setObjectName("secondaryButton");
    // Clearing is immediate and independent of Save — it is an action, not a
    // setting, and there is nothing to roll back on Cancel.
    connect(clearSearchHistoryButton_, &QPushButton::clicked, this, [this]() {
        if (!app_ || !app_->searchHistory())
            return;
        app_->searchHistory()->clear();
        updateSearchHistoryButton();
    });
    searchLayout->addRow(clearSearchHistoryButton_);

    tabLayout->addWidget(searchGroup);

    // --- Updates ---
    QGroupBox* updatesGroup = new QGroupBox(tr("Updates"));
    QFormLayout* updatesLayout = new QFormLayout(updatesGroup);

    checkUpdatesCheck_ = new QCheckBox(tr("Check for updates on startup"));
    updatesLayout->addRow(checkUpdatesCheck_);

    tabLayout->addWidget(updatesGroup);

    tabLayout->addStretch();
    return wrapInScrollArea(tab);
}

// =============================================================================
// Tab: Network
// =============================================================================
QWidget* SettingsDialog::createNetworkTab()
{
    QWidget* tab = new QWidget();
    QVBoxLayout* tabLayout = new QVBoxLayout(tab);
    tabLayout->setSpacing(16);
    tabLayout->setContentsMargins(12, 12, 12, 12);

    // --- Ports ---
    QGroupBox* portsGroup = new QGroupBox(tr("Ports"));
    QFormLayout* portsLayout = new QFormLayout(portsGroup);
    portsLayout->setSpacing(10);

    p2pPortSpin_ = new QSpinBox();
    p2pPortSpin_->setRange(1024, 65535);
    portsLayout->addRow(tr("P2P Port:"), p2pPortSpin_);

    dhtPortSpin_ = new QSpinBox();
    dhtPortSpin_->setRange(1024, 65535);
    portsLayout->addRow(tr("DHT Port:"), dhtPortSpin_);

    httpPortSpin_ = new QSpinBox();
    httpPortSpin_->setRange(1024, 65535);
    portsLayout->addRow(tr("HTTP API Port:"), httpPortSpin_);

    QLabel* portsHint = new QLabel(tr("* Changing ports requires restart"));
    portsHint->setObjectName("hintLabel");
    portsLayout->addRow(portsHint);

    tabLayout->addWidget(portsGroup);

    // --- P2P Network ---
    QGroupBox* p2pGroup = new QGroupBox(tr("P2P Network"));
    QFormLayout* p2pLayout = new QFormLayout(p2pGroup);
    p2pLayout->setSpacing(10);

    p2pConnectionsSpin_ = new QSpinBox();
    p2pConnectionsSpin_->setRange(10, 1000);
    p2pConnectionsSpin_->setToolTip(tr("Maximum number of P2P connections"));
    p2pLayout->addRow(tr("Max connections:"), p2pConnectionsSpin_);

    p2pReplicationCheck_ = new QCheckBox(tr("Enable P2P replication (client)"));
    p2pReplicationCheck_->setToolTip(tr("Replicate database from other peers"));
    p2pLayout->addRow(p2pReplicationCheck_);

    p2pReplicationServerCheck_ = new QCheckBox(tr("Enable P2P replication server"));
    p2pReplicationServerCheck_->setToolTip(tr("Serve database to other peers"));
    p2pLayout->addRow(p2pReplicationServerCheck_);

    databaseSharingCheck_ = new QCheckBox(tr("Let peers download my whole database"));
    databaseSharingCheck_->setToolTip(
        tr("Answer a peer's request for a full copy of your index. Producing one exports the "
           "whole database and uploads it, which can take a long time and a lot of bandwidth. "
           "Turning this off also hides you from other users' peer lists."));
    p2pLayout->addRow(databaseSharingCheck_);

    tabLayout->addWidget(p2pGroup);

    // --- REST API ---
    QGroupBox* apiGroup = new QGroupBox(tr("REST API"));
    QFormLayout* apiLayout = new QFormLayout(apiGroup);

    restApiCheck_ = new QCheckBox(tr("Enable REST API server"));
    apiLayout->addRow(restApiCheck_);

    tabLayout->addWidget(apiGroup);

    tabLayout->addStretch();
    return wrapInScrollArea(tab);
}

// =============================================================================
// Tab: Indexer
// =============================================================================
QWidget* SettingsDialog::createIndexerTab()
{
    QWidget* tab = new QWidget();
    QVBoxLayout* tabLayout = new QVBoxLayout(tab);
    tabLayout->setSpacing(16);
    tabLayout->setContentsMargins(12, 12, 12, 12);

    // --- DHT Indexer ---
    QGroupBox* indexerGroup = new QGroupBox(tr("DHT Indexer"));
    QFormLayout* indexerLayout = new QFormLayout(indexerGroup);
    indexerLayout->setSpacing(10);

    indexerCheck_ = new QCheckBox(tr("Enable DHT indexer"));
    indexerCheck_->setToolTip(tr("Crawl the DHT network for new torrents"));
    indexerLayout->addRow(indexerCheck_);

    trackersCheck_ = new QCheckBox(tr("Enable tracker checking"));
    trackersCheck_->setToolTip(tr("Check trackers for seeders/leechers info"));
    indexerLayout->addRow(trackersCheck_);

    tabLayout->addWidget(indexerGroup);

    // --- Spider Performance ---
    QGroupBox* perfGroup = new QGroupBox(tr("Spider Performance"));
    QFormLayout* perfLayout = new QFormLayout(perfGroup);
    perfLayout->setSpacing(10);

    walkIntervalSpin_ = new QSpinBox();
    walkIntervalSpin_->setRange(1, 500);
    walkIntervalSpin_->setToolTip(tr("Interval between DHT walks (lower = faster, more CPU)"));
    perfLayout->addRow(tr("Walk interval:"), walkIntervalSpin_);

    crawlerConcurrencySpin_ = new QSpinBox();
    crawlerConcurrencySpin_->setObjectName("crawlerMetadataConcurrency");
    crawlerConcurrencySpin_->setRange(1, rats::common::CrawlerLimits::MaxConcurrency);
    crawlerConcurrencySpin_->setToolTip(tr("Maximum concurrent crawler metadata requests. Capacity limits may keep the actual count lower. Changes apply after Save without restarting."));
    perfLayout->addRow(tr("Metadata concurrency limit:"), crawlerConcurrencySpin_);
    crawlerActivityLabel_ = new QLabel();
    crawlerActivityLabel_->setObjectName("crawlerMetadataActivity");
    crawlerActivityLabel_->setWordWrap(true);
    perfLayout->addRow(tr("Current activity:"), crawlerActivityLabel_);
    const auto refreshActivity = [this] {
        const auto* crawler = app_ ? app_->crawler() : nullptr;
        crawlerActivityLabel_->setText(crawler
            ? tr("%1 active; saved limit %2%3").arg(crawler->activeMetadataFetches()).arg(crawler->metadataConcurrency())
                .arg(crawler->metadataBackpressured() ? tr("; waiting for capacity") : QString())
            : tr("Stopped"));
    };
    if (app_ && app_->crawler()) connect(app_->crawler(), &rats::net::Crawler::metadataActivityChanged, this, refreshActivity);
    refreshActivity();
    crawlerPendingSpin_ = new QSpinBox();
    crawlerPendingSpin_->setObjectName("crawlerPendingLimit");
    crawlerPendingSpin_->setRange(1, rats::common::CrawlerLimits::Pending);
    crawlerPendingSpin_->setToolTip(tr("Shared task limit for lookups, result reservations, writes and retries. The 64 MiB estimated byte budget also applies."));
    perfLayout->addRow(tr("Pending task limit:"), crawlerPendingSpin_);
    crawlerBatchSpin_ = new QSpinBox();
    crawlerBatchSpin_->setObjectName("crawlerBatchSize");
    crawlerBatchSpin_->setRange(1, 100);
    crawlerBatchSpin_->setToolTip(tr("Maximum items per database batch. Sparse batches still flush after 100 ms."));
    perfLayout->addRow(tr("Database batch size:"), crawlerBatchSpin_);

    QLabel* perfHint = new QLabel(tr("* Lower walk interval = faster indexing but higher CPU usage"));
    perfHint->setObjectName("hintLabel");
    perfHint->setWordWrap(true);
    perfLayout->addRow(perfHint);

    tabLayout->addWidget(perfGroup);

    tabLayout->addStretch();
    return wrapInScrollArea(tab);
}

// =============================================================================
// Tab: Filters
// =============================================================================
QWidget* SettingsDialog::createFiltersTab()
{
    QWidget* tab = new QWidget();
    QVBoxLayout* tabLayout = new QVBoxLayout(tab);
    tabLayout->setSpacing(16);
    tabLayout->setContentsMargins(12, 12, 12, 12);

    // --- Name Filters ---
    QGroupBox* nameGroup = new QGroupBox(tr("Name Filters"));
    QVBoxLayout* nameLayout = new QVBoxLayout(nameGroup);
    nameLayout->setSpacing(10);

    QHBoxLayout* regexRow = new QHBoxLayout();
    QLabel* regexLabel = new QLabel(tr("Name filter (regex):"));
    regexEdit_ = new QLineEdit();
    regexEdit_->setPlaceholderText(tr("Regular expression pattern..."));

    QComboBox* regexExamples = new QComboBox();
    regexExamples->addItem(tr("Examples..."), "");
    regexExamples->addItem(tr("Russian + English only"),
        QString::fromUtf8(R"(^[А-Яа-я0-9A-Za-z.!@?#"$%&:;() *\+,\/;\-=[\\\]\^_{|}<>\u0400-\u04FF]+$)"));
    regexExamples->addItem(tr("English only"), R"(^[0-9A-Za-z.!@?#"$%&:;() *\+,\/;\-=[\\\]\^_{|}<>]+$)");
    regexExamples->addItem(tr("Ignore 'badword'"), R"(^((?!badword).)*$)");

    connect(regexExamples, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, regexExamples](int index) {
        QString example = regexExamples->itemData(index).toString();
        if (!example.isEmpty()) {
            regexEdit_->setText(example);
        }
    });

    regexRow->addWidget(regexLabel);
    regexRow->addWidget(regexEdit_, 1);
    regexRow->addWidget(regexExamples);
    nameLayout->addLayout(regexRow);

    regexNegativeCheck_ = new QCheckBox(tr("Negative regex filter (reject matches)"));
    regexNegativeCheck_->setToolTip(tr("When enabled, torrents matching the regex will be rejected"));
    nameLayout->addWidget(regexNegativeCheck_);

    QLabel* regexHint = new QLabel(tr("* Empty string = Disabled"));
    regexHint->setObjectName("hintLabel");
    nameLayout->addWidget(regexHint);

    adultFilterCheck_ = new QCheckBox(tr("Adult content filter (ignore XXX content)"));
    adultFilterCheck_->setToolTip(tr("When enabled, adult content will be filtered out"));
    nameLayout->addWidget(adultFilterCheck_);

    tabLayout->addWidget(nameGroup);

    // --- Size & File Limits ---
    QGroupBox* sizeGroup = new QGroupBox(tr("Size && File Limits"));
    QVBoxLayout* sizeGroupLayout = new QVBoxLayout(sizeGroup);
    sizeGroupLayout->setSpacing(10);

    // Max files per torrent
    QHBoxLayout* maxFilesRow = new QHBoxLayout();
    QLabel* maxFilesLabel = new QLabel(tr("Max files per torrent:"));
    maxFilesLabel->setToolTip(tr("Maximum number of files in a torrent (0 = disabled)"));

    maxFilesSlider_ = new QSlider(Qt::Horizontal);
    maxFilesSlider_->setRange(0, 50000);

    maxFilesSpin_ = new QSpinBox();
    maxFilesSpin_->setRange(0, 50000);
    maxFilesSpin_->setMinimumWidth(80);

    connect(maxFilesSlider_, &QSlider::valueChanged, maxFilesSpin_, &QSpinBox::setValue);
    connect(maxFilesSpin_, QOverload<int>::of(&QSpinBox::valueChanged), maxFilesSlider_, &QSlider::setValue);

    maxFilesRow->addWidget(maxFilesLabel);
    maxFilesRow->addWidget(maxFilesSlider_, 1);
    maxFilesRow->addWidget(maxFilesSpin_);
    sizeGroupLayout->addLayout(maxFilesRow);

    QLabel* maxFilesHint = new QLabel(tr("* 0 = Disabled (no limit)"));
    maxFilesHint->setObjectName("hintLabel");
    sizeGroupLayout->addWidget(maxFilesHint);

    // Size filter
    QFormLayout* sizeLayout = new QFormLayout();
    sizeLayout->setSpacing(8);

    sizeMinSpin_ = new QSpinBox();
    sizeMinSpin_->setRange(0, 999999);
    sizeMinSpin_->setSuffix(" MB");
    sizeMinSpin_->setToolTip(tr("Minimum torrent size (0 = no minimum)"));
    sizeLayout->addRow(tr("Minimum size:"), sizeMinSpin_);

    sizeMaxSpin_ = new QSpinBox();
    sizeMaxSpin_->setRange(0, 999999);
    sizeMaxSpin_->setSuffix(" MB");
    sizeMaxSpin_->setToolTip(tr("Maximum torrent size (0 = no maximum)"));
    sizeLayout->addRow(tr("Maximum size:"), sizeMaxSpin_);

    sizeGroupLayout->addLayout(sizeLayout);
    tabLayout->addWidget(sizeGroup);

    // --- Content Type Filter ---
    QGroupBox* contentTypeBox = new QGroupBox(tr("Content Type Filter"));
    QVBoxLayout* contentTypeLayout = new QVBoxLayout(contentTypeBox);

    QLabel* contentTypeHint = new QLabel(tr("Uncheck to disable specific content types:"));
    contentTypeHint->setObjectName("hintLabel");
    contentTypeLayout->addWidget(contentTypeHint);

    QGridLayout* typeGrid = new QGridLayout();
    videoCheck_ = new QCheckBox(tr("Video"));
    audioCheck_ = new QCheckBox(tr("Audio/Music"));
    picturesCheck_ = new QCheckBox(tr("Pictures/Images"));
    booksCheck_ = new QCheckBox(tr("Books"));
    appsCheck_ = new QCheckBox(tr("Apps/Games"));
    archivesCheck_ = new QCheckBox(tr("Archives"));
    discsCheck_ = new QCheckBox(tr("Discs/ISO"));

    typeGrid->addWidget(videoCheck_, 0, 0);
    typeGrid->addWidget(audioCheck_, 0, 1);
    typeGrid->addWidget(picturesCheck_, 1, 0);
    typeGrid->addWidget(booksCheck_, 1, 1);
    typeGrid->addWidget(appsCheck_, 2, 0);
    typeGrid->addWidget(archivesCheck_, 2, 1);
    typeGrid->addWidget(discsCheck_, 3, 0);

    contentTypeLayout->addLayout(typeGrid);
    tabLayout->addWidget(contentTypeBox);

    tabLayout->addStretch();
    return wrapInScrollArea(tab);
}

// =============================================================================
// Tab: Storage
// =============================================================================
QWidget* SettingsDialog::createStorageTab()
{
    QWidget* tab = new QWidget();
    QVBoxLayout* tabLayout = new QVBoxLayout(tab);
    tabLayout->setSpacing(16);
    tabLayout->setContentsMargins(12, 12, 12, 12);

    // --- Downloads ---
    QGroupBox* downloadGroup = new QGroupBox(tr("Downloads"));
    QFormLayout* downloadLayout = new QFormLayout(downloadGroup);
    downloadLayout->setSpacing(10);

    QHBoxLayout* downloadPathLayout = new QHBoxLayout();
    downloadPathEdit_ = new QLineEdit();
    QPushButton* browseDownloadBtn = new QPushButton(tr("Browse..."));
    browseDownloadBtn->setObjectName("secondaryButton");
    connect(browseDownloadBtn, &QPushButton::clicked, this, [this]() {
        QString dir
            = QFileDialog::getExistingDirectory(this, tr("Select Download Directory"), downloadPathEdit_->text());
        if (!dir.isEmpty()) {
            downloadPathEdit_->setText(dir);
        }
    });
    downloadPathLayout->addWidget(downloadPathEdit_);
    downloadPathLayout->addWidget(browseDownloadBtn);
    downloadLayout->addRow(tr("Default directory:"), downloadPathLayout);

    QLabel* downloadPathHint = new QLabel(tr("* Default location for downloaded torrents"));
    downloadPathHint->setObjectName("hintLabel");
    downloadLayout->addRow(downloadPathHint);

    tabLayout->addWidget(downloadGroup);

    // --- Data Directory ---
    QGroupBox* dbGroup = new QGroupBox(tr("Data Directory"));
    QFormLayout* dbLayout = new QFormLayout(dbGroup);
    dbLayout->setSpacing(10);

    QHBoxLayout* pathLayout = new QHBoxLayout();
    dataPathEdit_ = new QLineEdit();
    QPushButton* browseBtn = new QPushButton(tr("Browse..."));
    browseBtn->setObjectName("secondaryButton");
    connect(browseBtn, &QPushButton::clicked, this, &SettingsDialog::onBrowseDataPath);
    pathLayout->addWidget(dataPathEdit_);
    pathLayout->addWidget(browseBtn);
    dbLayout->addRow(tr("Path:"), pathLayout);

    migrateDataCheck_ = new QCheckBox(tr("Migrate current data on restart"));
    migrateDataCheck_->setChecked(true);
    dbLayout->addRow(migrateDataCheck_);
    QLabel* dataPathHint = new QLabel(tr("Migration requires an empty destination and keeps the original files as a backup."));
    dataPathHint->setObjectName("hintLabel");
    dataPathHint->setWordWrap(true);
    dbLayout->addRow(dataPathHint);

    tabLayout->addWidget(dbGroup);

    // --- Logging ---
    QGroupBox* logGroup = new QGroupBox(tr("Logging"));
    QFormLayout* logLayout = new QFormLayout(logGroup);
    logLayout->setSpacing(10);

    logMaxSizeSpin_ = new QSpinBox();
    logMaxSizeSpin_->setRange(rats::common::kMinLogMaxSizeMb, rats::common::kMaxLogMaxSizeMb);
    logMaxSizeSpin_->setSingleStep(10);
    logMaxSizeSpin_->setSuffix(tr(" MB"));
    logMaxSizeSpin_->setToolTip(tr("Total disk space used by the log file and its rotated copies"));
    logLayout->addRow(tr("Maximum log size:"), logMaxSizeSpin_);

    QLabel* logHint
        = new QLabel(tr("* Shared by %1 and its rotated copies (.1 - .%2). Once the limit is reached the oldest "
                        "entries are dropped. Applies immediately.")
                .arg(QDir::toNativeSeparators(dataDirectory_ + QStringLiteral("/rats-search.log")))
                .arg(rats::common::kLogRetentionCount));
    logHint->setObjectName("hintLabel");
    logHint->setWordWrap(true);
    logLayout->addRow(logHint);

    tabLayout->addWidget(logGroup);

    // --- Database Cleanup ---
    QGroupBox* cleanupBox = new QGroupBox(tr("Database Cleanup"));
    QVBoxLayout* cleanupLayout = new QVBoxLayout(cleanupBox);
    cleanupLayout->setSpacing(10);

    QLabel* cleanupDesc = new QLabel(tr("Check and remove torrents that don't match the current filters:"));
    cleanupDesc->setWordWrap(true);
    cleanupLayout->addWidget(cleanupDesc);

    cleanupProgress_ = new QLabel("");
    cleanupProgress_->setObjectName("cleanupProgressLabel");
    cleanupLayout->addWidget(cleanupProgress_);

    cleanupProgressBar_ = new QProgressBar();
    cleanupProgressBar_->setVisible(false);
    cleanupProgressBar_->setRange(0, 100);
    cleanupLayout->addWidget(cleanupProgressBar_);

    QHBoxLayout* cleanupBtnRow = new QHBoxLayout();
    checkTorrentsBtn_ = new QPushButton(tr("Check Torrents"));
    checkTorrentsBtn_->setToolTip(tr("Count how many torrents would be removed (dry run)"));
    checkTorrentsBtn_->setObjectName("secondaryButton");
    connect(checkTorrentsBtn_, &QPushButton::clicked, this, &SettingsDialog::onCheckTorrentsClicked);

    cleanTorrentsBtn_ = new QPushButton(tr("Clean Torrents"));
    cleanTorrentsBtn_->setToolTip(tr("Remove torrents that don't match the current filters"));
    cleanTorrentsBtn_->setObjectName("dangerButton");
    connect(cleanTorrentsBtn_, &QPushButton::clicked, this, &SettingsDialog::onCleanTorrentsClicked);

    cleanupBtnRow->addWidget(checkTorrentsBtn_);
    cleanupBtnRow->addWidget(cleanTorrentsBtn_);
    cleanupBtnRow->addStretch();
    cleanupLayout->addLayout(cleanupBtnRow);

    tabLayout->addWidget(cleanupBox);

    tabLayout->addStretch();
    return wrapInScrollArea(tab);
}

// Label the clear button with how many queries are stored, and disable it when
// there is nothing to clear.
void SettingsDialog::updateSearchHistoryButton()
{
    const int count = app_ && app_->searchHistory() ? app_->searchHistory()->size() : 0;
    clearSearchHistoryButton_->setEnabled(count > 0);
    clearSearchHistoryButton_->setText(
        count > 0 ? tr("Clear search history (%1)").arg(count) : tr("Clear search history"));
}

// =============================================================================
// Load / Save
// =============================================================================

void SettingsDialog::loadSettings()
{
    if (!app_ || !app_->config())
        return;
    auto* config_ = app_->config();
    const auto policy=app_->downloads()->globalPolicy();
    downloadPolicyRevision_=policy.revision;
    schedule_->setValue(policy.schedule);seeding_->setValue(policy.seeding);cacheMiB_->setValue(policy.cacheBytes/1048576.0);
    downloadLimit_->setValue(policy.downloadRate/1024.0);
    uploadLimit_->setValue(policy.uploadRate/1024.0);
    downloadSlots_->setValue(policy.downloadSlots); seedSlots_->setValue(policy.seedSlots);
    metadataSlots_->setValue(policy.metadataSlots); checkingSlots_->setValue(policy.checkingSlots);
    btConnections_->setValue(policy.maxConnections); taskConnections_->setValue(policy.taskConnections);

    // General
    QString currentLang = config_->language();
    for (int i = 0; i < languageCombo_->count(); ++i) {
        if (languageCombo_->itemData(i).toString() == currentLang) {
            languageCombo_->setCurrentIndex(i);
            break;
        }
    }
    darkModeCheck_->setChecked(config_->darkMode());
    const QString fontFamily = config_->uiFontFamily();
    int fontIndex = fontFamily.isEmpty() ? 0 : fontFamilyCombo_->findData(fontFamily);
    if (fontIndex < 0) {
        // Not installed here (a config carried over from another machine): keep
        // it selectable, so saving the dialog does not quietly drop it.
        fontFamilyCombo_->addItem(fontFamily, fontFamily);
        fontIndex = fontFamilyCombo_->count() - 1;
    }
    fontFamilyCombo_->setCurrentIndex(fontIndex);
    fontScaleSpin_->setValue(config_->uiFontScale());
    autoStartCheck_->setChecked(AutoStartManager::isEnabled());
    startMinimizedCheck_->setChecked(config_->startMinimized());
    minimizeToTrayCheck_->setChecked(config_->trayOnMinimize());
    closeToTrayCheck_->setChecked(config_->trayOnClose());
    confirmExitCheck_->setChecked(config_->confirmExit());
    checkUpdatesCheck_->setChecked(config_->checkUpdatesOnStartup());
    searchHistoryCheck_->setChecked(config_->searchHistoryEnabled());
    updateSearchHistoryButton();

    // Network
    p2pPortSpin_->setValue(config_->p2pPort());
    dhtPortSpin_->setValue(config_->dhtPort());
    httpPortSpin_->setValue(config_->httpPort());
    restApiCheck_->setChecked(config_->restApiEnabled());
    p2pConnectionsSpin_->setValue(config_->p2pConnections());
    p2pReplicationCheck_->setChecked(config_->p2pReplication());
    p2pReplicationServerCheck_->setChecked(config_->p2pReplicationServer());
    databaseSharingCheck_->setChecked(config_->databaseSharing());

    // Indexer
    indexerCheck_->setChecked(config_->indexerEnabled());
    trackersCheck_->setChecked(config_->trackersEnabled());
    walkIntervalSpin_->setValue(config_->spiderWalkInterval());
    crawlerConcurrencySpin_->setValue(config_->spiderMetadataConcurrency());
    crawlerPendingSpin_->setValue(config_->spiderPendingLimit());
    crawlerBatchSpin_->setValue(config_->spiderBatchSize());

    // Filters
    maxFilesSpin_->setValue(config_->filtersMaxFiles());
    maxFilesSlider_->setValue(config_->filtersMaxFiles());
    regexEdit_->setText(config_->filtersNamingRegExp());
    regexNegativeCheck_->setChecked(config_->filtersNamingRegExpNegative());
    adultFilterCheck_->setChecked(config_->filtersAdultFilter());
    sizeMinSpin_->setValue(static_cast<int>(config_->filtersSizeMin() / (1024 * 1024)));
    sizeMaxSpin_->setValue(static_cast<int>(config_->filtersSizeMax() / (1024 * 1024)));

    // Content types
    QString currentContentTypes = config_->filtersContentType();
    QStringList enabledTypes = currentContentTypes.isEmpty()
        ? QStringList { "video", "audio", "pictures", "books", "application", "archive", "disc" }
        : currentContentTypes.split(",", Qt::SkipEmptyParts);

    videoCheck_->setChecked(enabledTypes.contains("video"));
    audioCheck_->setChecked(enabledTypes.contains("audio"));
    picturesCheck_->setChecked(enabledTypes.contains("pictures"));
    booksCheck_->setChecked(enabledTypes.contains("books"));
    appsCheck_->setChecked(enabledTypes.contains("application"));
    archivesCheck_->setChecked(enabledTypes.contains("archive"));
    discsCheck_->setChecked(enabledTypes.contains("disc"));

    // Storage
    downloadPathEdit_->setText(config_->downloadPath());
    logMaxSizeSpin_->setValue(config_->logMaxSizeMb());

    const auto pendingPath = rats::app::relocation::pending(dataDirectory_).destination;
    QSettings settings("RatsSearch", "RatsSearch");
    const auto savedPath = settings.value("dataDirectory").toString();
    const auto displayed = pendingPath.isEmpty() ? (savedPath.isEmpty() ? dataDirectory_ : savedPath) : pendingPath;
    dataPathEdit_->setText(displayed);
    migrateDataCheck_->setChecked(!pendingPath.isEmpty() || displayed == dataDirectory_);
}

void SettingsDialog::saveSettings()
{
    if (!app_ || !app_->config())
        return;
    auto* config_ = app_->config();

    // Track what changed for restart notification
    int oldP2pPort = config_->p2pPort();
    int oldDhtPort = config_->dhtPort();
    int oldHttpPort = config_->httpPort();
    bool oldRestApi = config_->restApiEnabled();

    // Save General
    config_->setLanguage(languageCombo_->currentData().toString());
    config_->setDarkMode(darkModeCheck_->isChecked());
    config_->setUiFontFamily(fontFamilyCombo_->currentData().toString());
    config_->setUiFontScale(fontScaleSpin_->value());
    config_->setStartMinimized(startMinimizedCheck_->isChecked());
    config_->setTrayOnMinimize(minimizeToTrayCheck_->isChecked());
    config_->setTrayOnClose(closeToTrayCheck_->isChecked());
    config_->setConfirmExit(confirmExitCheck_->isChecked());
    config_->setCheckUpdatesOnStartup(checkUpdatesCheck_->isChecked());
    config_->setSearchHistoryEnabled(searchHistoryCheck_->isChecked());

    // Autostart lives in the OS (registry / .desktop / launch agent), which is its
    // only source of truth — loadSettings() reads it back from AutoStartManager.
    bool autoStartEnabled = autoStartCheck_->isChecked();
    if (autoStartEnabled != AutoStartManager::isEnabled()) {
        if (!AutoStartManager::setEnabled(autoStartEnabled)) {
            qWarning() << "Failed to" << (autoStartEnabled ? "enable" : "disable") << "autostart";
        }
    }

    // Save Network
    config_->setP2pPort(p2pPortSpin_->value());
    config_->setDhtPort(dhtPortSpin_->value());
    config_->setHttpPort(httpPortSpin_->value());
    config_->setRestApiEnabled(restApiCheck_->isChecked());
    config_->setP2pConnections(p2pConnectionsSpin_->value());
    config_->setP2pReplication(p2pReplicationCheck_->isChecked());
    config_->setP2pReplicationServer(p2pReplicationServerCheck_->isChecked());
    config_->setDatabaseSharing(databaseSharingCheck_->isChecked());

    // Save Indexer
    config_->setIndexerEnabled(indexerCheck_->isChecked());
    config_->setTrackersEnabled(trackersCheck_->isChecked());
    config_->setSpiderWalkInterval(walkIntervalSpin_->value());
    config_->setSpiderPendingLimit(crawlerPendingSpin_->value());
    config_->setSpiderBatchSize(crawlerBatchSpin_->value());
    config_->setSpiderMetadataConcurrency(crawlerConcurrencySpin_->value());

    // Save Filters
    config_->setFiltersMaxFiles(maxFilesSpin_->value());
    config_->setFiltersNamingRegExp(regexEdit_->text());
    config_->setFiltersNamingRegExpNegative(regexNegativeCheck_->isChecked());
    config_->setFiltersAdultFilter(adultFilterCheck_->isChecked());
    config_->setFiltersSizeMin(static_cast<qint64>(sizeMinSpin_->value()) * 1024 * 1024);
    config_->setFiltersSizeMax(static_cast<qint64>(sizeMaxSpin_->value()) * 1024 * 1024);

    config_->setFiltersContentType(selectedContentTypes());

    config_->setLogMaxSizeMb(logMaxSizeSpin_->value());

    // Save Download Path
    QString newDownloadPath = downloadPathEdit_->text();
    if (!newDownloadPath.isEmpty()) {
        config_->setDownloadPath(newDownloadPath);
    }

    // Check if restart needed (only for settings that can't be applied at
    // runtime)
    needsRestart_ = (p2pPortSpin_->value() != oldP2pPort) || (dhtPortSpin_->value() != oldDhtPort)
        || (httpPortSpin_->value() != oldHttpPort) || (restApiCheck_->isChecked() != oldRestApi)
        || !rats::app::relocation::pending(dataDirectory_).destination.isEmpty();

    // Save config to file
    config_->save();
}

QString SettingsDialog::selectedContentTypes() const
{
    QStringList contentTypes;
    if (videoCheck_->isChecked())
        contentTypes << "video";
    if (audioCheck_->isChecked())
        contentTypes << "audio";
    if (picturesCheck_->isChecked())
        contentTypes << "pictures";
    if (booksCheck_->isChecked())
        contentTypes << "books";
    if (appsCheck_->isChecked())
        contentTypes << "application";
    if (archivesCheck_->isChecked())
        contentTypes << "archive";
    if (discsCheck_->isChecked())
        contentTypes << "disc";

    // Every box ticked means "no type filter" — the same as none at all.
    return contentTypes.size() == 7 ? QString() : contentTypes.join(QLatin1Char(','));
}

QJsonObject SettingsDialog::currentFilters() const
{
    return QJsonObject { { "maxFiles", maxFilesSpin_->value() }, { "namingRegExp", regexEdit_->text() },
        { "namingRegExpNegative", regexNegativeCheck_->isChecked() }, { "adultFilter", adultFilterCheck_->isChecked() },
        { "sizeMin", static_cast<double>(static_cast<qint64>(sizeMinSpin_->value()) * 1024 * 1024) },
        { "sizeMax", static_cast<double>(static_cast<qint64>(sizeMaxSpin_->value()) * 1024 * 1024) },
        { "contentType", selectedContentTypes() } };
}

// =============================================================================
// Slots
// =============================================================================

void SettingsDialog::runCleanup(bool dryRun)
{
    cleanupProgressBar_->setVisible(false);
    cleanupProgress_->setObjectName("cleanupProgressLabel");
    cleanupProgress_->setText(dryRun ? tr("Checking torrents against filters...") : tr("Cleaning torrents..."));
    QApplication::processEvents(); // paint the status before the synchronous
                                   // sweep

    app_->api()->call(QStringLiteral("torrent.cleanup"), { { "dryRun", dryRun }, { "filters", currentFilters() } },
        [this, dryRun](const rats::Result& r) {
            if (!r.ok()) {
                cleanupProgress_->setText(tr("Cleanup failed: %1").arg(r.error()));
                return;
            }
            const QJsonObject data = r.data().toObject();
            const int matched = data["matched"].toInt();
            const int scanned = data["scanned"].toInt();
            cleanupProgress_->setText(dryRun
                    ? tr("%n of %1 torrent(s) don't match the current filters.", nullptr, matched).arg(scanned)
                    : tr("Removed %n torrent(s) that didn't match the filters.", nullptr, matched));
        });
}

void SettingsDialog::onCheckTorrentsClicked()
{
    runCleanup(/*dryRun*/ true);
}

void SettingsDialog::onCleanTorrentsClicked()
{
    runCleanup(/*dryRun*/ false);
}

void SettingsDialog::onBrowseDataPath()
{
    QString dir = QFileDialog::getExistingDirectory(this, tr("Select Data Directory"), dataDirectory_);
    if (!dir.isEmpty()) {
        dataPathEdit_->setText(dir);
    }
}

void SettingsDialog::onAccepted()
{
    namespace relocation = rats::app::relocation;
    const QString entered = dataPathEdit_->text().trimmed();
    const QString previous = relocation::pending(dataDirectory_).destination;
    const bool stay = entered.isEmpty() || QDir::cleanPath(entered) == QDir::cleanPath(dataDirectory_)
        || (QFileInfo(entered).exists() && QFileInfo(entered).canonicalFilePath() == QFileInfo(dataDirectory_).canonicalFilePath());
    const bool migrate = !stay && migrateDataCheck_->isChecked();
    if (migrate && entered != previous) {
        const auto error = relocation::validate(dataDirectory_, entered);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, tr("Data Directory"), error);
            return;
        }
    }
    auto p=app_->downloads()->globalPolicy();
    p.downloadRate=qRound64(downloadLimit_->value()*1024); p.uploadRate=qRound64(uploadLimit_->value()*1024);
    p.downloadSlots=downloadSlots_->value(); p.seedSlots=seedSlots_->value(); p.metadataSlots=metadataSlots_->value();
    p.checkingSlots=checkingSlots_->value(); p.maxConnections=btConnections_->value(); p.taskConnections=taskConnections_->value();
    p.schedule=schedule_->value();p.seeding=seeding_->value().value_or(rats::service::SeedingPolicy{});p.cacheBytes=qRound64(cacheMiB_->value()*1048576);
    const auto result=app_->downloads()->setGlobalPolicy(p,downloadPolicyRevision_);
    if(result.outcome!=rats::OperationResult::Outcome::Applied) {
        if(result.applied()) downloadPolicyRevision_=result.revision;
        QMessageBox::warning(this,tr("Download settings"),result.error.message); return;
    }
    downloadPolicyRevision_ = result.revision;
    auto error = migrate && entered != previous ? relocation::schedule(dataDirectory_, entered)
        : (migrate ? QString() : relocation::clear(dataDirectory_));
    if (error.isEmpty() && !migrate && !entered.isEmpty()) {
        QSettings settings("RatsSearch", "RatsSearch");
        settings.setValue("dataDirectory", stay ? dataDirectory_ : entered);
        settings.sync();
        if (settings.status() != QSettings::NoError)
            error = tr("Could not save the new data directory.");
    }
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Data Directory"), error);
        return;
    }
    saveSettings();
    needsRestart_ = needsRestart_ || !stay;
    accept();
}

QWidget* SettingsDialog::createDownloadsTab() {
    auto* tab=new QWidget; auto* layout=new QFormLayout(tab);
    auto* texts=new DownloadTextBinder(tab,"SettingsDialog");auto tr=[texts](const char* source){return texts->text(source);};
    auto rate=[&] { auto* box=new QDoubleSpinBox(tab); box->setDecimals(6); box->setRange(0,8796093022207.0);
        box->setSuffix(" KiB/s"); box->setSpecialValueText(tr("Unlimited")); return box; };
    auto count=[&](int max) { auto* box=new QSpinBox(tab); box->setRange(1,max); return box; };
    downloadLimit_=rate(); uploadLimit_=rate();
    layout->addRow(tr("Global download limit"),downloadLimit_); layout->addRow(tr("Global upload limit"),uploadLimit_);
    downloadSlots_=count(1000); seedSlots_=count(1000); metadataSlots_=count(1000); checkingSlots_=count(1000);
    btConnections_=count(10000); taskConnections_=count(10000);
    layout->addRow(tr("Active downloads"),downloadSlots_); layout->addRow(tr("Active seeds"),seedSlots_);
    layout->addRow(tr("Metadata tasks"),metadataSlots_); layout->addRow(tr("File checks"),checkingSlots_);
    layout->addRow(tr("BT connections total"),btConnections_); layout->addRow(tr("BT connections per task"),taskConnections_);
    cacheMiB_=new QDoubleSpinBox(tab);cacheMiB_->setDecimals(9);cacheMiB_->setRange(0,256);cacheMiB_->setSuffix(" MiB");cacheMiB_->setSpecialValueText(tr("Disabled"));layout->addRow(tr("Shared read cache"),cacheMiB_);
    schedule_=new DownloadScheduleWidget(tab);layout->addRow(schedule_);seeding_=new SeedingPolicyWidget(false,tab);layout->addRow(tr("Default seeding conditions"),seeding_);
    auto* note=new QLabel(tr("Limits apply to BitTorrent content over TCP and uTP. Protocol and metadata traffic use a separate 64 KiB/s budget. Cache is opt-in; seeding conditions never delete files."));
    note->setWordWrap(true); layout->addRow(note);texts->bind(); return wrapInScrollArea(tab);
}
