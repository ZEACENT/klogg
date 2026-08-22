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

// SearchToolbar is the shared, reusable owner of the search-input QComboBox,
// the option toggle buttons, the action buttons and the RegularExpression
// Pattern construction. See searchtoolbar.h for the design rationale.

#include "searchtoolbar.h"

#include <QAction>
#include <QComboBox>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDialog>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QStringListModel>
#include <QToolButton>
#include <algorithm>
#include <iterator>
#include <limits>

#include "configuration.h"
#include "dispatch_to.h"
#include "filterdiffdialog.h"
#include "filterfavoritesmodel.h"
#include "predefinedfilterscombobox.h"
#include "savefavoritedialog.h"
#include "savedsearches.h"
#include "uimessage.h"

SearchToolbar::SearchToolbar( QWidget* parent, SavedSearches* savedSearches )
    : QWidget( parent )
    , savedSearches_( savedSearches )
    , iconLoader_( this )
{
    setupWidgets();
    setupConnections();
    loadIcons();
}

SearchToolbar::~SearchToolbar() = default;

void SearchToolbar::setupWidgets()
{
    // --- Option toggle buttons (ports crawlerwidget.cpp:1350-1378) ---
    matchCaseButton_ = new QToolButton();
    matchCaseButton_->setToolTip( tr( "Match case" ) );
    matchCaseButton_->setCheckable( true );
    matchCaseButton_->setFocusPolicy( Qt::NoFocus );
    matchCaseButton_->setContentsMargins( 2, 2, 2, 2 );

    useRegexpButton_ = new QToolButton();
    useRegexpButton_->setToolTip( tr( "Use regex" ) );
    useRegexpButton_->setCheckable( true );
    useRegexpButton_->setFocusPolicy( Qt::NoFocus );
    useRegexpButton_->setContentsMargins( 2, 2, 2, 2 );

    inverseButton_ = new QToolButton();
    inverseButton_->setToolTip( tr( "Inverse match" ) );
    inverseButton_->setCheckable( true );
    inverseButton_->setFocusPolicy( Qt::NoFocus );
    inverseButton_->setContentsMargins( 2, 2, 2, 2 );

    booleanButton_ = new QToolButton();
    booleanButton_->setToolTip( tr( "Enable regular expression logical combining" ) );
    booleanButton_->setCheckable( true );
    booleanButton_->setFocusPolicy( Qt::NoFocus );
    booleanButton_->setContentsMargins( 2, 2, 2, 2 );

    searchRefreshButton_ = new QToolButton();
    searchRefreshButton_->setToolTip( tr( "Auto-refresh" ) );
    searchRefreshButton_->setCheckable( true );
    searchRefreshButton_->setFocusPolicy( Qt::NoFocus );
    searchRefreshButton_->setContentsMargins( 2, 2, 2, 2 );

    // --- Search line QComboBox + completer + context menu (1380-1404) ---
    const auto recent = ( savedSearches_ != nullptr ) ? savedSearches_->recentSearches() : QStringList{};
    searchLineCompleter_ = new QCompleter( recent, this );
    searchLineEdit_ = new QComboBox;
    searchLineEdit_->setEditable( true );
    searchLineEdit_->setCompleter( searchLineCompleter_ );
    searchLineEdit_->addItems( recent );
    searchLineEdit_->lineEdit()->clear();
    searchLineEdit_->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Minimum );
    searchLineEdit_->setSizeAdjustPolicy( QComboBox::AdjustToMinimumContentsLengthWithIcon );
    searchLineEdit_->lineEdit()->setMaxLength( std::numeric_limits<int>::max() / 1024 );
    searchLineEdit_->setContentsMargins( 2, 2, 2, 2 );
    searchLineEdit_->installEventFilter( this );
    searchLineEdit_->lineEdit()->installEventFilter( this );

    auto* clearSearchHistoryAction = new QAction( tr( "Clear Search History" ), this );
    clearSearchHistoryAction->setObjectName( QStringLiteral( "clearSearchHistoryAction" ) );
    auto* editSearchHistoryAction = new QAction( tr( "Edit Search History..." ), this );
    editSearchHistoryAction->setObjectName( QStringLiteral( "editSearchHistoryAction" ) );
    auto* addFavoriteFilterAction
        = new QAction( tr( "Add to Filter Favorites..." ), this );
    addFavoriteFilterAction->setObjectName( QStringLiteral( "addFavoriteFilterAction" ) );

    searchLineContextMenu_ = searchLineEdit_->lineEdit()->createStandardContextMenu();
    searchLineContextMenu_->addSeparator();
    searchLineContextMenu_->addAction( addFavoriteFilterAction );
    searchLineContextMenu_->addSeparator();
    searchLineContextMenu_->addAction( editSearchHistoryAction );
    searchLineContextMenu_->addAction( clearSearchHistoryAction );
    searchLineEdit_->setContextMenuPolicy( Qt::CustomContextMenu );

    setFocusProxy( searchLineEdit_ );

    // --- Action buttons (1408-1436) ---
    clearButton_ = new QToolButton();
    clearButton_->setText( tr( "Clear search text" ) );
    clearButton_->setAutoRaise( true );
    clearButton_->setContentsMargins( 2, 2, 2, 2 );

    searchButton_ = new QToolButton();
    searchButton_->setText( tr( "Search" ) );
    searchButton_->setAutoRaise( true );
    searchButton_->setContentsMargins( 2, 2, 2, 2 );

    keepSearchResultsButton_ = new QToolButton();
    keepSearchResultsButton_->setText( tr( "Keep Results" ) );
    keepSearchResultsButton_->setToolTip(
        tr( "Keep these results and show subsequent results in a new window" ) );
    keepSearchResultsButton_->setCheckable( true );
    keepSearchResultsButton_->setContentsMargins( 2, 2, 2, 2 );

    stopButton_ = new QToolButton();
    stopButton_->setAutoRaise( true );
    stopButton_->setEnabled( false );
    stopButton_->setVisible( false );
    stopButton_->setContentsMargins( 2, 2, 2, 2 );

    predefinedFilters_ = new PredefinedFiltersComboBox( this );
    favoriteFilterButton_ = new QToolButton();
    favoriteFilterButton_->setAutoRaise( true );
    favoriteFilterButton_->setToolTip( tr( "Add current filter to favorites" ) );
    favoriteFilterButton_->setFocusPolicy( Qt::NoFocus );
    favoriteFilterButton_->setContentsMargins( 2, 2, 2, 2 );

    // --- Layout (ports the searchLineLayout widget order, 1464-1482) ---
    auto* layout = new QHBoxLayout( this );
    layout->setContentsMargins( 0, 0, 0, 0 );
    layout->setSpacing( 2 );

    layout->addWidget( matchCaseButton_ );
    layout->addWidget( useRegexpButton_ );
    layout->addWidget( inverseButton_ );
    layout->addWidget( booleanButton_ );
    layout->addWidget( searchRefreshButton_ );
    layout->addWidget( predefinedFilters_ );
    layout->addWidget( favoriteFilterButton_ );
    layout->addWidget( searchLineEdit_, 1 );
    layout->addWidget( clearButton_ );
    layout->addWidget( searchButton_ );
    layout->addWidget( keepSearchResultsButton_ );
    layout->addWidget( stopButton_ );

    setLayout( layout );

    // Favorite persistence is common to every host; history remains host-specific.
    connect( addFavoriteFilterAction, &QAction::triggered, this,
             &SearchToolbar::saveCurrentSearchAsFavorite );
    connect( clearSearchHistoryAction, &QAction::triggered, this,
             [ this ]() { Q_EMIT clearHistoryRequested(); } );
    connect( editSearchHistoryAction, &QAction::triggered, this,
             [ this ]() { Q_EMIT editHistoryRequested(); } );
    connect( favoriteFilterButton_, &QToolButton::clicked, this,
             &SearchToolbar::saveCurrentSearchAsFavorite );
    connect( searchLineEdit_, &QWidget::customContextMenuRequested, this,
             [ this ]() { searchLineContextMenu_->exec( QCursor::pos() ); } );
}

