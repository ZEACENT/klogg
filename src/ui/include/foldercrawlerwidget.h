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
#include <memory>
#include <unordered_map>

#include "linetypes.h"
#include "viewinterface.h"

#include <QWidget>

class FolderFilteredView;
class FolderSearchEngine;
class FolderSearchResults;
class LogMainView;
class LogData;
class QuickFindPattern;
class QSplitter;
class QLineEdit;
class QLabel;
class QToolButton;

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
class FolderCrawlerWidget : public QWidget, public ViewInterface {
    Q_OBJECT

  public:
    explicit FolderCrawlerWidget( QWidget* parent = nullptr );
    ~FolderCrawlerWidget() override;

    FolderCrawlerWidget( const FolderCrawlerWidget& ) = delete;
    FolderCrawlerWidget& operator=( const FolderCrawlerWidget& ) = delete;

    // Identify the folder + its (already enumerated, natural-sorted) files.
    void setFolder( const QString& folderPath, const QStringList& filePaths );

  Q_SIGNALS:
    // Required by TabbedCrawlerWidget::addCrawler (template expects this
    // signal). Folder tabs are static, so this is never emitted for now.
    void dataStatusChanged( DataStatus status );

  protected:
    // ViewInterface (single-file APIs are no-ops in folder mode).
    void doSetData( std::shared_ptr<SearchableLogData> log_data,
                    std::shared_ptr<LogFilteredData> filtered_data ) override;
    void doSetFolderData( std::shared_ptr<FolderSearchResults> folder_results ) override;
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
    void onResultSelected( LineNumber line, LinesCount nLines, LineColumn startCol,
                           LineLength nSymbols );
    void onHeaderClicked( LineNumber line );
    void collapseAll();
    void expandAll();

  private:
    void openFileInMainView( const QString& filePath, LineNumber localLine );
    void cacheMainViewData( const QString& filePath, std::shared_ptr<LogData> data );

    QString folderPath_;
    QStringList filePaths_;
    std::shared_ptr<QuickFindPattern> quickFindPattern_;

    std::shared_ptr<FolderSearchResults> folderResults_;
    FolderSearchEngine* engine_ = nullptr;
    FolderFilteredView* filteredView_ = nullptr;
    LogMainView* mainView_ = nullptr;
    QSplitter* splitter_ = nullptr;

    QLineEdit* searchEdit_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QToolButton* searchButton_ = nullptr;
    QToolButton* stopButton_ = nullptr;
    QToolButton* collapseAllButton_ = nullptr;
    QToolButton* expandAllButton_ = nullptr;

    // Main-view file data: a placeholder (empty) until a row is clicked, then
    // the selected file's LogData. Recently-used files are cached so switching
    // between files is instant.
    std::shared_ptr<LogData> placeholderData_;
    std::shared_ptr<LogData> currentMainData_;
    QString currentMainFilePath_;
    std::unordered_map<QString, std::shared_ptr<LogData>> mainViewCache_;
    static constexpr size_t MainViewCacheLimit = 8;

    // Pending async load (file clicked before its index finished building).
    std::shared_ptr<LogData> pendingMainData_;
    QString pendingMainFilePath_;
    LineNumber pendingJumpLine_ = 0_lnum;
};

#endif // FOLDERCRAWLERWIDGET_H
