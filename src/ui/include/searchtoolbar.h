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

#ifndef SEARCHTOOLBAR_H
#define SEARCHTOOLBAR_H

#include <cstdint>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "iconloader.h"
#include "regularexpressionpattern.h"

class QComboBox;
class QCompleter;
class QMenu;
class QToolButton;
class PredefinedFiltersComboBox;
class SavedSearches;

// SearchToolbar is the single owner of the search-input QComboBox, the five
// option toggle buttons (match case / use regex / inverse / boolean /
// auto-refresh), the clear / search / keep-results / stop buttons, the
// predefined-filters combo and favorite button, and the
// RegularExpressionPattern construction (including its escape / combine
// helpers).
//
// It is adopted as-is by CrawlerWidget (behavior-identical: the SAME
// QComboBox / QToolButton instances are created here and wired to handlers
// that emit the same downstream effect) and by FolderCrawlerWidget
// (replacing its minimal inline toolbar, gaining case / regex / inverse /
// boolean support for free).
//
// CrawlerWidget keeps the LogFilteredData / FilteredView-specific widgets
// (visibilityBox, searchInfoLine, context-lines) and the SearchState machine;
// it wires itself to this toolbar's signals.
class SearchToolbar : public QWidget {
    Q_OBJECT

  public:
    // savedSearches may be null (folder mode has no history); the toolbar
    // guards a null pointer and skips history population / completer model.
    explicit SearchToolbar( QWidget* parent = nullptr, SavedSearches* savedSearches = nullptr );
    ~SearchToolbar() override;

    SearchToolbar( const SearchToolbar& ) = delete;
    SearchToolbar& operator=( const SearchToolbar& ) = delete;

    struct FilterSpec {
        QString text;
        bool matchCase = false;
        bool useRegexp = false;
        bool inverse = false;
        bool boolean = false;
        bool autoRefresh = false;
        bool keepResults = false;

        RegularExpressionPattern regularExpressionPattern() const;
        bool operator==( const FilterSpec& other ) const;
        bool operator!=( const FilterSpec& other ) const { return !( *this == other ); }
    };

    struct FilterDraftSnapshot {
        FilterSpec filter;
        uint64_t revision = 0;
    };

    // Owner-thread value observation of the current toolbar draft, NOT the
    // submitted query, a pane's applied query, or result readiness. Reads do not
    // focus, edit, or request a search. Revision is local to this toolbar's
    // lifetime and advances on semantic edits, including edits back to an old
    // value; history repopulation and search busy/idle presentation are neutral.
    FilterDraftSnapshot filterDraftSnapshot() const;

    // Owner-thread compare-and-install for coordinated publication. One semantic
    // revision, no focus, input/option signals or automatic search. The host must
    // install its applied query/model state in the same non-yielding publication.
    bool canInstallFilterDraft( const FilterDraftSnapshot& expected, const FilterSpec& candidate ) const;
    bool installFilterDraft( const FilterDraftSnapshot& expected, const FilterSpec& candidate );

    // --- Pattern contract (verbatim move of crawlerwidget.cpp:1976-1978) ---
    // isPlainText = !useRegexp is preserved exactly.
    RegularExpressionPattern currentRegularExpressionPattern() const;

    QString currentSearchText() const;

    // Sets the edit text, updates predefined-filters, focuses the line edit,
    // and (if Configuration::autoRunSearchOnPatternChange) emits
    // searchRequested() so the host re-runs its search.
    void setSearchPattern( const QString& searchPattern );

    // Escape / combine helpers (moved from crawlerwidget.cpp:1155-1182).
    // Public so CrawlerWidget's addToSearch / excludeFromSearch / replaceSearch
    // slots can reuse them.
    QString escapeSearchPattern( const QString& pattern, bool isRegex = false ) const;
    QString& combinePatterns( QString& currentPattern, const QString& newPattern ) const;

    // Wrap `pattern` as a single boolean operand: surround it with double-quotes
    // and backslash-escape any embedded double-quote first. Does NOT regex-escape.
    // Used by escapeSearchPattern (boolean branch) and by CrawlerWidget's
    // excludeFromSearch, which previously inlined a no-op quote replace
    // (replace('"',"\"") -- a 1-char literal -- leaving embedded quotes to break
    // the boolean expression). Centralizing the escaping here keeps the two
    // callers from drifting apart again.
    QString wrapBooleanOperand( const QString& pattern ) const;

