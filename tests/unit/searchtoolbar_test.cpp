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

// Focused unit tests for SearchToolbar. These lock the pattern contract
// (currentRegularExpressionPattern is the verbatim move of the former
// crawlerwidget.cpp:1976-1978 construction) independent of CrawlerWidget, and
// verify the escape/combine helpers round-trip. Constructing SearchToolbar
// alone (null SavedSearches) also proves the folder-mode construction path is
// null-safe.

#include <catch2/catch.hpp>

#include <QAbstractItemDelegate>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QStyleOptionViewItem>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <Qt>

#include <algorithm>
#include <functional>
#include <string_view>

#include "filterfavoritesmodel.h"
#include "persistentinfo.h"
#include "predefinedfilters.h"
#include "predefinedfilterscombobox.h"
#include "regularexpression.h"
#include "regularexpressionpattern.h"
#include "searchtoolbar.h"
#include "uimessage.h"

namespace {
// Helper: drive the widget through the Qt event loop so queued autoRun searches
// (if enabled) and signal delivery settle before assertions.
void pumpEvents()
{
    QTest::qWait( 10 );
}

template <typename Predicate>
bool processEventsUntil( Predicate predicate )
{
    QElapsedTimer deadline;
    deadline.start();
    while ( !predicate() && deadline.elapsed() < 500 ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
    }
    return predicate();
}

class FilterFavoritesRestoreGuard final {
  public:
    using Collection = PredefinedFiltersCollection::Collection;

    explicit FilterFavoritesRestoreGuard( const Collection& initialFavorites )
        : model_( FilterFavoritesModel::instance() )
    {
        auto& settings = PersistentInfo::getSettings( app_settings{} );
        settings.sync();
        settings.beginGroup( QStringLiteral( "PredefinedFiltersCollection" ) );
        const auto keys = settings.allKeys();
        for ( const auto& key : keys ) {
            savedSettingsValues_.insert( key, settings.value( key ) );
        }
        settings.endGroup();
        savedStoredFavorites_ = PredefinedFiltersCollection::getSynced().getFilters();

        // Seed storage first, then synchronize the observable singleton. This is
        // deterministic even when the pre-test model happens to equal the seed
        // while the persistent store differs.
        PredefinedFiltersCollection::get().saveToStorage( initialFavorites );
        model_.synchronizeFromStorage();
    }

    ~FilterFavoritesRestoreGuard()
    {
        auto& settings = PersistentInfo::getSettings( app_settings{} );
        settings.beginGroup( QStringLiteral( "PredefinedFiltersCollection" ) );
        settings.remove( QString{} );
        for ( auto it = savedSettingsValues_.cbegin(); it != savedSettingsValues_.cend(); ++it ) {
            settings.setValue( it.key(), it.value() );
        }
        settings.endGroup();
        settings.sync();

        auto& collection = PredefinedFiltersCollection::getSynced();
        collection.setFilters( savedStoredFavorites_ );
        model_.synchronizeFromStorage();
    }

    FilterFavoritesRestoreGuard( const FilterFavoritesRestoreGuard& ) = delete;
    FilterFavoritesRestoreGuard& operator=( const FilterFavoritesRestoreGuard& ) = delete;

    FilterFavoritesModel& model() const { return model_; }

