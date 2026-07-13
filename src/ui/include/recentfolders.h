/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef RECENTFOLDERS_H
#define RECENTFOLDERS_H

#include <QString>
#include <QStringList>

#include "persistable.h"

// Manage the list of recently opened folders (mirrors RecentFiles).
// Persisted under its own "RecentFolders" QSettings group so the existing
// RecentFiles format is untouched.
class RecentFolders final : public Persistable<RecentFolders, session_settings> {
  public:
    static const char* persistableName()
    {
        return "RecentFolders";
    }

    void addRecent( const QString& text );
    void removeRecent( const QString& text );
    void removeAll();
    int getNumberItemsToShow() const;
    int foldersHistoryMaxItems() const;
    void setFoldersHistoryMaxItems( const int recentFoldersItems );

    // Returns a list of recent folders (latest opened first)
    QStringList recentFolders() const;

    void saveToStorage( QSettings& settings ) const;
    void retrieveFromStorage( QSettings& settings );

  private:
    static constexpr int RECENTFOLDERS_VERSION = 1;
    static constexpr int DEFAULT_MAX_ITEMS_TO_SHOW = 5;

    QStringList recentFolders_;
    int foldersHistoryMaxItemsToShow_ = DEFAULT_MAX_ITEMS_TO_SHOW;
};

#endif // RECENTFOLDERS_H