    // --- Option flag accessors ---
    bool isMatchCase() const;
    bool isUseRegexp() const;
    bool isInverse() const;
    bool isBoolean() const;
    bool isAutoRefresh() const;

    // --- Option flag setters (programmatic; fire the normal signals) ---
    void setMatchCase( bool checked );
    void setUseRegexp( bool checked );
    void setInverse( bool checked );
    void setBoolean( bool checked );
    void setAutoRefresh( bool checked );

    bool isKeepResultsChecked() const;
    void setKeepResultsChecked( bool checked );

    // The search start/stop show-hide dance shared by search start and
    // terminal status publication.
    // busy: stop enable+show, clear/search hide.
    // !busy: stop disable+hide, search/clear show.
    void setSearchInProgress( bool busy );

    // History / items management.
    void setItems( const QStringList& items );
    void setSearchHistory( SavedSearches* savedSearches );

    // Records the current pattern into the shared history: reloads from disk
    // first (another klogg instance may have changed it), saves, and refreshes
    // the dropdown. Shared by CrawlerWidget and FolderCrawlerWidget so folder
    // searches persist across restarts exactly like single-file ones.
    void recordSearch();
    // Retain the committed query if a later observer has already edited the draft.
    void recordSearch( const QString& pattern );

    // Loads icons for the owned buttons via its own IconLoader.
    void loadIcons();

    // --- QTest access (the SAME widget instances, now parented here) ---
    QComboBox* searchLineEdit() const;
    QToolButton* matchCaseButton() const;
    QToolButton* inverseButton() const;
    QToolButton* booleanButton() const;
    QToolButton* searchButton() const;
    QToolButton* stopButton() const;
    QToolButton* useRegexpButton() const;
    QToolButton* searchRefreshButton() const;
    QToolButton* clearButton() const;
    QToolButton* keepSearchResultsButton() const;
    QToolButton* favoriteFilterButton() const;
    PredefinedFiltersComboBox* predefinedFilters() const;
    QCompleter* searchLineCompleter() const;

  Q_SIGNALS:
    // The user requested a new search (Return pressed or Search clicked).
    void searchRequested();
    // The user requested stopping the current search (Stop clicked).
    void stopRequested();
    // An option that affects the search outcome changed (match case / use
    // regex / boolean). Replaces the per-button toggled handlers.
    void optionsChanged();
    // The search text was edited by the user.
    void searchTextChanged( QString text );
    // The user picked an entry from the search-history dropdown (distinct from
    // searchTextChanged so the host can update the predefined-filters widget
    // WITHOUT suspending auto-refresh tracking -- mirrors the original split
    // where currentIndexChanged only refreshed predefined filters).
    void predefinedFilterActivated( QString text );
    // "match case" toggle changed (also drives the completer case sensitivity
    // internally and is forwarded to the host's MainWindow-facing signal).
    void matchCaseChanged( bool matchCase );
    // "auto-refresh" toggle changed (forwarded to the host).
    void autoRefreshChanged( bool isRefreshing );
    // Search-history actions remain host-specific; favorite saving is owned here.
    void clearHistoryRequested();
    void editHistoryRequested();

  protected:
    bool eventFilter( QObject* watched, QEvent* event ) override;

  private Q_SLOTS:
    void saveCurrentSearchAsFavorite();

  private:
    void setupWidgets();
    void setupConnections();
    void updateFilterDraft();

    FilterDraftSnapshot filterDraft_;
    bool updatingHistoryItems_ = false;

    SavedSearches* savedSearches_ = nullptr;

    IconLoader iconLoader_;

    QComboBox* searchLineEdit_ = nullptr;
    QCompleter* searchLineCompleter_ = nullptr;
    QMenu* searchLineContextMenu_ = nullptr;

    QToolButton* clearButton_ = nullptr;
    QToolButton* searchButton_ = nullptr;
    QToolButton* keepSearchResultsButton_ = nullptr;
    QToolButton* favoriteFilterButton_ = nullptr;
    QToolButton* stopButton_ = nullptr;

    QToolButton* matchCaseButton_ = nullptr;
    QToolButton* useRegexpButton_ = nullptr;
    QToolButton* inverseButton_ = nullptr;
    QToolButton* booleanButton_ = nullptr;
    QToolButton* searchRefreshButton_ = nullptr;

    PredefinedFiltersComboBox* predefinedFilters_ = nullptr;
};

#endif // SEARCHTOOLBAR_H