  private:
    FilterFavoritesModel& model_;
    Collection savedStoredFavorites_;
    QMap<QString, QVariant> savedSettingsValues_;
};

PredefinedFiltersCollection::Collection oneFavorite( const QString& name,
                                                     const QString& pattern )
{
    return { PredefinedFilter{ name, pattern, false } };
}

void replaceStoredFavorites( const PredefinedFiltersCollection::Collection& favorites )
{
    PredefinedFiltersCollection::get().saveToStorage( favorites );
    PersistentInfo::getSettings( app_settings{} ).sync();
}

struct FavoriteModalResult {
    bool saveDialogFound = false;
    bool followupDialogFound = false;
    bool conflictWarningFound = false;
    QString error;
};

void closeActiveModalOnFailure()
{
    if ( auto* const modal = qobject_cast<QDialog*>( QApplication::activeModalWidget() ) ) {
        modal->reject();
    }
}

void scheduleCreateFavoriteAcceptance( QObject* context, const QString& favoriteName,
                                       FavoriteModalResult& result,
                                       const std::function<void()>& beforeAccept = {} )
{
    QTimer::singleShot( 0, Qt::PreciseTimer, context,
                        [ favoriteName, &result, beforeAccept ] {
                            auto* const dialog
                                = qobject_cast<QDialog*>( QApplication::activeModalWidget() );
                            if ( dialog == nullptr
                                 || dialog->objectName() != QStringLiteral( "SaveFavoriteDialog" ) ) {
                                result.error = QStringLiteral( "Save Favorite dialog was not active" );
                                closeActiveModalOnFailure();
                                return;
                            }

                            result.saveDialogFound = true;
                            auto* const nameEdit = dialog->findChild<QLineEdit*>(
                                QStringLiteral( "favoriteNameLineEdit" ) );
                            auto* const buttonBox = dialog->findChild<QDialogButtonBox*>(
                                QStringLiteral( "saveFavoriteButtonBox" ) );
                            auto* const ok = buttonBox != nullptr
                                                 ? buttonBox->button( QDialogButtonBox::Ok )
                                                 : nullptr;
                            if ( nameEdit == nullptr || ok == nullptr ) {
                                result.error
                                    = QStringLiteral( "Save Favorite controls were incomplete" );
                                dialog->reject();
                                return;
                            }

                            nameEdit->setText( favoriteName );
                            if ( beforeAccept ) {
                                beforeAccept();
                            }
                            ok->click();
                        } );
}

void requireFavorite( const FilterFavoritesModel::Collection& favorites, const QString& name,
                      const QString& pattern, bool useRegex )
{
    const auto found = std::find_if(
        favorites.cbegin(), favorites.cend(), [ &name ]( const PredefinedFilter& favorite ) {
            return favorite.name.compare( name, Qt::CaseInsensitive ) == 0;
        } );
    REQUIRE( found != favorites.cend() );
    REQUIRE( found->pattern == pattern );
    REQUIRE( found->useRegex == useRegex );
}

void scheduleOverwriteFavoriteAcceptance( QObject* context, const QString& targetName,
                                          FavoriteModalResult& result,
                                          const std::function<void()>& beforeAccept,
                                          int targetOccurrence = 0 )
{
    QTimer::singleShot( 0, Qt::PreciseTimer, context,
                        [ targetName, &result, beforeAccept, targetOccurrence ] {
                            auto* const dialog
                                = qobject_cast<QDialog*>( QApplication::activeModalWidget() );
                            if ( dialog == nullptr
                                 || dialog->objectName() != QStringLiteral( "SaveFavoriteDialog" ) ) {
                                result.error = QStringLiteral( "Save Favorite dialog was not active" );
                                closeActiveModalOnFailure();
                                return;
                            }

                            result.saveDialogFound = true;
                            auto* const overwrite = dialog->findChild<QRadioButton*>(
                                QStringLiteral( "overwriteFavoriteRadioButton" ) );
                            auto* const existing = dialog->findChild<QComboBox*>(
                                QStringLiteral( "existingFavoriteComboBox" ) );
                            auto* const buttonBox = dialog->findChild<QDialogButtonBox*>(
                                QStringLiteral( "saveFavoriteButtonBox" ) );
                            auto* const ok = buttonBox != nullptr
                                                 ? buttonBox->button( QDialogButtonBox::Ok )
                                                 : nullptr;
                            if ( overwrite == nullptr || existing == nullptr || ok == nullptr ) {
                                result.error
                                    = QStringLiteral( "Overwrite Favorite controls were incomplete" );
                                dialog->reject();
                                return;
                            }

                            int targetIndex = -1;
                            int matchingOccurrence = 0;
                            for ( int index = 0; index < existing->count(); ++index ) {
                                if ( existing->itemText( index ).compare(
                                         targetName, Qt::CaseInsensitive )
                                     != 0 ) {
                                    continue;
                                }
                                if ( matchingOccurrence == targetOccurrence ) {
                                    targetIndex = index;
                                    break;
                                }
                                ++matchingOccurrence;
                            }
                            if ( targetIndex < 0 ) {
                                result.error = QStringLiteral( "Overwrite target was not listed" );
                                dialog->reject();
                                return;
                            }

                            overwrite->setChecked( true );
                            existing->setCurrentIndex( targetIndex );
                            beforeAccept();
                            ok->click();
                        } );
}
} // namespace

TEST_CASE( "SearchToolbar constructs with null SavedSearches (folder mode)",
           "[searchtoolbar]" )
{
    SearchToolbar toolbar( nullptr, nullptr );
    REQUIRE( toolbar.searchLineEdit() != nullptr );
    REQUIRE( toolbar.currentSearchText().isEmpty() );
    // No history is populated when SavedSearches is null.
    REQUIRE( toolbar.searchLineEdit()->count() == 0 );
    REQUIRE( toolbar.searchLineCompleter() != nullptr );
}

TEST_CASE( "SearchToolbar pattern contract mirrors crawlerwidget.cpp:1976-1978",
           "[searchtoolbar]" )
{
    SearchToolbar toolbar( nullptr, nullptr );

    SECTION( "construction defaults: all toggles off (config defaults are applied by "
             "the host, not the toolbar)" )
    {
        toolbar.searchLineEdit()->setEditText( "ERROR" );
        const auto pattern = toolbar.currentRegularExpressionPattern();
        REQUIRE( pattern.pattern == "ERROR" );
        // matchCase unchecked => not case-sensitive
        REQUIRE( pattern.isCaseSensitive == false );
        REQUIRE( pattern.isExclude == false );
        REQUIRE( pattern.isBoolean == false );
        // isPlainText == !useRegexp. useRegexp defaults off => plainText true.
        REQUIRE( pattern.isPlainText == true );
        REQUIRE( toolbar.isUseRegexp() == false );
    }

    SECTION( "useRegexp on => isPlainText false" )
    {
        toolbar.searchLineEdit()->setEditText( "ERR" );
        toolbar.setUseRegexp( true );
        const auto pattern = toolbar.currentRegularExpressionPattern();
        REQUIRE( pattern.isPlainText == false );
        REQUIRE( toolbar.isUseRegexp() == true );
    }

    SECTION( "matchCase on => isCaseSensitive true" )
    {
        toolbar.searchLineEdit()->setEditText( "ERR" );
        toolbar.setMatchCase( true );
        REQUIRE( toolbar.currentRegularExpressionPattern().isCaseSensitive == true );
        REQUIRE( toolbar.isMatchCase() == true );
    }

    SECTION( "inverse on => isExclude true" )
    {
        toolbar.searchLineEdit()->setEditText( "ERR" );
        toolbar.setInverse( true );
        REQUIRE( toolbar.currentRegularExpressionPattern().isExclude == true );
        REQUIRE( toolbar.isInverse() == true );
    }

    SECTION( "boolean on => isBoolean true" )
    {
        toolbar.searchLineEdit()->setEditText( "ERR" );
        toolbar.setBoolean( true );
        REQUIRE( toolbar.currentRegularExpressionPattern().isBoolean == true );
        REQUIRE( toolbar.isBoolean() == true );
    }

    SECTION( "all flags combined round-trip through getters" )
    {
        toolbar.searchLineEdit()->setEditText( "combined" );
        toolbar.setMatchCase( true );
        toolbar.setUseRegexp( true );
        toolbar.setInverse( true );
        toolbar.setBoolean( true );

        const auto pattern = toolbar.currentRegularExpressionPattern();
        REQUIRE( pattern.pattern == "combined" );
        REQUIRE( pattern.isCaseSensitive == true );
        REQUIRE( pattern.isExclude == true );
        REQUIRE( pattern.isBoolean == true );
        // useRegexp on => plainText false
        REQUIRE( pattern.isPlainText == false );

        // getters mirror the setters
        REQUIRE( toolbar.isMatchCase() == true );
        REQUIRE( toolbar.isUseRegexp() == true );
        REQUIRE( toolbar.isInverse() == true );
        REQUIRE( toolbar.isBoolean() == true );
    }
}