void SearchToolbar::setupConnections()
{
    // Return / search button -> searchRequested (ports 1520-1521 + 1539).
    connect( searchLineEdit_->lineEdit(), &QLineEdit::returnPressed, searchButton_,
             &QToolButton::click );
    connect( searchButton_, &QToolButton::clicked, this, [ this ]() { Q_EMIT searchRequested(); } );

    // Stop button -> stopRequested (ports 1541).
    connect( stopButton_, &QToolButton::clicked, this, [ this ]() { Q_EMIT stopRequested(); } );

    // Clear button -> clear edit text (ports 1542).
    connect( clearButton_, &QToolButton::clicked, searchLineEdit_, &QComboBox::clearEditText );

    // Text edited -> searchTextChanged (ports 1522-1523).
    connect( searchLineEdit_->lineEdit(), &QLineEdit::textEdited, this,
             &SearchToolbar::searchTextChanged );

    // currentIndexChanged -> predefinedFilterActivated (dropdown selection only
    // refreshes the predefined-filters widget; it must NOT suspend auto-refresh
    // tracking, so it does NOT fire searchTextChanged/resetState). Ports the
    // original 1525-1526 split (currentIndexChanged -> updatePredefinedFilters
    // only; textEdited -> searchTextChangeHandler).
    connect( searchLineEdit_,
             QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
             [ this ]( auto ) { Q_EMIT predefinedFilterActivated( searchLineEdit_->currentText() ); } );

    // Predefined filters combo (ports 1528-1529): the host owns
    // setSearchPatternFromPredefinedFilters; re-emit as saveFavorite-less signal
    // is not enough, so expose the combo directly for the host to connect.
    // (CrawlerWidget connects predefinedFilters_->filterChanged itself.)

    // Option toggles (ports 1622-1638).
    // matchCase: set completer case sensitivity internally + emit optionsChanged
    // (so the host runs resetStateOnSearchPatternChanges) + matchCaseChanged
    // (forwarded to MainWindow).  Ports matchCaseChangedHandler 1106-1107 + the
    // toggled wiring.
    connect( matchCaseButton_, &QPushButton::toggled, this, [ this ]( bool shouldMatchCase ) {
        searchLineCompleter_->setCaseSensitivity( shouldMatchCase ? Qt::CaseSensitive
                                                                  : Qt::CaseInsensitive );
        Q_EMIT optionsChanged();
        Q_EMIT matchCaseChanged( shouldMatchCase );
    } );

    // useRegexp / boolean: emit optionsChanged (ports useRegexpChangeHandler /
    // booleanCombiningChangedHandler, both of which called resetState).
    connect( useRegexpButton_, &QPushButton::toggled, this, [ this ]( bool ) { Q_EMIT optionsChanged(); } );
    connect( booleanButton_, &QPushButton::toggled, this, [ this ]( bool ) { Q_EMIT optionsChanged(); } );

    // inverseButton has NO toggled handler today (only read at pattern-build
    // time) -- keep it handler-less to preserve behavior.

    // auto-refresh: emit autoRefreshChanged (host runs searchRefreshChangedHandler
    // and forwards searchRefreshChanged to MainWindow). Ports 1622-1623 +
    // 1636-1637.
    connect( searchRefreshButton_, &QPushButton::toggled, this, &SearchToolbar::autoRefreshChanged );
}

