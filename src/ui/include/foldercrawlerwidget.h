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

#ifndef FOLDERCRAWLERWIDGET_H
#define FOLDERCRAWLERWIDGET_H

#include <QString>
#include <QStringList>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>

#include "linetypes.h"
#include "regularexpressionpattern.h"
#include "abstractcrawlerwidget.h"

#include <QWidget>

#include "overview.h"

class FolderFilteredView;
class FolderSearchEngine;
class FolderSearchResults;
class LogMainView;
class LogData;
class OverviewWidget;
class QuickFindPattern;
class QSplitter;
class QLabel;
class QToolButton;
class SearchToolbar;

enum class DataStatus;

// The document widget for "Open Folder" mode: a vertical splitter with a small
// search toolbar, the (initially empty) main view on top, and the folder
// results view (grouped, collapsible per file) on the bottom.
//
//   search toolbar:  [ pattern........ ] [Search] [Stop] [Collapse all] [Expand all]   N matches
//   -----------------------------------------------------------------------
//   main view (LogMainView): empty until a result row is clicked; then shows
//                            that row's source file at the line.
//   -----------------------------------------------------------------------
//   filtered view (FolderFilteredView): grouped results, click a header to
//                                       collapse/expand, click a row to open.
//
// Search is streaming + index-less (FolderSearchEngine). Selecting a match
// opens its source file in the main view: the file is indexed on demand (the
// only place klogg's index is used in folder mode) and cached so re-selecting a
// file is instant.
class FolderCrawlerWidget : public QWidget, public AbstractCrawlerWidget {
    Q_OBJECT

  public:
    explicit FolderCrawlerWidget( QWidget* parent = nullptr );
    ~FolderCrawlerWidget() override;

    FolderCrawlerWidget( const FolderCrawlerWidget& ) = delete;
    FolderCrawlerWidget& operator=( const FolderCrawlerWidget& ) = delete;

    // Identify the folder + its (already enumerated, natural-sorted) files.
    void setFolder( const QString& folderPath, const QStringList& filePaths );

    // --- Test access / programmatic driving (no UI events needed) ---
    FolderSearchResults* folderResults() const { return folderResults_.get(); }
    FolderFilteredView* filteredView() const { return filteredView_; }
    LogMainView* mainView() const { return mainView_; }
    // Read-only access to the toolbar (pattern text + option toggles). Used by
    // tests to assert view-context round-trip and to drive toggle state.
    SearchToolbar* searchToolbar() const { return searchToolbar_; }
    // Mutable access for test-driving (e.g. forcing updateView); const variant
    // below for read-only inspection.
    Overview* overview() { return &overview_; }
    const Overview* overview() const { return &overview_; }
    QString currentMainFilePath() const { return currentMainFilePath_; }
    // True while a search is running (cleared on searchFinished). Lets
    // integration tests wait for completion before asserting exact counts.
    bool isSearchActive() const { return searchActive_; }
    // Set the pattern and kick off a search (async; results land via the
    // searchFinished signal).
    void searchFor( const QString& pattern );
    // Simulate selecting a result row (opens its source file in the main view).
    void selectResultRow( LineNumber line );
    // Simulate clicking a group-header row (toggles that group's collapse).
    void clickHeaderRow( LineNumber line );

    // Collapse / expand every group (wired to the toolbar buttons; public so
    // they can be driven programmatically / from tests).
    void collapseAll();
    void expandAll();

    // Re-apply Configuration (line numbers, font, overview, shortcuts) to both
    // views. Called on construction and on MainWindow::optionsChanged (the
    // View-menu toggles for line numbers / overview / wrap). Overrides
    // AbstractCrawlerWidget.
    void applyConfiguration() override;
    // Register view-level keyboard shortcuts on both views (arrow keys, PgUp/
    // PgDn, jump-to-top/bottom, ...). Overrides AbstractCrawlerWidget.
    void registerShortcuts() override;

  Q_SIGNALS:
    // Required by TabbedCrawlerWidget::addCrawler (template expects this
    // signal). Folder tabs are static, so this is never emitted for now.
    void dataStatusChanged( DataStatus status );