TEST_CASE( "SearchToolbar escape/combine helpers (ports crawlerwidget.cpp:1155-1182)",
           "[searchtoolbar]" )
{
    SearchToolbar toolbar( nullptr, nullptr );

    SECTION( "escape wraps in quotes when boolean mode is on" )
    {
        toolbar.setBoolean( true );
        const auto escaped = toolbar.escapeSearchPattern( "error", false );
        // boolean mode: prepend/append '"'
        REQUIRE( escaped.startsWith( '"' ) );
        REQUIRE( escaped.endsWith( '"' ) );
    }

    SECTION( "boolean escape backslash-escapes embedded double-quotes" )
    {
        // Regression: escapeSearchPattern used to call replace('"',"\"") -- a
        // 1-char literal (just '"') so the replace was a no-op and an embedded
        // quote broke the wrapped boolean expression. The 2-char literal "\\\""
        // (backslash+quote) escapes it to \".
        toolbar.setBoolean( true );
        const auto escaped = toolbar.escapeSearchPattern( "a\"b", false );
        REQUIRE( escaped == "\"a\\\"b\"" );
    }

    SECTION( "combine joins regex with '|'" )
    {
        toolbar.setUseRegexp( true );
        toolbar.setBoolean( false );
        QString current = "foo";
        toolbar.combinePatterns( current, "bar" );
        REQUIRE( current == "foo|bar" );
    }

    SECTION( "combine joins boolean with ' or '" )
    {
        toolbar.setBoolean( true );
        QString current = "foo";
        toolbar.combinePatterns( current, "bar" );
        REQUIRE( current == "foo or bar" );
    }

    SECTION( "combine on empty current does not prepend a separator" )
    {
        toolbar.setUseRegexp( true );
        QString current;
        toolbar.combinePatterns( current, "first" );
        REQUIRE( current == "first" );
    }
}

TEST_CASE( "SearchToolbar wrapBooleanOperand quotes and escapes (excludeFromSearch path)",
           "[searchtoolbar]" )
{
    // CrawlerWidget::excludeFromSearch calls wrapBooleanOperand directly on the
    // current search text when transitioning into boolean mode -- the toggle may
    // still be off, so it can't rely on escapeSearchPattern's boolean branch.
    // This locks the operand-wrapping contract independently of the toggle.
    SearchToolbar toolbar( nullptr, nullptr );

    SECTION( "plain pattern is surrounded by quotes" )
    {
        REQUIRE( toolbar.wrapBooleanOperand( "error" ) == "\"error\"" );
    }

    SECTION( "embedded double-quote is backslash-escaped" )
    {
        // Regression: excludeFromSearch inlined replace('"',"\"") -- a no-op
        // 1-char literal -- so input a"b wrapped to the broken "a"b" instead of
        // the correct "a\"b".
        REQUIRE( toolbar.wrapBooleanOperand( "a\"b" ) == "\"a\\\"b\"" );
    }

    SECTION( "empty pattern becomes a pair of quotes" )
    {
        REQUIRE( toolbar.wrapBooleanOperand( "" ) == "\"\"" );
    }

    SECTION( "multiple embedded quotes are each escaped" )
    {
        REQUIRE( toolbar.wrapBooleanOperand( "\"quoted\"" ) == "\"\\\"quoted\\\"\"" );
    }
}

TEST_CASE( "wrapBooleanOperand escapes backslashes so the operand stays parseable",
           "[searchtoolbar]" )
{
    // Regression (CodeRabbit, searchtoolbar.cpp:284-295): a boolean operand
    // ending in a backslash used to wrap as "C:\Users\" -- the trailing '\'
    // makes the boolean quote parser treat the CLOSING quote as escaped, so the
    // whole operand became "unmatched quotes" and the search failed. Escape
    // backslashes first (C-style '\\'), then embedded quotes, so the closing
    // quote sits after an even backslash run and the parser sees a real closer.
    SearchToolbar toolbar( nullptr, nullptr );

    SECTION( "trailing backslash is doubled before wrapping" )
    {
        // C:\Users\  ->  "C:\\Users\\"  (backslash run before the closer is even)
        REQUIRE( toolbar.wrapBooleanOperand( "C:\\Users\\" ) == "\"C:\\\\Users\\\\\"" );
    }

    SECTION( "wrapped trailing-backslash operand parses and matches as boolean" )
    {
        const auto wrapped = toolbar.wrapBooleanOperand( "C:\\Users\\" );
        RegularExpression expression(
            RegularExpressionPattern( wrapped, false, false, true, true ) );
        REQUIRE( expression.isValid() );
        REQUIRE_FALSE( expression.errorString().contains( QLatin1String( "unmatched" ) ) );

        const auto matcher = expression.createMatcher();
        REQUIRE( matcher->hasMatch( std::string_view{ "path is C:\\Users\\" } ) );
        REQUIRE_FALSE( matcher->hasMatch( std::string_view{ "path is C:\\Users" } ) );
    }

    SECTION( "internal backslashes round-trip through the boolean parser" )
    {
        const auto wrapped = toolbar.wrapBooleanOperand( "C:\\Users\\admin" );
        RegularExpression expression(
            RegularExpressionPattern( wrapped, false, false, true, true ) );
        REQUIRE( expression.isValid() );

        const auto matcher = expression.createMatcher();
        REQUIRE( matcher->hasMatch( std::string_view{ "hello C:\\Users\\admin world" } ) );
    }
}