RegularExpressionPattern SearchToolbar::currentRegularExpressionPattern() const
{
    // Verbatim move of crawlerwidget.cpp:1976-1978.
    return RegularExpressionPattern( currentSearchText(), isMatchCase(), isInverse(),
                                     isBoolean(), !isUseRegexp() );
}

QString SearchToolbar::currentSearchText() const
{
    return searchLineEdit_->currentText();
}

void SearchToolbar::setSearchPattern( const QString& searchPattern )
{
    // Ports crawlerwidget.cpp:1217-1227. updatePredefinedFiltersWidget is the
    // host's concern; it is driven by the searchTextChanged signal below.
    searchLineEdit_->setEditText( searchPattern );
    Q_EMIT searchTextChanged( searchPattern );
    // Set the focus to lineEdit so that the user can press 'Return' immediately
    searchLineEdit_->lineEdit()->setFocus();

    if ( Configuration::get().autoRunSearchOnPatternChange() ) {
        // The search itself is the host's concern; request it.
        Q_EMIT searchRequested();
    }
}

QString SearchToolbar::escapeSearchPattern( const QString& pattern, bool isRegex ) const
{
    // Verbatim move of crawlerwidget.cpp:1155-1166.
    auto escapedPattern = ( !isRegex && useRegexpButton_->isChecked() )
                              ? QRegularExpression::escape( pattern )
                              : pattern;

    if ( booleanButton_->isChecked() ) {
        escapedPattern = wrapBooleanOperand( escapedPattern );
    }

    return escapedPattern;
}

