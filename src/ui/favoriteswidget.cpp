#include "favoriteswidget.h"
#include "app/application.h"
#include "app/favorites_store.h"
#include "app/favorites_xlsx.h"
#include "services/search_service.h"
#include "searchresultmodel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

using rats::app::FavoritesStore;

FavoritesWidget::FavoritesWidget(QWidget* parent) : TorrentTableWidget(parent)
{
    tableView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    setupControls();
    connect(tableView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &FavoritesWidget::updateActions);
}

std::optional<QString> FavoritesWidget::chooseFolder(QWidget* parent, const QStringList& folders, const QString& title)
{
    if (folders.isEmpty())
        return QString();

    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto* layout = new QFormLayout(&dialog);
    auto* choice = new QComboBox(&dialog);
    choice->addItem(tr("Unfiled"), QString());
    for (const auto& folder : folders)
        choice->addItem(folder, folder);
    layout->addRow(tr("Destination folder"), choice);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    return choice->currentData().toString();
}

void FavoritesWidget::setupControls()
{
    auto* header = new QWidget(this);
    header->setObjectName("filterRow");
    auto* row = new QHBoxLayout(header);
    row->setContentsMargins(12, 8, 12, 8);
    folderBox_ = new QComboBox(header);
    folderBox_->setAccessibleName(tr("Favorites folder"));
    folderBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    folderBox_->setMinimumContentsLength(16);
    row->addWidget(folderBox_);
    connect(folderBox_, &QComboBox::currentIndexChanged, this, &FavoritesWidget::refresh);

    auto* folders = new QToolButton(header);
    folders->setText(tr("Manage folders"));
    folders->setPopupMode(QToolButton::InstantPopup);
    auto* folderMenu = new QMenu(folders);
    folderMenu->addAction(tr("New folder…"), this, [this] { editFolder(false); });
    renameFolderAction_ = folderMenu->addAction(tr("Rename folder…"), this, [this] { editFolder(true); });
    deleteFolderAction_ = folderMenu->addAction(tr("Delete folder…"), this, &FavoritesWidget::removeFolder);
    folders->setMenu(folderMenu);
    row->addWidget(folders);
    row->addStretch();

    statusLabel_ = new QLabel(header);
    statusLabel_->setObjectName("statusLabel");
    row->addWidget(statusLabel_);
    moveButton_ = new QPushButton(tr("Move selected…"), header);
    connect(moveButton_, &QPushButton::clicked, this, &FavoritesWidget::moveSelected);
    row->addWidget(moveButton_);
    removeButton_ = new QPushButton(tr("Remove selected"), header);
    removeButton_->setObjectName("dangerButton");
    connect(removeButton_, &QPushButton::clicked, this, &FavoritesWidget::removeSelected);
    row->addWidget(removeButton_);

    exportButton_ = new QToolButton(header);
    exportButton_->setText(tr("Export favorites"));
    exportButton_->setPopupMode(QToolButton::InstantPopup);
    auto* exportMenu = new QMenu(exportButton_);
    exportSelectedAction_ = exportMenu->addAction(tr("Export selected…"), this,
        [this] { exportFavorites(ExportScope::Selected); });
    exportFolderAction_ = exportMenu->addAction(tr("Export current folder…"), this,
        [this] { exportFavorites(ExportScope::Folder); });
    exportMenu->addAction(tr("Export all favorites…"), this,
        [this] { exportFavorites(ExportScope::All); });
    exportButton_->setMenu(exportMenu);
    row->addWidget(exportButton_);

    mainLayout_->insertWidget(0, header);
    emptyLabel_ = new QLabel(this);
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setObjectName("emptyStateLabel");
    emptyLabel_->setWordWrap(true);
    mainLayout_->insertWidget(1, emptyLabel_);
    updateActions();
}

void FavoritesWidget::onApplicationSet()
{
    if (!app_ || !app_->favorites())
        return;
    connect(app_->favorites(), &FavoritesStore::favoritesChanged, this, &FavoritesWidget::refresh);
    refresh();
}