TEST_CASE( "SearchToolbar emits the expected signals", "[searchtoolbar]" )
{
    SearchToolbar toolbar( nullptr, nullptr );

    SECTION( "searchRequested fires on Search button click" )
    {
        QSignalSpy spy( &toolbar, &SearchToolbar::searchRequested );
        toolbar.searchButton()->click();
        pumpEvents();
        REQUIRE( spy.count() >= 1 );
    }

    SECTION( "stopRequested fires on Stop button click (button enabled first)" )
    {
        // stopButton is disabled until a search is in progress.
        toolbar.setSearchInProgress( true );
        QSignalSpy spy( &toolbar, &SearchToolbar::stopRequested );
        toolbar.stopButton()->click();
        pumpEvents();
        REQUIRE( spy.count() >= 1 );
    }

    SECTION( "optionsChanged fires on matchCase toggle" )
    {
        QSignalSpy spy( &toolbar, &SearchToolbar::optionsChanged );
        toolbar.matchCaseButton()->toggle();
        pumpEvents();
        REQUIRE( toolbar.isMatchCase() == true );
        REQUIRE( spy.count() >= 1 );
    }

    SECTION( "autoRefreshChanged fires on searchRefresh toggle" )
    {
        QSignalSpy spy( &toolbar, &SearchToolbar::autoRefreshChanged );
        toolbar.searchRefreshButton()->toggle();
        pumpEvents();
        REQUIRE( toolbar.isAutoRefresh() == true );
        REQUIRE( spy.count() >= 1 );
    }
}

TEST_CASE( "SearchToolbar setSearchInProgress button dance", "[searchtoolbar]" )
{
    SearchToolbar toolbar( nullptr, nullptr );

    // Use isHidden() (not isVisible()) because the parent toolbar is never
    // shown in this headless test; isHidden() directly reflects the show()/
    // hide() calls made by setSearchInProgress.
    REQUIRE( toolbar.stopButton()->isHidden() == true );
    REQUIRE( toolbar.searchButton()->isHidden() == false );

    SECTION( "busy shows+enables stop, hides search+clear" )
    {
        toolbar.setSearchInProgress( true );
        REQUIRE( toolbar.stopButton()->isHidden() == false );
        REQUIRE( toolbar.stopButton()->isEnabled() == true );
        REQUIRE( toolbar.searchButton()->isHidden() == true );
        REQUIRE( toolbar.clearButton()->isHidden() == true );
    }

    SECTION( "idle again hides+disables stop, shows search+clear" )
    {
        toolbar.setSearchInProgress( true );
        toolbar.setSearchInProgress( false );
        REQUIRE( toolbar.stopButton()->isHidden() == true );
        REQUIRE( toolbar.stopButton()->isEnabled() == false );
        REQUIRE( toolbar.searchButton()->isHidden() == false );
        REQUIRE( toolbar.clearButton()->isHidden() == false );
    }
}

TEST_CASE( "SearchToolbar session round-trip (view-context flags)", "[searchtoolbar]" )
{
    // Mirrors CrawlerWidgetContext persistence: matchCase/useRegexp/inverse/
    // boolean/autoRefresh must round-trip through the setters/getters.
    SearchToolbar toolbar( nullptr, nullptr );

    toolbar.setMatchCase( true );
    toolbar.setUseRegexp( true );
    toolbar.setInverse( true );
    toolbar.setBoolean( true );
    toolbar.setAutoRefresh( true );

    REQUIRE( toolbar.isMatchCase() == true );
    REQUIRE( toolbar.isUseRegexp() == true );
    REQUIRE( toolbar.isInverse() == true );
    REQUIRE( toolbar.isBoolean() == true );
    REQUIRE( toolbar.isAutoRefresh() == true );

    // toggle back to false
    toolbar.setMatchCase( false );
    toolbar.setUseRegexp( false );
    toolbar.setInverse( false );
    toolbar.setBoolean( false );
    toolbar.setAutoRefresh( false );

    REQUIRE( toolbar.isMatchCase() == false );
    REQUIRE( toolbar.isUseRegexp() == false );
    REQUIRE( toolbar.isInverse() == false );
    REQUIRE( toolbar.isBoolean() == false );
    REQUIRE( toolbar.isAutoRefresh() == false );
}

TEST_CASE( "New SearchToolbar synchronizes external filter favorite storage changes",
           "[searchtoolbar][filter-favorites]" )
{
    FilterFavoritesRestoreGuard restore(
        oneFavorite( QStringLiteral( "Memory" ), QStringLiteral( "memory-pattern" ) ) );
    auto& model = restore.model();
    const auto external
        = oneFavorite( QStringLiteral( "External" ), QStringLiteral( "external-pattern" ) );
    replaceStoredFavorites( external );

    SearchToolbar toolbar( nullptr, nullptr );

    REQUIRE( toolbar.predefinedFilters()->model() == &model );
    REQUIRE( toolbar.predefinedFilters()->count() == 1 );
    REQUIRE( toolbar.predefinedFilters()->itemText( 0 ) == QStringLiteral( "External" ) );
    requireFavorite( model.favorites(), QStringLiteral( "External" ),
                     QStringLiteral( "external-pattern" ), false );
}

