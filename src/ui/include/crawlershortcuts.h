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

#ifndef KLOGG_CRAWLERSHORTCUTS_H
#define KLOGG_CRAWLERSHORTCUTS_H

#include <functional>
#include <map>

#include <QString>

class AbstractLogView;
class QComboBox;
class QShortcut;
class QWidget;
class SearchToolbar;

namespace klogg {

// The crawler widget-level shortcut family shared by CrawlerWidget (single
// file) and FolderCrawlerWidget (folder): visibility cycling, search-option
// toggles, keep-results, top-view resize and Esc refocus. Both widgets
// register the same actions through this helper so neither can silently drop
// a binding (the historical root cause of the folder-mode shortcut gaps).
// Hosts call this from their registerShortcuts() after clearing their own
// shortcuts storage, making re-registration on Configuration changes safe.
struct CrawlerShortcutHooks {
    // Visibility combo driven by the CrawlerChangeVisibility* actions.
    std::function<QComboBox*()> visibilityBox;
    // Toolbar whose option buttons the CrawlerEnable* / CrawlerKeepResults
    // actions toggle.
    std::function<SearchToolbar*()> searchToolbar;
    // Grow/shrink the top (main) view by delta steps (~10px each).
    std::function<void( int delta )> changeTopViewSize;
    // The view Esc returns keyboard focus to.
    std::function<AbstractLogView*()> activeView;
    // Also register CrawlerEnableAutoRefresh (single-file only; the folder
    // toolbar hides the auto-refresh button).
    bool includeAutoRefresh = true;
};

void registerCrawlerShortcuts( QWidget* host, std::map<QString, QShortcut*>& shortcuts,
                               const CrawlerShortcutHooks& hooks );

} // namespace klogg

#endif // KLOGG_CRAWLERSHORTCUTS_H
