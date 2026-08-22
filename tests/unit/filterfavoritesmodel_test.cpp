/*
 * Copyright (C) 2026
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

#include <catch2/catch.hpp>

#include <memory>

#include <QAbstractItemModel>
#include <QDialogButtonBox>
#include <QFile>
#include <QMap>
#include <QModelIndex>
#include <QLockFile>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QVariant>

#include "filterfavoritesmodel.h"
#include "persistentinfo.h"
#include "predefinedfilters.h"
#include "predefinedfilterscombobox.h"
#include "predefinedfiltersdialog.h"
#include "uimessage.h"

struct PredefinedFiltersCollectionTestAccess {
    static PredefinedFiltersCollection::CommitResult commitToSettings(
        QSettings& settings, const QString& lockFile,
        const PredefinedFiltersCollection::Collection& expected,
        const PredefinedFiltersCollection::Collection& replacement )
    {
        return PredefinedFiltersCollection::commitToSettings( settings, lockFile, expected,
                                                              replacement );
    }
};

namespace {
using Collection = PredefinedFiltersCollection::Collection;

Collection orderedFavorites()
{
    return { { QStringLiteral( "Bravo" ), QStringLiteral( "bravo-pattern" ), true },
             { QStringLiteral( "Alpha" ), QStringLiteral( "alpha-pattern" ), false },
             { QStringLiteral( "Charlie" ), QStringLiteral( "charlie-pattern" ), true } };
}

Collection twoFavorites()
{
    return { { QStringLiteral( "A" ), QStringLiteral( "first" ), false },
             { QStringLiteral( "B" ), QStringLiteral( "second" ), true } };
}

void requireFavoritesEqual( const Collection& actual, const Collection& expected )
{
    REQUIRE( actual.size() == expected.size() );
    for ( int i = 0; i < expected.size(); ++i ) {
        CAPTURE( i );
        REQUIRE( actual.at( i ).name == expected.at( i ).name );
        REQUIRE( actual.at( i ).pattern == expected.at( i ).pattern );
        REQUIRE( actual.at( i ).useRegex == expected.at( i ).useRegex );
    }
}

// Model tests exercise the process-wide Persistable singleton and therefore the
// user's real application QSettings. Snapshot the complete settings group, not
// merely the decoded collection, so stack unwinding restores the exact persisted
// representation and order even when a REQUIRE aborts the test.
class PersistedFavoritesGuard {
  public:
    PersistedFavoritesGuard()
    {
        auto& settings = PersistentInfo::getSettings( app_settings{} );
        settings.sync();
        settings.beginGroup( QStringLiteral( "PredefinedFiltersCollection" ) );
        const auto keys = settings.allKeys();
        for ( const auto& key : keys ) {
            savedValues_.insert( key, settings.value( key ) );
        }
        settings.endGroup();

        savedStoredFavorites_ = PredefinedFiltersCollection::getSynced().getFilters();
    }

    ~PersistedFavoritesGuard()
    {
        auto& settings = PersistentInfo::getSettings( app_settings{} );
        settings.beginGroup( QStringLiteral( "PredefinedFiltersCollection" ) );
        settings.remove( QString{} );
        for ( auto it = savedValues_.cbegin(); it != savedValues_.cend(); ++it ) {
            settings.setValue( it.key(), it.value() );
        }
        settings.endGroup();
        settings.sync();

        // An absent settings group does not clear Persistable's in-memory
        // collection, so restore the decoded storage snapshot explicitly too.
        auto& collection = PredefinedFiltersCollection::getSynced();
        collection.setFilters( savedStoredFavorites_ );
        FilterFavoritesModel::instance().synchronizeFromStorage();
    }

    PersistedFavoritesGuard( const PersistedFavoritesGuard& ) = delete;
    PersistedFavoritesGuard& operator=( const PersistedFavoritesGuard& ) = delete;

  private:
    QMap<QString, QVariant> savedValues_;
    Collection savedStoredFavorites_;
};

void replaceStoredFavorites( const Collection& favorites )
{
    PredefinedFiltersCollection::get().saveToStorage( favorites );
    auto& settings = PersistentInfo::getSettings( app_settings{} );
    settings.sync();
    REQUIRE( settings.status() == QSettings::NoError );
}

QTableWidget* filtersTable( PredefinedFiltersDialog& dialog )
{
    return dialog.findChild<QTableWidget*>( QStringLiteral( "filtersTableWidget" ) );
}

QPushButton* standardButton( PredefinedFiltersDialog& dialog,
                             QDialogButtonBox::StandardButton standardButton )
{
    auto* const box
        = dialog.findChild<QDialogButtonBox*>( QStringLiteral( "buttonBox" ) );
    return box != nullptr ? box->button( standardButton ) : nullptr;
}

struct CapturedMessage {
    klogg::ui::MessageKind kind = klogg::ui::MessageKind::Information;
    QString title;
    QString text;
    int count = 0;
};

klogg::ui::ScopedMessageHandler captureMessages( CapturedMessage& message )
{
    return klogg::ui::ScopedMessageHandler{
        [ &message ]( klogg::ui::MessageKind kind, QWidget*, const QString& title,
                      const QString& text ) {
            message.kind = kind;
            message.title = title;
            message.text = text;
            ++message.count;
        } };
}
} // namespace

TEST_CASE( "Scoped UI message handlers support reentrancy and non-LIFO cleanup",
           "[filter-favorites][ui-message]" )
{
    int outerCount = 0;
    int innerCount = 0;
    auto outer = std::make_unique<klogg::ui::ScopedMessageHandler>(
        [ & ]( klogg::ui::MessageKind, QWidget*, const QString&, const QString& ) {
            ++outerCount;
            [[maybe_unused]] const klogg::ui::ScopedMessageHandler nested{
                [ & ]( klogg::ui::MessageKind, QWidget*, const QString&, const QString& ) {
                    ++innerCount;
                } };
            klogg::ui::information( nullptr, QStringLiteral( "nested" ), QStringLiteral( "nested" ) );
        } );
    auto inner = std::make_unique<klogg::ui::ScopedMessageHandler>(
        [ & ]( klogg::ui::MessageKind, QWidget*, const QString&, const QString& ) {
            ++innerCount;
        } );

    outer.reset();
    klogg::ui::warning( nullptr, QStringLiteral( "inner" ), QStringLiteral( "inner" ) );
    REQUIRE( outerCount == 0 );
    REQUIRE( innerCount == 1 );

    inner.reset();
    [[maybe_unused]] const klogg::ui::ScopedMessageHandler reentrant{
        [ & ]( klogg::ui::MessageKind, QWidget*, const QString&, const QString& ) {
            ++outerCount;
            [[maybe_unused]] const klogg::ui::ScopedMessageHandler nested{
                [ & ]( klogg::ui::MessageKind, QWidget*, const QString&, const QString& ) {
                    ++innerCount;
                } };
            klogg::ui::information( nullptr, QStringLiteral( "nested" ), QStringLiteral( "nested" ) );
        } };
    klogg::ui::warning( nullptr, QStringLiteral( "outer" ), QStringLiteral( "outer" ) );
    REQUIRE( outerCount == 1 );
    REQUIRE( innerCount == 2 );
}

TEST_CASE( "Predefined filter settings roundtrip preserves insertion order",
           "[filter-favorites]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto settingsPath = dir.filePath( QStringLiteral( "favorites.ini" ) );
    const auto expected = orderedFavorites();

    {
        QSettings settings( settingsPath, QSettings::IniFormat );
        PredefinedFiltersCollection saved;
        saved.setFilters( expected );
        saved.saveToStorage( settings );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings settings( settingsPath, QSettings::IniFormat );
    PredefinedFiltersCollection restored;
    restored.retrieveFromStorage( settings );

    requireFavoritesEqual( restored.getFilters(), expected );
}

TEST_CASE( "Predefined filter file export and import preserves insertion order",
           "[filter-favorites]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto exportPath = dir.filePath( QStringLiteral( "favorites.ini" ) );
    const auto expected = orderedFavorites();

    REQUIRE( PredefinedFiltersCollection::saveToFile( exportPath, expected ) );
    requireFavoritesEqual( PredefinedFiltersCollection::loadFromFile( exportPath ), expected );
}

TEST_CASE( "Validated filter favorite import distinguishes empty and invalid files",
           "[filter-favorites]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );

    SECTION( "valid empty export succeeds" )
    {
        const auto path = dir.filePath( QStringLiteral( "empty.conf" ) );
        REQUIRE( PredefinedFiltersCollection::saveToFile( path, Collection{} ) );

        const auto result = PredefinedFiltersCollection::tryLoadFromFile( path );
        REQUIRE( result.status == PredefinedFiltersCollection::LoadStatus::Success );
        REQUIRE( result.filters.isEmpty() );
    }

    SECTION( "missing file is reported" )
    {
        const auto path = dir.filePath( QStringLiteral( "missing.conf" ) );
        const auto result = PredefinedFiltersCollection::tryLoadFromFile( path );
        REQUIRE( result.status == PredefinedFiltersCollection::LoadStatus::MissingFile );
        REQUIRE( result.filters.isEmpty() );
    }

    SECTION( "malformed file is reported" )
    {
        const auto path = dir.filePath( QStringLiteral( "malformed.conf" ) );
        QFile file( path );
        REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        REQUIRE( file.write( "[PredefinedFiltersCollection]\nversion=not-a-number\n" ) > 0 );
        file.close();

        const auto result = PredefinedFiltersCollection::tryLoadFromFile( path );
        REQUIRE( result.status == PredefinedFiltersCollection::LoadStatus::MalformedFile );
        REQUIRE( result.filters.isEmpty() );
    }

    SECTION( "unsupported version is reported" )
    {
        const auto path = dir.filePath( QStringLiteral( "future.conf" ) );
        QSettings settings( path, QSettings::IniFormat );
        settings.setValue( QStringLiteral( "PredefinedFiltersCollection/version" ), 999 );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );

        const auto result = PredefinedFiltersCollection::tryLoadFromFile( path );
        REQUIRE( result.status == PredefinedFiltersCollection::LoadStatus::UnsupportedVersion );
        REQUIRE( result.filters.isEmpty() );
    }
}

TEST_CASE( "Validated filter favorite import rejects unsafe array sizes",
           "[filter-favorites]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );

    for ( const int size : { -1, PredefinedFiltersCollection::MaximumFilterCount + 1 } ) {
        CAPTURE( size );
        const auto path = dir.filePath( QStringLiteral( "unsafe-%1.conf" ).arg( size ) );
        QSettings settings( path, QSettings::IniFormat );
        settings.setValue( QStringLiteral( "PredefinedFiltersCollection/version" ), 2 );
        settings.setValue( QStringLiteral( "PredefinedFiltersCollection/filters/size" ), size );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );

        const auto result = PredefinedFiltersCollection::tryLoadFromFile( path );
        REQUIRE( result.status == PredefinedFiltersCollection::LoadStatus::MalformedFile );
        REQUIRE( result.filters.isEmpty() );
    }
}

TEST_CASE( "Invalid persisted filter favorites keep the last valid collection",
           "[filter-favorites]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto path = dir.filePath( QStringLiteral( "favorites.conf" ) );
    QSettings settings( path, QSettings::IniFormat );
    const auto initial = twoFavorites();

    PredefinedFiltersCollection collection;
    collection.setFilters( initial );
    collection.saveToStorage( settings );
    settings.sync();
    collection.retrieveFromStorage( settings );
    requireFavoritesEqual( collection.getFilters(), initial );

    settings.setValue( QStringLiteral( "PredefinedFiltersCollection/filters/size" ),
                       PredefinedFiltersCollection::MaximumFilterCount + 1 );
    settings.sync();
    collection.retrieveFromStorage( settings );

    requireFavoritesEqual( collection.getFilters(), initial );
}

TEST_CASE( "Filter favorite storage commit is locked and compare-and-replace",
           "[filter-favorites]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto settingsPath = dir.filePath( QStringLiteral( "favorites.conf" ) );
    const auto lockPath = dir.filePath( QStringLiteral( "favorites.lock" ) );
    QSettings settings( settingsPath, QSettings::IniFormat );

    const auto initial = twoFavorites();
    PredefinedFiltersCollection seed;
    seed.setFilters( initial );
    seed.saveToStorage( settings );
    settings.sync();
    REQUIRE( settings.status() == QSettings::NoError );

    const auto replacement = orderedFavorites();
    const auto success = PredefinedFiltersCollectionTestAccess::commitToSettings(
        settings, lockPath, initial, replacement );
    REQUIRE( success.status == PredefinedFiltersCollection::CommitStatus::Success );
    requireFavoritesEqual( success.storedFilters, replacement );

    const auto conflict = PredefinedFiltersCollectionTestAccess::commitToSettings(
        settings, lockPath, initial, Collection{} );
    REQUIRE( conflict.status == PredefinedFiltersCollection::CommitStatus::Conflict );
    requireFavoritesEqual( conflict.storedFilters, replacement );

    {
        QLockFile heldLock( lockPath );
        REQUIRE( heldLock.tryLock() );
        const auto lockError = PredefinedFiltersCollectionTestAccess::commitToSettings(
            settings, lockPath, replacement, Collection{} );
        REQUIRE( lockError.status == PredefinedFiltersCollection::CommitStatus::LockError );
    }

    settings.setValue( QStringLiteral( "PredefinedFiltersCollection/filters/size" ),
                       PredefinedFiltersCollection::MaximumFilterCount + 1 );
    settings.sync();
    const auto repaired = PredefinedFiltersCollectionTestAccess::commitToSettings(
        settings, lockPath, replacement, initial );
    REQUIRE( repaired.status == PredefinedFiltersCollection::CommitStatus::Success );

    PredefinedFiltersCollection restored;
    restored.retrieveFromStorage( settings );
    requireFavoritesEqual( restored.getFilters(), initial );
}

TEST_CASE( "Failed filter favorite commit is not published by the shared model",
           "[filter-favorites]" )
{
    PersistedFavoritesGuard guard;
    auto& model = FilterFavoritesModel::instance();
    const auto initial = twoFavorites();
    model.replaceFavorites( initial );
    QSignalSpy resetSpy( &model, &QAbstractItemModel::modelReset );

    Collection oversized;
    oversized.reserve( PredefinedFiltersCollection::MaximumFilterCount + 1 );
    const PredefinedFilter favorite{ QStringLiteral( "Name" ), QStringLiteral( "Pattern" ), false };
    for ( int index = 0; index <= PredefinedFiltersCollection::MaximumFilterCount; ++index ) {
        oversized.push_back( favorite );
    }

    const auto result = model.replaceFavorites( oversized );

    REQUIRE( result.status
             == PredefinedFiltersCollection::CommitStatus::InvalidReplacement );
    REQUIRE( resetSpy.count() == 0 );
    requireFavoritesEqual( model.favorites(), initial );
    requireFavoritesEqual( PredefinedFiltersCollection::getSynced().getFilters(), initial );
}

TEST_CASE( "Filter favorites model exposes ordered rows and roles", "[filter-favorites]" )
{
    PersistedFavoritesGuard guard;
    auto& model = FilterFavoritesModel::instance();
    const auto expected = orderedFavorites();

    model.replaceFavorites( expected );

    requireFavoritesEqual( model.favorites(), expected );
    REQUIRE( model.rowCount( QModelIndex{} ) == expected.size() );
    REQUIRE( model.roleNames().value( FilterFavoritesModel::NameRole ) == QByteArray( "name" ) );
    REQUIRE( model.roleNames().value( FilterFavoritesModel::PatternRole )
             == QByteArray( "pattern" ) );
    REQUIRE( model.roleNames().value( FilterFavoritesModel::RegexRole ) == QByteArray( "regex" ) );

    for ( int row = 0; row < expected.size(); ++row ) {
        CAPTURE( row );
        const auto index = model.index( row, 0, QModelIndex{} );
        REQUIRE( index.isValid() );
        REQUIRE( model.data( index, FilterFavoritesModel::NameRole ).toString()
                 == expected.at( row ).name );
        REQUIRE( model.data( index, FilterFavoritesModel::PatternRole ).toString()
                 == expected.at( row ).pattern );
        REQUIRE( model.data( index, FilterFavoritesModel::RegexRole ).toBool()
                 == expected.at( row ).useRegex );
    }
}

TEST_CASE( "Stale filter favorites replacement reports conflict and publishes durable state",
           "[filter-favorites]" )
{
    PersistedFavoritesGuard guard;
    auto& model = FilterFavoritesModel::instance();
    const auto expected = twoFavorites();
    model.replaceFavorites( expected );
    const auto concurrent = orderedFavorites();
    replaceStoredFavorites( concurrent );
    QSignalSpy resetSpy( &model, &QAbstractItemModel::modelReset );

    const auto result = model.replaceFavorites( expected );

    REQUIRE( result.status == PredefinedFiltersCollection::CommitStatus::Conflict );
    REQUIRE( resetSpy.count() == 1 );
    requireFavoritesEqual( model.favorites(), concurrent );
    requireFavoritesEqual( PredefinedFiltersCollection::getSynced().getFilters(), concurrent );
}

TEST_CASE( "Identical model and storage replacement emits no change signals",
           "[filter-favorites]" )
{
    PersistedFavoritesGuard guard;
    auto& model = FilterFavoritesModel::instance();
    const auto favorites = orderedFavorites();
    model.replaceFavorites( favorites );
    model.synchronizeFromStorage();

    QSignalSpy resetSpy( &model, &QAbstractItemModel::modelReset );
    QSignalSpy dataSpy( &model, &QAbstractItemModel::dataChanged );
    QSignalSpy insertedSpy( &model, &QAbstractItemModel::rowsInserted );
    QSignalSpy removedSpy( &model, &QAbstractItemModel::rowsRemoved );

    model.replaceFavorites( favorites );

    REQUIRE( resetSpy.count() == 0 );
    REQUIRE( dataSpy.count() == 0 );
    REQUIRE( insertedSpy.count() == 0 );
    REQUIRE( removedSpy.count() == 0 );
    requireFavoritesEqual( model.favorites(), favorites );
    requireFavoritesEqual( PredefinedFiltersCollection::getSynced().getFilters(), favorites );
}

TEST_CASE( "Replacing changed filter favorites emits one model reset", "[filter-favorites]" )
{
    PersistedFavoritesGuard guard;
    auto& model = FilterFavoritesModel::instance();
    model.replaceFavorites( twoFavorites() );
    QSignalSpy resetSpy( &model, &QAbstractItemModel::modelReset );

    model.replaceFavorites( orderedFavorites() );

    REQUIRE( resetSpy.count() == 1 );
}

TEST_CASE( "Replacing identical filter favorites emits no model reset", "[filter-favorites]" )
{
    PersistedFavoritesGuard guard;
    auto& model = FilterFavoritesModel::instance();
    const auto favorites = orderedFavorites();
    model.replaceFavorites( favorites );
    QSignalSpy resetSpy( &model, &QAbstractItemModel::modelReset );

    model.replaceFavorites( favorites );

    REQUIRE( resetSpy.count() == 0 );
}

TEST_CASE( "Reordering filter favorites persists the new order", "[filter-favorites]" )
{
    PersistedFavoritesGuard guard;
    auto& model = FilterFavoritesModel::instance();
    const auto initial = twoFavorites();
    const Collection reordered{ initial.at( 1 ), initial.at( 0 ) };

    model.replaceFavorites( initial );
    model.replaceFavorites( reordered );

    requireFavoritesEqual( PredefinedFiltersCollection::getSynced().getFilters(), reordered );

    model.synchronizeFromStorage();
    requireFavoritesEqual( model.favorites(), reordered );
}

TEST_CASE( "Filter favorite export reports QSettings write failures", "[filter-favorites]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );

    // An existing directory cannot be replaced by an INI file. QSettings only
    // exposes the failure after sync(), so saveToFile must synchronize and check
    // status rather than unconditionally reporting success.
    REQUIRE_FALSE( PredefinedFiltersCollection::saveToFile( dir.path(), orderedFavorites() ) );
}

TEST_CASE( "Filter favorites dialog rejects a concurrent full-table overwrite",
           "[filter-favorites][predefined-filters-dialog]" )
{
    PersistedFavoritesGuard guard;
    auto& model = FilterFavoritesModel::instance();
    const auto base = twoFavorites();
    model.replaceFavorites( base );

    PredefinedFiltersDialog dialog;
    auto* const table = filtersTable( dialog );
    auto* const apply = standardButton( dialog, QDialogButtonBox::Apply );
    REQUIRE( table != nullptr );
    REQUIRE( apply != nullptr );
    REQUIRE( table->rowCount() == base.size() );
    table->item( 0, 1 )->setText( QStringLiteral( "dialog-edit" ) );

    const Collection concurrent{
        { QStringLiteral( "External" ), QStringLiteral( "external-pattern" ), false } };
    replaceStoredFavorites( concurrent );

    CapturedMessage warning;
    [[maybe_unused]] const auto messageHandler = captureMessages( warning );
    apply->click();

    REQUIRE( warning.count == 1 );
    REQUIRE( warning.kind == klogg::ui::MessageKind::Warning );
    REQUIRE_FALSE( warning.text.isEmpty() );
    REQUIRE( dialog.result() == 0 );
    REQUIRE( table->item( 0, 1 )->text() == QStringLiteral( "dialog-edit" ) );
    requireFavoritesEqual( model.favorites(), concurrent );
    requireFavoritesEqual( PredefinedFiltersCollection::getSynced().getFilters(), concurrent );
}

TEST_CASE( "Filter favorites dialog updates visible and hidden pickers after every Apply",
           "[filter-favorites][predefined-filters-dialog]" )
{
    PersistedFavoritesGuard guard;
    auto& model = FilterFavoritesModel::instance();
    const auto initial = twoFavorites();
    model.replaceFavorites( initial );

    PredefinedFiltersComboBox visiblePicker( nullptr );
    PredefinedFiltersComboBox hiddenPicker( nullptr );
    visiblePicker.show();
    hiddenPicker.hide();

    PredefinedFiltersDialog dialog;
    auto* const table = filtersTable( dialog );
    auto* const apply = standardButton( dialog, QDialogButtonBox::Apply );
    auto* const add = dialog.findChild<QToolButton*>( QStringLiteral( "addFilterButton" ) );
    auto* const remove
        = dialog.findChild<QToolButton*>( QStringLiteral( "removeFilterButton" ) );
    auto* const up = dialog.findChild<QToolButton*>( QStringLiteral( "upButton" ) );
    REQUIRE( table != nullptr );
    REQUIRE( apply != nullptr );
    REQUIRE( add != nullptr );
    REQUIRE( remove != nullptr );
    REQUIRE( up != nullptr );

    const auto requirePickers = [ & ]( const Collection& expected ) {
        requireFavoritesEqual( model.favorites(), expected );
        REQUIRE( visiblePicker.count() == expected.size() );
        REQUIRE( hiddenPicker.count() == expected.size() );
        for ( int index = 0; index < expected.size(); ++index ) {
            CAPTURE( index );
            CHECK( visiblePicker.itemText( index ) == expected.at( index ).name );
            CHECK( hiddenPicker.itemText( index ) == expected.at( index ).name );
        }
    };

    add->click();
    const int addedRow = table->rowCount() - 1;
    table->item( addedRow, 0 )->setText( QStringLiteral( "Gamma" ) );
    table->item( addedRow, 1 )->setText( QStringLiteral( "third" ) );
    apply->click();
    Collection expected{ initial };
    expected.push_back( { QStringLiteral( "Gamma" ), QStringLiteral( "third" ), false } );
    requirePickers( expected );

    table->item( addedRow, 0 )->setText( QStringLiteral( "Gamma Prime" ) );
    apply->click();
    expected[ 2 ].name = QStringLiteral( "Gamma Prime" );
    requirePickers( expected );

    table->setCurrentCell( addedRow, 0 );
    up->click();
    up->click();
    apply->click();
    expected.move( 2, 0 );
    requirePickers( expected );

    table->setCurrentCell( 2, 0 );
    remove->click();
    apply->click();
    expected.removeAt( 2 );
    requirePickers( expected );
}

TEST_CASE( "Filter favorites dialog advances its conflict base after Apply",
           "[filter-favorites][predefined-filters-dialog]" )
{
    PersistedFavoritesGuard guard;
    auto& model = FilterFavoritesModel::instance();
    const auto base = twoFavorites();
    model.replaceFavorites( base );

    PredefinedFiltersDialog dialog;
    auto* const table = filtersTable( dialog );
    auto* const apply = standardButton( dialog, QDialogButtonBox::Apply );
    REQUIRE( table != nullptr );
    REQUIRE( apply != nullptr );

    table->item( 0, 1 )->setText( QStringLiteral( "first-apply" ) );
    apply->click();
    auto firstApplied = base;
    firstApplied[ 0 ].pattern = QStringLiteral( "first-apply" );
    requireFavoritesEqual( model.favorites(), firstApplied );

    table->item( 0, 1 )->setText( QStringLiteral( "second-apply" ) );
    apply->click();
    auto secondApplied = base;
    secondApplied[ 0 ].pattern = QStringLiteral( "second-apply" );
    requireFavoritesEqual( model.favorites(), secondApplied );
    requireFavoritesEqual( PredefinedFiltersCollection::getSynced().getFilters(), secondApplied );
}

TEST_CASE( "Filter favorites dialog preserves its table on invalid import",
           "[filter-favorites][predefined-filters-dialog]" )
{
    PersistedFavoritesGuard guard;
    FilterFavoritesModel::instance().replaceFavorites( twoFavorites() );

    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto invalidPath = dir.filePath( QStringLiteral( "malformed.conf" ) );
    QFile invalidFile( invalidPath );
    REQUIRE( invalidFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( invalidFile.write( "[PredefinedFiltersCollection]\nversion=not-a-number\n" ) > 0 );
    invalidFile.close();

    PredefinedFiltersDialog dialog;
    auto* const table = filtersTable( dialog );
    REQUIRE( table != nullptr );
    REQUIRE( table->rowCount() == 2 );
    const auto originalFirstName = table->item( 0, 0 )->text();
    const auto originalFirstPattern = table->item( 0, 1 )->text();

    CapturedMessage warning;
    [[maybe_unused]] const auto messageHandler = captureMessages( warning );
    const bool invoked = QMetaObject::invokeMethod(
        &dialog, "importFiltersFromFile", Qt::DirectConnection, Q_ARG( QString, invalidPath ) );

    REQUIRE( invoked );
    REQUIRE( warning.count == 1 );
    REQUIRE( warning.kind == klogg::ui::MessageKind::Warning );
    REQUIRE_FALSE( warning.text.isEmpty() );
    REQUIRE( table->rowCount() == 2 );
    REQUIRE( table->item( 0, 0 )->text() == originalFirstName );
    REQUIRE( table->item( 0, 1 )->text() == originalFirstPattern );
}

TEST_CASE( "Filter favorites dialog reports invalid export destinations",
           "[filter-favorites][predefined-filters-dialog]" )
{
    PersistedFavoritesGuard guard;
    FilterFavoritesModel::instance().replaceFavorites( twoFavorites() );

    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    PredefinedFiltersDialog dialog;

    CapturedMessage warning;
    [[maybe_unused]] const auto messageHandler = captureMessages( warning );
    const bool invoked = QMetaObject::invokeMethod(
        &dialog, "exportFiltersToFile", Qt::DirectConnection, Q_ARG( QString, dir.path() ) );

    REQUIRE( invoked );
    REQUIRE( warning.count == 1 );
    REQUIRE( warning.kind == klogg::ui::MessageKind::Warning );
    REQUIRE_FALSE( warning.text.isEmpty() );
}
