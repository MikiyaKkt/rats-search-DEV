#pragma once

#include "app/favorites_store.h"

namespace rats::app {

QString exportFavoritesXlsx(const QString& path, const QVector<FavoritesStore::Entry>& entries,
    FavoritesStore::ExportOptions options, const FavoritesStore::FileLookup& lookup,
    const std::function<bool()>& cancelled);

} // namespace rats::app