QString SearchToolbar::wrapBooleanOperand( const QString& pattern ) const
{
    // Escape the operand for inclusion in a "..." boolean operand, C-style:
    // backslashes FIRST (so a trailing '\' does not escape the closing quote),
    // then embedded double-quotes. The boolean parser
    // (parseBooleanExpressions) counts the backslash run before a quote to
    // decide real-vs-escaped and un-escapes both '\\' and '\\"', so this
    // round-trips. Order matters: escaping '"' would insert backslashes, which
    // is why backslashes must be doubled first.
    QString wrapped = pattern;
    wrapped.replace( '\\', "\\\\" );
    wrapped.replace( '"', "\\\"" );
    wrapped.prepend( '"' ).append( '"' );
    return wrapped;
}

QString& SearchToolbar::combinePatterns( QString& currentPattern, const QString& newPattern ) const
{
    // Verbatim move of crawlerwidget.cpp:1168-1182.
    if ( !currentPattern.isEmpty() ) {
        if ( booleanButton_->isChecked() ) {
            currentPattern.append( " or " );
        }
        else if ( useRegexpButton_->isChecked() ) {
            currentPattern.append( '|' );
        }
    }

    currentPattern.append( newPattern );

    return currentPattern;
}

bool SearchToolbar::isMatchCase() const
{
    return matchCaseButton_->isChecked();
}

bool SearchToolbar::isUseRegexp() const
{
    return useRegexpButton_->isChecked();
}

bool SearchToolbar::isInverse() const
{
    return inverseButton_->isChecked();
}

bool SearchToolbar::isBoolean() const
{
    return booleanButton_->isChecked();
}

bool SearchToolbar::isAutoRefresh() const
{
    return searchRefreshButton_->isChecked();
}

void SearchToolbar::setMatchCase( bool checked )
{
    matchCaseButton_->setChecked( checked );
}

void SearchToolbar::setUseRegexp( bool checked )
{
    useRegexpButton_->setChecked( checked );
}

void SearchToolbar::setInverse( bool checked )
{
    inverseButton_->setChecked( checked );
}

void SearchToolbar::setBoolean( bool checked )
{
    booleanButton_->setChecked( checked );
}

void SearchToolbar::setAutoRefresh( bool checked )
{
    searchRefreshButton_->setChecked( checked );
}

bool SearchToolbar::isKeepResultsChecked() const
{
    return keepSearchResultsButton_->isChecked();
}

void SearchToolbar::setKeepResultsChecked( bool checked )
{
    keepSearchResultsButton_->setChecked( checked );
}

void SearchToolbar::setSearchInProgress( bool busy )
{
    // Ports replaceCurrentSearch 1985-1988 + updateFilteredView 674-677.
    if ( busy ) {
        stopButton_->setEnabled( true );
        stopButton_->show();
        clearButton_->hide();
        searchButton_->hide();
    }
    else {
        stopButton_->setEnabled( false );
        stopButton_->hide();
        searchButton_->show();
        clearButton_->show();
    }
}

void SearchToolbar::setItems( const QStringList& items )
{
    const QString text = searchLineEdit_->lineEdit()->text();
    searchLineEdit_->clear();
    searchLineEdit_->addItems( items );
    // In case we had something that wasn't added to the list (blank...):
    searchLineEdit_->lineEdit()->setText( text );

    searchLineCompleter_->setModel( new QStringListModel( items, searchLineCompleter_ ) );
}

void SearchToolbar::setSearchHistory( SavedSearches* savedSearches )
{
    savedSearches_ = savedSearches;
}

void SearchToolbar::recordSearch()
{
    if ( savedSearches_ == nullptr ) {
        return;
    }
    const auto pattern = currentSearchText();
    if ( pattern.isEmpty() ) {
        return;
    }
    // Reload first in case another klogg instance changed the history.
    const auto& searches = SavedSearches::getSynced();
    savedSearches_->addRecent( pattern );
    searches.save();
    setItems( savedSearches_->recentSearches() );
}