void FavoritesWidget::refresh()
{
    if (!app_ || !app_->favorites())
        return;
    const auto selected = selectedHashes();
    const auto currentHash = selectedTorrent().hash;
    const bool showAll = folderBox_->currentIndex() <= 0;
    const auto folder = folderBox_->currentData().toString();
    const auto folders = app_->favorites()->folders();
    if (folderBox_->count() == 0 || folders != displayedFolders_) {
        const QSignalBlocker block(folderBox_);
        displayedFolders_ = folders;
        folderBox_->clear();
        folderBox_->addItem(tr("All favorites"));
        folderBox_->addItem(tr("Unfiled"), QString());
        for (const auto& name : folders)
            folderBox_->addItem(name, name);
        const int restored = folder.isEmpty() ? 1 : folderBox_->findData(folder);
        folderBox_->setCurrentIndex(showAll || restored < 0 ? 0 : restored);
    }

    QVector<rats::domain::Torrent> torrents;
    const bool all = folderBox_->currentIndex() == 0;
    const QString visibleFolder = folderBox_->currentData().toString();
    for (const auto& entry : app_->favorites()->favorites()) {
        if (all || entry.folder == visibleFolder)
            torrents.append(entry.torrent);
    }
    setTorrents(torrents);
    QItemSelection selection;
    QModelIndex current;
    if (!selected.isEmpty()) {
        for (int index = 0; index < model_->rowCount(); ++index) {
            const auto cell = model_->index(index, 0);
            const auto hash = cell.data(SearchResultModel::HashRole).toString();
            if (selected.contains(hash))
                selection.select(cell, cell);
            if (hash == currentHash)
                current = cell;
        }
        tableView_->selectionModel()->select(selection, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        if (!current.isValid() && !selection.isEmpty())
            current = selection.first().topLeft();
        if (current.isValid())
            tableView_->selectionModel()->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
    }
    if (!current.isValid())
        emit torrentSelected({});

    const bool empty = torrents.isEmpty();
    emptyLabel_->setText(all ? tr("No favorites yet.") : tr("This folder has no favorites."));
    emptyLabel_->setVisible(empty);
    tableView_->setVisible(!empty);
    statusLabel_->setText(tr("%n favorite(s)", nullptr, static_cast<int>(torrents.size())));
    updateActions();
}

QSet<QString> FavoritesWidget::selectedHashes() const
{
    QSet<QString> hashes;
    for (const auto& index : tableView_->selectionModel()->selectedRows())
        hashes.insert(index.data(SearchResultModel::HashRole).toString());
    return hashes;
}

void FavoritesWidget::updateActions()
{
    const bool selected = tableView_->selectionModel()->hasSelection();
    auto* store = app_ ? app_->favorites() : nullptr;
    moveButton_->setEnabled(selected && store && !store->folders().isEmpty());
    removeButton_->setEnabled(selected);
    renameFolderAction_->setEnabled(folderBox_->currentIndex() > 1);
    deleteFolderAction_->setEnabled(folderBox_->currentIndex() > 1);
    exportButton_->setEnabled(!exportInProgress_ && store && !store->favorites().isEmpty());
    exportSelectedAction_->setEnabled(selected);
    exportFolderAction_->setEnabled(folderBox_->currentIndex() > 0 && model_->rowCount() > 0);
}

QMenu* FavoritesWidget::buildContextMenu(const rats::domain::Torrent& torrent)
{
    auto* menu = TorrentTableWidget::buildContextMenu(torrent);
    menu->addSeparator();
    menu->addAction(tr("Move selected…"), this, &FavoritesWidget::moveSelected)
        ->setEnabled(moveButton_->isEnabled());
    menu->addAction(tr("Remove selected"), this, &FavoritesWidget::removeSelected);
    menu->addAction(tr("Export selected…"), this, [this] { exportFavorites(ExportScope::Selected); })
        ->setEnabled(!exportInProgress_);
    return menu;
}

void FavoritesWidget::removeSelected()
{
    const auto hashes = selectedHashes();
    if (hashes.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Remove favorites"),
            tr("Remove %n selected torrent(s) from Favorites?", nullptr, static_cast<int>(hashes.size())))
        == QMessageBox::Yes)
        app_->favorites()->removeMany(hashes);
}

void FavoritesWidget::moveSelected()
{
    const auto hashes = selectedHashes();
    if (hashes.isEmpty())
        return;
    const auto folder = chooseFolder(this, app_->favorites()->folders(), tr("Move favorites"));
    if (folder)
        app_->favorites()->moveMany(hashes, *folder);
}

void FavoritesWidget::editFolder(bool rename)
{
    if (!app_ || !app_->favorites())
        return;
    auto* store = app_->favorites();
    const QString current = rename ? folderBox_->currentData().toString() : QString();
    bool accepted = false;
    const QString name = QInputDialog::getText(this, rename ? tr("Rename folder") : tr("New folder"),
        tr("Folder name"), QLineEdit::Normal, current, &accepted).trimmed();
    if (!accepted || (rename && current == name))
        return;
    auto folders = store->folders();
    if (rename)
        folders.removeOne(current);
    if (name.isEmpty() || folders.contains(name, Qt::CaseInsensitive)) {
        QMessageBox::warning(this, tr("Favorites folder"), tr("Enter a non-empty, unique folder name."));
        return;
    }
    if (rename ? store->renameFolder(current, name) : store->createFolder(name))
        folderBox_->setCurrentIndex(folderBox_->findData(name));
}