TEST_CASE( "Opening a favorites popup synchronizes every bound SearchToolbar",
           "[searchtoolbar][filter-favorites]" )
{
    FilterFavoritesRestoreGuard restore(
        oneFavorite( QStringLiteral( "Initial" ), QStringLiteral( "initial-pattern" ) ) );
    auto& model = restore.model();
    SearchToolbar first( nullptr, nullptr );
    SearchToolbar second( nullptr, nullptr );
    first.show();
    second.show();
    REQUIRE( processEventsUntil( [ & ] { return first.isVisible() && second.isVisible(); } ) );

    const auto external
        = oneFavorite( QStringLiteral( "External" ), QStringLiteral( "external-pattern" ) );
    replaceStoredFavorites( external );
    QSignalSpy resetSpy( &model, &QAbstractItemModel::modelReset );

    first.predefinedFilters()->showPopup();
    const bool synchronized = processEventsUntil( [ & ] {
        return resetSpy.count() == 1 && first.predefinedFilters()->count() == 1
               && second.predefinedFilters()->count() == 1
               && first.predefinedFilters()->itemText( 0 ) == QStringLiteral( "External" )
               && second.predefinedFilters()->itemText( 0 ) == QStringLiteral( "External" );
    } );
    first.predefinedFilters()->hidePopup();

    REQUIRE( synchronized );
    requireFavorite( model.favorites(), QStringLiteral( "External" ),
                     QStringLiteral( "external-pattern" ), false );
}

TEST_CASE( "SearchToolbar instances observe the shared filter favorites model",
           "[searchtoolbar][filter-favorites]" )
{
    FilterFavoritesRestoreGuard restore(
        oneFavorite( QStringLiteral( "Short favorite" ), QStringLiteral( "short" ) ) );
    auto& model = restore.model();

    SearchToolbar first( nullptr, nullptr );
    SearchToolbar second( nullptr, nullptr );
    first.show();
    second.show();

    REQUIRE( processEventsUntil( [ & ] { return first.isVisible() && second.isVisible(); } ) );
    REQUIRE( first.predefinedFilters()->model() == &model );
    REQUIRE( second.predefinedFilters()->model() == &model );
    REQUIRE( first.predefinedFilters()->model() == second.predefinedFilters()->model() );

    first.predefinedFilters()->setCurrentIndex( 0 );
    second.predefinedFilters()->setCurrentIndex( 0 );
    REQUIRE( first.predefinedFilters()->currentIndex() == 0 );
    REQUIRE( second.predefinedFilters()->currentIndex() == 0 );

    QSignalSpy resetSpy( &model, &QAbstractItemModel::modelReset );
    const auto replacement = oneFavorite( QStringLiteral( "Replacement favorite" ),
                                          QStringLiteral( "replacement" ) );
    model.replaceFavorites( replacement );

    REQUIRE( processEventsUntil( [ & ] {
        return resetSpy.count() == 1 && first.predefinedFilters()->count() == 1
               && second.predefinedFilters()->count() == 1
               && first.predefinedFilters()->itemText( 0 ) == replacement.front().name
               && second.predefinedFilters()->itemText( 0 ) == replacement.front().name
               && first.predefinedFilters()->currentIndex() == -1
               && second.predefinedFilters()->currentIndex() == -1;
    } ) );
}

TEST_CASE( "Long filter favorite names do not consume the expanding search input",
           "[searchtoolbar][filter-favorites]" )
{
    const auto shortFavorites
        = oneFavorite( QStringLiteral( "Short" ), QStringLiteral( "short-pattern" ) );
    FilterFavoritesRestoreGuard restore( shortFavorites );
    auto& model = restore.model();

    SearchToolbar toolbar( nullptr, nullptr );
    auto* const favorites = toolbar.predefinedFilters();
    auto* const searchInput = toolbar.searchLineEdit();

    const auto initialHint = toolbar.sizeHint();
    const int fixedWidth = initialHint.width() + searchInput->sizeHint().width();
    toolbar.setFixedSize( fixedWidth, initialHint.height() );
    toolbar.show();

    REQUIRE( processEventsUntil( [ & ] {
        return toolbar.isVisible() && toolbar.width() == fixedWidth && favorites->width() > 0
               && searchInput->width() > 0;
    } ) );

    const int shortFavoriteSizeHintWidth = favorites->sizeHint().width();
    const int shortFavoriteWidth = favorites->width();
    const int shortSearchInputWidth = searchInput->width();

    const QString longName
        = QStringLiteral( "Several-hundred-character favorite " )
          + QString( 384, QLatin1Char( 'x' ) );
    model.replaceFavorites( oneFavorite( longName, QStringLiteral( "long-pattern" ) ) );

    REQUIRE( processEventsUntil( [ & ] {
        const auto index = model.index( 0, 0 );
        return model.data( index, Qt::DisplayRole ).toString() == longName
               && favorites->sizeHint().width() <= shortFavoriteSizeHintWidth
               && favorites->width() <= shortFavoriteWidth
               && searchInput->width() >= shortSearchInputWidth;
    } ) );

    const auto index = model.index( 0, 0 );
    REQUIRE( model.data( index, Qt::DisplayRole ).toString() == longName );
    REQUIRE( model.data( index, Qt::ToolTipRole ).toString() == longName );
    REQUIRE( favorites->sizeHint().width() <= shortFavoriteSizeHintWidth );
    REQUIRE( favorites->width() <= shortFavoriteWidth );
    REQUIRE( searchInput->width() >= shortSearchInputWidth );
    REQUIRE( toolbar.width() == fixedWidth );
    REQUIRE( favorites->currentIndex() == -1 );

    REQUIRE( favorites->view()->textElideMode() == Qt::ElideNone );
    QStyleOptionViewItem option;
    option.initFrom( favorites->view() );
    const int fullTextWidth = QFontMetrics( favorites->view()->font() ).horizontalAdvance( longName );
    const int delegateWidth
        = favorites->view()->itemDelegate()->sizeHint( option, model.index( 0, 0 ) ).width();
    REQUIRE( delegateWidth >= fullTextWidth );

    favorites->showPopup();
    const bool popupReady = processEventsUntil( [ & ] {
        return favorites->view()->isVisible()
               && favorites->view()->window()->width() > favorites->width()
               && favorites->view()->horizontalScrollBar()->maximum() > 0;
    } );
    const int popupWidth = favorites->view()->window()->width();
    const int viewportWidth = favorites->view()->viewport()->width();
    const int visualItemWidth = favorites->view()->visualRect( model.index( 0, 0 ) ).width();
    auto* const horizontalScrollBar = favorites->view()->horizontalScrollBar();
    const int horizontalMaximum = horizontalScrollBar->maximum();
    horizontalScrollBar->setValue( horizontalMaximum );
    const int horizontalValue = horizontalScrollBar->value();
    const auto horizontalPolicy = favorites->view()->horizontalScrollBarPolicy();
    favorites->hidePopup();

    CAPTURE( popupWidth, favorites->width(), viewportWidth, visualItemWidth,
             horizontalMaximum, horizontalValue, horizontalPolicy, delegateWidth,
             fullTextWidth );
    REQUIRE( popupReady );
    REQUIRE( popupWidth > favorites->width() );
    REQUIRE( horizontalMaximum > 0 );
    REQUIRE( horizontalValue == horizontalMaximum );
}

