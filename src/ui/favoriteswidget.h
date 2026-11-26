#ifndef FAVORITESWIDGET_H
#define FAVORITESWIDGET_H

#include "torrenttablewidget.h"
#include <QSet>
#include <QStringList>
#include <optional>

class QComboBox;
class QLabel;
class QPushButton;
class QMenu;
class QToolButton;

class FavoritesWidget : public TorrentTableWidget {
    Q_OBJECT

public:
    explicit FavoritesWidget(QWidget* parent = nullptr);
    static std::optional<QString> chooseFolder(QWidget* parent, const QStringList& folders, const QString& title);

protected:
    void refresh() override;
    void onApplicationSet() override;
    QMenu* buildContextMenu(const rats::domain::Torrent& torrent) override;

private:
    enum class ExportScope { Selected, Folder, All };
    void setupControls();
    void updateActions();
    QSet<QString> selectedHashes() const;
    void removeSelected();
    void moveSelected();
    void editFolder(bool rename);
    void removeFolder();
    void exportFavorites(ExportScope scope);

    QLabel* statusLabel_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
    QComboBox* folderBox_ = nullptr;
    QStringList displayedFolders_;
    QPushButton* moveButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QToolButton* exportButton_ = nullptr;
    QAction* renameFolderAction_ = nullptr;
    QAction* deleteFolderAction_ = nullptr;
    QAction* exportSelectedAction_ = nullptr;
    QAction* exportFolderAction_ = nullptr;
    bool exportInProgress_ = false;
};

#endif // FAVORITESWIDGET_H