  protected:
    // ViewInterface (single-file APIs are no-ops in folder mode).
    void doSetData( std::shared_ptr<SearchableLogData> log_data,
                    std::shared_ptr<LogFilteredData> filtered_data ) override;
    void doSetQuickFindPattern( std::shared_ptr<QuickFindPattern> qfp ) override;
    void doSetSavedSearches( SavedSearches* saved_searches ) override;
    void doSetViewContext( const QString& view_context ) override;
    std::shared_ptr<const ViewContextInterface> doGetViewContext() const override;

  private Q_SLOTS:
    void startSearch();
    void stopSearch();
    void onSearchStarted( quint64 generation );
    void onSearchProgressed( quint64 nbMatches, int percent, quint64 generation );
    void onSearchFinished( quint64 generation );
    void onFileGroupReady( int fileIndex, quint64 generation );
    void onResultSelected( LineNumber line, LinesCount nLines, LineColumn startCol,
                           LineLength nSymbols );
    void onHeaderClicked( LineNumber line );

  private:
    void openFileInMainView( const QString& filePath, LineNumber localLine );
    void cacheMainViewData( const QString& filePath, std::shared_ptr<LogData> data );
    // Re-point the per-file overview at the opened file: that file's folder-search
    // matches become the Overview marks, and linesInFile_ is sized to the file so
    // the viewport box + click-to-jump map within the file. No-op until a file is
    // loaded into currentMainData_. Folder mode uses an explicit match-line list
    // (no LogFilteredData), so this is a pure else-branch on the Overview.
    void refreshFileOverview( const QString& filePath );

    QString folderPath_;
    QStringList filePaths_;
    std::shared_ptr<QuickFindPattern> quickFindPattern_;

    std::shared_ptr<FolderSearchResults> folderResults_;
    FolderSearchEngine* engine_ = nullptr;
    FolderFilteredView* filteredView_ = nullptr;
    LogMainView* mainView_ = nullptr;
    QSplitter* splitter_ = nullptr;

    // LogMainView requires a (non-null) Overview/OverviewWidget pair. In folder
    // mode the Overview is driven per-file: when a result is opened, that file's
    // folder-search matches become the overview marks and linesInFile_ is sized to
    // the file. The widget is re-parented to mainView_ so AbstractLogView places
    // it; visibility follows Configuration::isOverviewVisible() (mirrors
    // CrawlerWidget).
    Overview overview_;
    OverviewWidget* overviewWidget_ = nullptr;

    // Shared search toolbar (replaces the former inline QLineEdit + hand-built
    // Search/Stop buttons). Folder mode passes a null SavedSearches (no
    // history); the toolbar guards that. Collapse/expand buttons live
    // alongside the toolbar in this widget's own layout.
    SearchToolbar* searchToolbar_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QToolButton* collapseAllButton_ = nullptr;
    QToolButton* expandAllButton_ = nullptr;

    // Main-view file data: a placeholder (empty) until a row is clicked, then
    // the selected file's LogData. Recently-used files are cached (true LRU,
    // MainViewCacheLimit entries) so switching between files is instant.
    std::shared_ptr<LogData> placeholderData_;
    std::shared_ptr<LogData> currentMainData_;
    QString currentMainFilePath_;
    // value: { data, iterator into mainViewCacheOrder_ }; order tracks access
    // recency (front = most-recently-used). std::list iterators stay valid on
    // splice/erase, so the map is never invalidated by reordering.
    std::unordered_map<QString,
                       std::pair<std::shared_ptr<LogData>, std::list<QString>::iterator>>
        mainViewCache_;
    std::list<QString> mainViewCacheOrder_;
    static constexpr size_t MainViewCacheLimit = 8;

    // Pending async load (file clicked before its index finished building).
    std::shared_ptr<LogData> pendingMainData_;
    QString pendingMainFilePath_;
    LineNumber pendingJumpLine_ = 0_lnum;

    // Current search generation: streaming fileGroupReady/searchFinished signals
    // whose generation differs are dropped (superseded by a newer search).
    quint64 currentSearchGeneration_ = 0;
    bool searchActive_ = false;

    // The last search pattern run by startSearch. Forwarded to mainView_ so that
    // when a result is clicked and its file opens, the matched substring is
    // highlighted in the opened file (single-file parity). Stored here so the
    // pattern can be re-applied right after each setDataSource swap in
    // openFileInMainView (setDataSource does not reset searchPattern_, but the
    // re-apply makes the parity intent explicit and is robust to future changes).
    RegularExpressionPattern currentSearchPattern_;
};

#endif // FOLDERCRAWLERWIDGET_H