void SearchToolbar::saveCurrentSearchAsFavorite()
{
    const auto currentText = currentSearchText().trimmed();
    if ( currentText.isEmpty() ) {
        return;
    }

    auto& model = FilterFavoritesModel::instance();
    model.synchronizeFromStorage();
    const auto dialogFavorites = model.favorites();
    const auto useRegex = isUseRegexp();

    SaveFavoriteDialog dialog( currentText, dialogFavorites, this );
    if ( dialog.exec() != QDialog::Accepted ) {
        return;
    }

    const auto favoriteName = dialog.favoriteName();
    if ( favoriteName.isEmpty() ) {
        return;
    }

    const bool isCreateNew = dialog.isCreateNew();
    QString targetName = favoriteName;
    PredefinedFilter selectedFavorite{};
    const PredefinedFilter* selectedIdentity = nullptr;
    if ( !isCreateNew ) {
        const int selectedIndex = dialog.selectedExistingIndex();
        if ( selectedIndex < 0 || selectedIndex >= dialogFavorites.size() ) {
            return;
        }
        selectedFavorite = dialogFavorites.at( selectedIndex );
        selectedIdentity = &selectedFavorite;
        targetName = selectedFavorite.name;
    }

    const auto countByName = []( const auto& favorites, const QString& name ) {
        int count = 0;
        for ( const auto& favorite : favorites ) {
            if ( favorite.name.compare( name, Qt::CaseInsensitive ) == 0 ) {
                ++count;
            }
        }
        return count;
    };
    const auto findStableTarget = []( auto& favorites, const QString& name,
                                      const PredefinedFilter* identity ) {
        auto onlyNameMatch = favorites.end();
        auto exactIdentityMatch = favorites.end();
        int nameMatchCount = 0;
        int identityMatchCount = 0;

        for ( auto candidate = favorites.begin(); candidate != favorites.end(); ++candidate ) {
            if ( candidate->name.compare( name, Qt::CaseInsensitive ) != 0 ) {
                continue;
            }

            ++nameMatchCount;
            onlyNameMatch = candidate;
            if ( identity != nullptr && candidate->pattern == identity->pattern
                 && candidate->useRegex == identity->useRegex ) {
                ++identityMatchCount;
                exactIdentityMatch = candidate;
            }
        }

        if ( identity != nullptr ) {
            return identityMatchCount == 1 ? exactIdentityMatch : favorites.end();
        }
        return nameMatchCount == 1 ? onlyNameMatch : favorites.end();
    };
    const auto warnConflict = [ this, &targetName ] {
        klogg::ui::warning(
            this, tr( "klogg" ),
            tr( "Favorite \"%1\" changed while the save dialog was open. Try again." )
                .arg( targetName ) );
    };
    const auto commitFavorites = [ this, &model, &warnConflict ](
                                     const auto& expected, const auto& replacement ) {
        const auto result = model.replaceFavorites( expected, replacement );
        switch ( result.status ) {
        case PredefinedFiltersCollection::CommitStatus::Success:
        case PredefinedFiltersCollection::CommitStatus::Unchanged:
            return true;
        case PredefinedFiltersCollection::CommitStatus::Conflict:
            warnConflict();
            return false;
        case PredefinedFiltersCollection::CommitStatus::InvalidReplacement:
        case PredefinedFiltersCollection::CommitStatus::LockError:
        case PredefinedFiltersCollection::CommitStatus::StorageError:
        case PredefinedFiltersCollection::CommitStatus::WriteError:
            klogg::ui::warning( this, tr( "klogg" ),
                                tr( "Unable to save filter favorites. Try again." ) );
            return false;
        }
        return false;
    };

    // The modal can remain open while another window or process changes the
    // collection. Merge the requested target against the latest snapshot so
    // unrelated concurrent favorites are retained.
    model.synchronizeFromStorage();
    auto favorites = model.favorites();
    const int nameMatchCount = countByName( favorites, targetName );
    auto existing = findStableTarget( favorites, targetName, selectedIdentity );

    if ( isCreateNew ) {
        if ( nameMatchCount != 0 ) {
            klogg::ui::warning(
                this, tr( "klogg" ),
                tr( "A favorite named \"%1\" already exists. Choose a different name." )
                    .arg( favoriteName ) );
            return;
        }

        const auto expectedFavorites = favorites;
        favorites.push_back( { favoriteName, currentText, useRegex } );
        commitFavorites( expectedFavorites, favorites );
        return;
    }

    if ( existing == favorites.end() ) {
        warnConflict();
        return;
    }

    if ( existing->pattern == currentText && existing->useRegex == useRegex ) {
        klogg::ui::information(
            this, tr( "klogg" ),
            tr( "Favorite \"%1\" already has the same content." ).arg( existing->name ) );
        return;
    }

    const auto displayedFavorite = *existing;
    FilterDiffDialog diffDialog( existing->name, displayedFavorite, currentText, useRegex, this );
    if ( diffDialog.exec() != QDialog::Accepted ) {
        return;
    }

    // A second modal was open. Re-read once more and only overwrite the exact
    // target the user reviewed; retain concurrent changes to other favorites.
    model.synchronizeFromStorage();
    favorites = model.favorites();
    existing = findStableTarget( favorites, targetName, &displayedFavorite );
    if ( existing == favorites.end()
         || existing->name.compare( displayedFavorite.name, Qt::CaseInsensitive ) != 0
         || existing->pattern != displayedFavorite.pattern
         || existing->useRegex != displayedFavorite.useRegex ) {
        warnConflict();
        return;
    }

    const int existingIndex = static_cast<int>( std::distance( favorites.begin(), existing ) );
    const auto expectedFavorites = favorites;
    favorites[ existingIndex ].pattern = currentText;
    favorites[ existingIndex ].useRegex = useRegex;
    commitFavorites( expectedFavorites, favorites );
}