TEST_CASE( "Filter favorite popup uses full text width when the screen can fit it",
           "[searchtoolbar][filter-favorites]" )
{
    FilterFavoritesRestoreGuard restore( {} );
    auto& model = restore.model();
    SearchToolbar toolbar( nullptr, nullptr );
    toolbar.resize( 620, toolbar.sizeHint().height() );
    toolbar.show();
    REQUIRE( processEventsUntil( [ & ] { return toolbar.isVisible(); } ) );

    auto* const combo = toolbar.predefinedFilters();
    auto* screen = QGuiApplication::screenAt( combo->mapToGlobal( combo->rect().center() ) );
    if ( screen == nullptr ) {
        screen = QGuiApplication::primaryScreen();
    }
    REQUIRE( screen != nullptr );

    const QFontMetrics metrics( combo->view()->font() );
    QString longButFittingName = QStringLiteral( "Wide favorite " );
    const int targetTextWidth
        = std::min( screen->availableGeometry().width() / 2, combo->width() + 320 );
    while ( metrics.horizontalAdvance( longButFittingName ) < targetTextWidth ) {
        longButFittingName.append( QLatin1Char( 'W' ) );
    }
    REQUIRE( metrics.horizontalAdvance( longButFittingName )
             < screen->availableGeometry().width() );

    model.replaceFavorites(
        oneFavorite( longButFittingName, QStringLiteral( "wide-pattern" ) ) );
    combo->showPopup();
    const bool expanded = processEventsUntil( [ & ] {
        return combo->view()->isVisible() && combo->view()->window()->width() > combo->width()
               && combo->view()->window()->width()
                      >= metrics.horizontalAdvance( longButFittingName );
    } );
    const int popupWidth = combo->view()->window()->width();
    combo->hidePopup();

    REQUIRE( expanded );
    REQUIRE( popupWidth > combo->width() );
    REQUIRE( popupWidth >= metrics.horizontalAdvance( longButFittingName ) );
}

TEST_CASE( "SearchToolbar star and context action persist a favorite across toolbars",
           "[searchtoolbar][filter-favorites]" )
{
    const auto exerciseSave = []( bool useContextAction ) {
        FilterFavoritesRestoreGuard restore( {} );
        auto& model = restore.model();
        SearchToolbar first( nullptr, nullptr );
        SearchToolbar second( nullptr, nullptr );
        first.show();
        second.show();
        REQUIRE( processEventsUntil( [ & ] { return first.isVisible() && second.isVisible(); } ) );

        const QString pattern = useContextAction ? QStringLiteral( "context-pattern" )
                                                 : QStringLiteral( "star-pattern" );
        const QString favoriteName = useContextAction ? QStringLiteral( "Context Favorite" )
                                                      : QStringLiteral( "Star Favorite" );
        first.searchLineEdit()->setEditText( pattern );
        first.setUseRegexp( true );

        FavoriteModalResult modal;
        scheduleCreateFavoriteAcceptance( &first, favoriteName, modal );
        if ( useContextAction ) {
            auto* const action = first.findChild<QAction*>(
                QStringLiteral( "addFavoriteFilterAction" ) );
            REQUIRE( action != nullptr );
            action->trigger();
        }
        else {
            first.favoriteFilterButton()->click();
        }

        REQUIRE( modal.error.isEmpty() );
        REQUIRE( modal.saveDialogFound );
        REQUIRE( processEventsUntil( [ & ] {
            return model.rowCount() == 1 && second.predefinedFilters()->count() == 1
                   && second.predefinedFilters()->itemText( 0 ) == favoriteName;
        } ) );
        requireFavorite( model.favorites(), favoriteName, pattern, true );
        requireFavorite( PredefinedFiltersCollection::getSynced().getFilters(), favoriteName,
                         pattern, true );
        REQUIRE( QApplication::activeModalWidget() == nullptr );
    };

    SECTION( "star button" ) { exerciseSave( false ); }
    SECTION( "Add to Filter Favorites action" ) { exerciseSave( true ); }
}