void FavoritesWidget::removeFolder()
{
    const QString folder = folderBox_->currentData().toString();
    if (folder.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Delete folder"),
            tr("Delete the folder “%1” and move its favorites to Unfiled?").arg(folder)) == QMessageBox::Yes
        && app_->favorites()->removeFolder(folder))
        folderBox_->setCurrentIndex(1);
}

void FavoritesWidget::exportFavorites(ExportScope scope)
{
    if (!app_ || !app_->favorites() || exportInProgress_)
        return;
    const auto hashes = selectedHashes();
    const auto folder = folderBox_->currentData().toString();
    auto entries = app_->favorites()->favorites();
    entries.removeIf([&](const FavoritesStore::Entry& entry) {
        return (scope == ExportScope::Selected && !hashes.contains(entry.torrent.hash))
            || (scope == ExportScope::Folder && entry.folder != folder);
    });
    if (entries.isEmpty())
        return;

    QDialog options(this);
    options.setWindowTitle(tr("Export favorites"));
    auto* layout = new QVBoxLayout(&options);
    auto* format = new QComboBox(&options);
    format->setAccessibleName(tr("Export format"));
    format->addItem(tr("JSON"), QStringLiteral("json"));
    format->addItem(tr("Excel workbook"), QStringLiteral("xlsx"));
    layout->addWidget(format);
    auto* files = new QCheckBox(tr("Include file lists when available"), &options);
    auto* date = new QCheckBox(tr("Include date added to Favorites"), &options);
    layout->addWidget(files);
    layout->addWidget(date);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &options);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &options, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &options, &QDialog::reject);
    if (options.exec() != QDialog::Accepted)
        return;
    const FavoritesStore::ExportOptions settings{ files->isChecked(), date->isChecked() };
    const QString suffix = format->currentData().toString();
    const bool excel = suffix == QLatin1String("xlsx");

    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath(QStringLiteral("favorites-export.") + suffix);
    QFileDialog save(this, tr("Export favorites"), suggested,
        excel ? tr("Excel workbooks (*.xlsx)") : tr("JSON files (*.json)"));
    save.setAcceptMode(QFileDialog::AcceptSave);
    save.setDefaultSuffix(suffix);
    if (save.exec() != QDialog::Accepted || save.selectedFiles().isEmpty())
        return;
    const auto path = save.selectedFiles().first();
    if (QFileInfo(path).suffix().compare(suffix, Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(this, tr("Export favorites"), tr("Use the .%1 filename extension for this format.").arg(suffix));
        return;
    }
    const QString storePath = QDir(app_->options().dataDirectory).filePath(QStringLiteral("favorites.json"));
    const auto canonicalStore = QFileInfo(storePath).canonicalFilePath();
    if (!canonicalStore.isEmpty() && QFileInfo(QFileInfo(path).canonicalFilePath()) == QFileInfo(canonicalStore)) {
        QMessageBox::warning(this, tr("Export favorites"), tr("Choose a different file from the live favorites file."));
        return;
    }

    exportInProgress_ = true;
    updateActions();
    const bool accepted = app_->search()->exportAsync(this,
        [path, entries = std::move(entries), settings, excel](rats::data::TorrentRepository& repository,
            const std::function<bool()>& cancelled) {
            const auto lookup = [&](const QString& hash) {
                const auto torrent = repository.get(hash, true);
                if (!torrent || torrent->fileList.size() < torrent->files)
                    return QVector<rats::domain::File>();
                return torrent->fileList;
            };
            return excel ? rats::app::exportFavoritesXlsx(path, entries, settings, lookup, cancelled)
                         : FavoritesStore::exportToFile(path, entries, settings, lookup, cancelled);
        },
        [this, path](const QString& error) {
            exportInProgress_ = false;
            updateActions();
            if (error.isEmpty())
                QMessageBox::information(this, tr("Export favorites"), tr("Favorites exported to %1").arg(path));
            else
                QMessageBox::warning(this, tr("Export favorites"), error);
        });
    if (!accepted) {
        exportInProgress_ = false;
        updateActions();
        QMessageBox::warning(this, tr("Export favorites"), tr("Search is busy. Try again."));
    }
}