void SearchToolbar::loadIcons()
{
    // Ports crawlerwidget.cpp:1919-1928 (toolbar owns its own IconLoader).
    searchRefreshButton_->setIcon( iconLoader_.load( "icons8-search-refresh" ) );
    useRegexpButton_->setIcon( iconLoader_.load( "regex" ) );
    inverseButton_->setIcon( iconLoader_.load( "icons8-not-equal" ) );
    booleanButton_->setIcon( iconLoader_.load( "icons8-venn-diagram" ) );
    clearButton_->setIcon( iconLoader_.load( "icons8-delete" ) );
    searchButton_->setIcon( iconLoader_.load( "icons8-search" ) );
    keepSearchResultsButton_->setIcon( iconLoader_.load( "icons8-lock" ) );
    matchCaseButton_->setIcon( iconLoader_.load( "icons8-font-size" ) );
    stopButton_->setIcon( iconLoader_.load( "icons8-close-window" ) );
    favoriteFilterButton_->setIcon( iconLoader_.load( "icons8-star" ) );
}

bool SearchToolbar::eventFilter( QObject* watched, QEvent* event )
{
    // Verbatim move of crawlerwidget.cpp:316-352 (Home/End handling for the
    // search input).
    const auto* searchEdit = searchLineEdit_ != nullptr ? searchLineEdit_->lineEdit() : nullptr;
    const auto isSearchInput = watched == searchLineEdit_ || watched == searchEdit;
    if ( !isSearchInput ) {
        return QWidget::eventFilter( watched, event );
    }

    if ( event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress ) {
        return QWidget::eventFilter( watched, event );
    }

    auto* keyEvent = static_cast<QKeyEvent*>( event );
    const auto key = keyEvent->key();
    if ( key != Qt::Key_Home && key != Qt::Key_End ) {
        return QWidget::eventFilter( watched, event );
    }

    const auto modifiers = keyEvent->modifiers() & ~Qt::KeypadModifier;
    if ( modifiers != Qt::NoModifier ) {
        return QWidget::eventFilter( watched, event );
    }

    if ( event->type() == QEvent::ShortcutOverride ) {
        event->accept();
        return false;
    }

    if ( key == Qt::Key_Home ) {
        searchLineEdit_->lineEdit()->setCursorPosition( 0 );
    }
    else {
        searchLineEdit_->lineEdit()->end( false );
    }
    event->accept();
    return true;
}

// --- QTest accessors ---
QComboBox* SearchToolbar::searchLineEdit() const
{
    return searchLineEdit_;
}

QToolButton* SearchToolbar::matchCaseButton() const
{
    return matchCaseButton_;
}

QToolButton* SearchToolbar::inverseButton() const
{
    return inverseButton_;
}

QToolButton* SearchToolbar::booleanButton() const
{
    return booleanButton_;
}

QToolButton* SearchToolbar::searchButton() const
{
    return searchButton_;
}

QToolButton* SearchToolbar::stopButton() const
{
    return stopButton_;
}

QToolButton* SearchToolbar::useRegexpButton() const
{
    return useRegexpButton_;
}

QToolButton* SearchToolbar::searchRefreshButton() const
{
    return searchRefreshButton_;
}

QToolButton* SearchToolbar::clearButton() const
{
    return clearButton_;
}

QToolButton* SearchToolbar::keepSearchResultsButton() const
{
    return keepSearchResultsButton_;
}

QToolButton* SearchToolbar::favoriteFilterButton() const
{
    return favoriteFilterButton_;
}

PredefinedFiltersComboBox* SearchToolbar::predefinedFilters() const
{
    return predefinedFilters_;
}

QCompleter* SearchToolbar::searchLineCompleter() const
{
    return searchLineCompleter_;
}