TEST_CASE( "SearchToolbar reports duplicate names separately from concurrency conflicts",
           "[searchtoolbar][filter-favorites]" )
{
    const auto initial
        = oneFavorite( QStringLiteral( "Duplicate" ), QStringLiteral( "old-pattern" ) );
    FilterFavoritesRestoreGuard restore( initial );
    auto& model = restore.model();
    SearchToolbar toolbar( nullptr, nullptr );
    toolbar.show();
    REQUIRE( processEventsUntil( [ & ] { return toolbar.isVisible(); } ) );
    toolbar.searchLineEdit()->setEditText( QStringLiteral( "new-pattern" ) );

    FavoriteModalResult modal;
    QString warningText;
    [[maybe_unused]] const klogg::ui::ScopedMessageHandler messageHandler{
        [ &warningText ]( klogg::ui::MessageKind kind, QWidget*, const QString&,
                          const QString& text ) {
            if ( kind == klogg::ui::MessageKind::Warning ) {
                warningText = text;
            }
        } };
    scheduleCreateFavoriteAcceptance( &toolbar, QStringLiteral( "duplicate" ), modal );
    toolbar.favoriteFilterButton()->click();

    REQUIRE( modal.error.isEmpty() );
    REQUIRE( modal.saveDialogFound );
    REQUIRE( warningText.contains( QStringLiteral( "already exists" ) ) );
    REQUIRE_FALSE( warningText.contains( QStringLiteral( "changed while" ) ) );
    REQUIRE( model.favorites() == initial );
    REQUIRE( PredefinedFiltersCollection::getSynced().getFilters() == initial );
}

TEST_CASE( "SearchToolbar favorite creation merges concurrent unrelated favorites",
           "[searchtoolbar][filter-favorites]" )
{
    const auto initial
        = oneFavorite( QStringLiteral( "Initial" ), QStringLiteral( "initial-pattern" ) );
    FilterFavoritesRestoreGuard restore( initial );
    auto& model = restore.model();
    SearchToolbar first( nullptr, nullptr );
    SearchToolbar second( nullptr, nullptr );
    first.show();
    second.show();
    REQUIRE( processEventsUntil( [ & ] { return first.isVisible() && second.isVisible(); } ) );

    first.searchLineEdit()->setEditText( QStringLiteral( "created-pattern" ) );
    auto concurrent = initial;
    concurrent.push_back( { QStringLiteral( "Concurrent" ),
                            QStringLiteral( "concurrent-pattern" ), false } );

    FavoriteModalResult modal;
    scheduleCreateFavoriteAcceptance(
        &first, QStringLiteral( "Created" ), modal,
        [ & ] { replaceStoredFavorites( concurrent ); } );
    first.favoriteFilterButton()->click();

    REQUIRE( modal.error.isEmpty() );
    REQUIRE( modal.saveDialogFound );
    REQUIRE( processEventsUntil( [ & ] {
        return model.rowCount() == 3 && second.predefinedFilters()->count() == 3;
    } ) );
    requireFavorite( model.favorites(), QStringLiteral( "Initial" ),
                     QStringLiteral( "initial-pattern" ), false );
    requireFavorite( model.favorites(), QStringLiteral( "Concurrent" ),
                     QStringLiteral( "concurrent-pattern" ), false );
    requireFavorite( model.favorites(), QStringLiteral( "Created" ),
                     QStringLiteral( "created-pattern" ), false );

    const auto stored = PredefinedFiltersCollection::getSynced().getFilters();
    REQUIRE( stored.size() == 3 );
    requireFavorite( stored, QStringLiteral( "Concurrent" ),
                     QStringLiteral( "concurrent-pattern" ), false );
    requireFavorite( stored, QStringLiteral( "Created" ),
                     QStringLiteral( "created-pattern" ), false );
    REQUIRE( QApplication::activeModalWidget() == nullptr );
}

TEST_CASE( "SearchToolbar warns when an overwrite target vanishes during the modal",
           "[searchtoolbar][filter-favorites]" )
{
    FilterFavoritesModel::Collection initial{
        { QStringLiteral( "Target" ), QStringLiteral( "old-pattern" ), false },
        { QStringLiteral( "Unrelated" ), QStringLiteral( "unrelated-pattern" ), false } };
    FilterFavoritesRestoreGuard restore( initial );
    auto& model = restore.model();
    SearchToolbar first( nullptr, nullptr );
    SearchToolbar second( nullptr, nullptr );
    first.show();
    second.show();
    REQUIRE( processEventsUntil( [ & ] { return first.isVisible() && second.isVisible(); } ) );
    first.searchLineEdit()->setEditText( QStringLiteral( "new-pattern" ) );

    const FilterFavoritesModel::Collection concurrent{
        { QStringLiteral( "Unrelated" ), QStringLiteral( "unrelated-pattern" ), false } };
    FavoriteModalResult modal;
    [[maybe_unused]] const klogg::ui::ScopedMessageHandler messageHandler{
        [ &modal ]( klogg::ui::MessageKind kind, QWidget*, const QString&, const QString& ) {
            if ( kind == klogg::ui::MessageKind::Warning ) {
                modal.followupDialogFound = true;
                modal.conflictWarningFound = true;
            }
        } };
    scheduleOverwriteFavoriteAcceptance(
        &first, QStringLiteral( "Target" ), modal,
        [ & ] { replaceStoredFavorites( concurrent ); } );
    first.favoriteFilterButton()->click();

    REQUIRE( modal.error.isEmpty() );
    REQUIRE( modal.saveDialogFound );
    REQUIRE( modal.followupDialogFound );
    REQUIRE( modal.conflictWarningFound );
    REQUIRE( model.rowCount() == 1 );
    REQUIRE( second.predefinedFilters()->count() == 1 );
    requireFavorite( model.favorites(), QStringLiteral( "Unrelated" ),
                     QStringLiteral( "unrelated-pattern" ), false );
    const auto stored = PredefinedFiltersCollection::getSynced().getFilters();
    REQUIRE( stored.size() == 1 );
    requireFavorite( stored, QStringLiteral( "Unrelated" ),
                     QStringLiteral( "unrelated-pattern" ), false );
    REQUIRE( QApplication::activeModalWidget() == nullptr );
}

TEST_CASE( "SearchToolbar does not rebind a vanished duplicate-name overwrite target",
           "[searchtoolbar][filter-favorites]" )
{
    FilterFavoritesModel::Collection initial{
        { QStringLiteral( "Duplicate" ), QStringLiteral( "first-pattern" ), false },
        { QStringLiteral( "duplicate" ), QStringLiteral( "second-pattern" ), true },
        { QStringLiteral( "Unrelated" ), QStringLiteral( "unrelated-pattern" ), false } };
    FilterFavoritesRestoreGuard restore( initial );
    auto& model = restore.model();
    SearchToolbar toolbar( nullptr, nullptr );
    toolbar.show();
    REQUIRE( processEventsUntil( [ & ] { return toolbar.isVisible(); } ) );
    toolbar.searchLineEdit()->setEditText( QStringLiteral( "replacement-pattern" ) );

    const FilterFavoritesModel::Collection concurrent{
        initial.at( 0 ), initial.at( 2 ) };
    FavoriteModalResult modal;
    [[maybe_unused]] const klogg::ui::ScopedMessageHandler messageHandler{
        [ &modal ]( klogg::ui::MessageKind kind, QWidget*, const QString&, const QString& ) {
            if ( kind == klogg::ui::MessageKind::Warning ) {
                modal.followupDialogFound = true;
                modal.conflictWarningFound = true;
            }
        } };
    scheduleOverwriteFavoriteAcceptance(
        &toolbar, QStringLiteral( "Duplicate" ), modal,
        [ & ] { replaceStoredFavorites( concurrent ); }, 1 );
    toolbar.favoriteFilterButton()->click();

    REQUIRE( modal.error.isEmpty() );
    REQUIRE( modal.saveDialogFound );
    REQUIRE( modal.conflictWarningFound );
    REQUIRE( model.favorites() == concurrent );
    REQUIRE( PredefinedFiltersCollection::getSynced().getFilters() == concurrent );
}

TEST_CASE( "SearchToolbar overwrite preserves the selected duplicate-name target",
           "[searchtoolbar][filter-favorites]" )
{
    FilterFavoritesModel::Collection initial{
        { QStringLiteral( "Duplicate" ), QStringLiteral( "first-pattern" ), false },
        { QStringLiteral( "duplicate" ), QStringLiteral( "second-pattern" ), true },
        { QStringLiteral( "Unrelated" ), QStringLiteral( "unrelated-pattern" ), false } };
    FilterFavoritesRestoreGuard restore( initial );
    auto& model = restore.model();
    SearchToolbar first( nullptr, nullptr );
    SearchToolbar second( nullptr, nullptr );
    first.show();
    second.show();
    REQUIRE( processEventsUntil( [ & ] { return first.isVisible() && second.isVisible(); } ) );
    first.searchLineEdit()->setEditText( QStringLiteral( "replacement-pattern" ) );

    FavoriteModalResult modal;
    [[maybe_unused]] const klogg::ui::ScopedMessageHandler messageHandler{
        [ &modal ]( klogg::ui::MessageKind kind, QWidget*, const QString&, const QString& ) {
            if ( kind == klogg::ui::MessageKind::Warning ) {
                modal.conflictWarningFound = true;
            }
        } };
    [[maybe_unused]] const klogg::ui::ScopedDialogHandler dialogHandler{
        [ &modal ]( QDialog& ) {
            modal.followupDialogFound = true;
            return QDialog::Accepted;
        } };
    scheduleOverwriteFavoriteAcceptance( &first, QStringLiteral( "Duplicate" ), modal, [] {}, 1 );
    first.favoriteFilterButton()->click();

    REQUIRE( modal.error.isEmpty() );
    REQUIRE( modal.saveDialogFound );
    REQUIRE( modal.followupDialogFound );
    REQUIRE_FALSE( modal.conflictWarningFound );
    REQUIRE( processEventsUntil( [ & ] {
        return model.rowCount() == initial.size() && second.predefinedFilters()->count() == initial.size();
    } ) );

    const auto favorites = model.favorites();
    REQUIRE( favorites.at( 0 ) == initial.at( 0 ) );
    REQUIRE( favorites.at( 1 ).name == initial.at( 1 ).name );
    REQUIRE( favorites.at( 1 ).pattern == QStringLiteral( "replacement-pattern" ) );
    REQUIRE_FALSE( favorites.at( 1 ).useRegex );
    REQUIRE( favorites.at( 2 ) == initial.at( 2 ) );

    const auto stored = PredefinedFiltersCollection::getSynced().getFilters();
    REQUIRE( stored == favorites );
    REQUIRE( QApplication::activeModalWidget() == nullptr );
}

TEST_CASE( "SearchToolbar custom search context actions use exact labels",
           "[searchtoolbar][menu-text]" )
{
    SearchToolbar toolbar( nullptr, nullptr );
    QStringList actionTexts;
    const auto actions = toolbar.findChildren<QAction*>( QString(), Qt::FindDirectChildrenOnly );
    for ( const auto* action : actions ) {
        if ( !action->isSeparator() ) {
            actionTexts.append( action->text() );
        }
    }

    REQUIRE( actionTexts.contains( QStringLiteral( "Add to Filter Favorites..." ) ) );
    REQUIRE( actionTexts.contains( QStringLiteral( "Edit Search History..." ) ) );
    REQUIRE( actionTexts.contains( QStringLiteral( "Clear Search History" ) ) );
}
