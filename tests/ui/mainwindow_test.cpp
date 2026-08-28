/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
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

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <QClipboard>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTabBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUuid>
#include <QVariant>

#include <QToolBar>

#include "test_utils.h"

#include "adblogcatsource.h"
#include "capturestore.h"
#include "crawlerwidget.h"
#include "favoritefiles.h"
#include "filterfavoritesmodel.h"
#include "foldercrawlerwidget.h"
#include "folderfilteredview.h"
#include "livelogsession.h"
#include "log.h"
#include "mainwindow.h"
#include "persistentinfo.h"
#include "predefinedfilters.h"
#include "predefinedfilterscombobox.h"
#include "session.h"
#include "sessioninfo.h"
#include "tabgroup.h"
#include "uimessage.h"

namespace {
QString makeTestDir( const QString& prefix )
{
    const auto dirPath = QDir::cleanPath(
        QDir::currentPath() + QDir::separator() + QLatin1String( "test_tmp" ) + QDir::separator()
        + prefix + QLatin1Char( '_' ) + QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    QDir{}.mkpath( dirPath );
    return dirPath;
}

void clearPersistedTabGroups()
{
    auto& settings = PersistentInfo::getSettings( app_settings{} );
    settings.remove( "tabGroups" );
    settings.sync();
    TabGroupManager::getSynced();
}

struct TabGroupCleanupGuard {
    TabGroupCleanupGuard()
    {
        clearPersistedTabGroups();
    }

    ~TabGroupCleanupGuard()
    {
        clearPersistedTabGroups();
    }
};

// RAII save/restore of the file-watch flags gating followAction's enabled
// state. MainWindow reads anyFileWatchEnabled() when the action is created
// (and on every tab switch via disableFileSpecificActions), so the guard must
// be installed before the window under test is constructed. Uses the
// in-memory Configuration singleton so nothing leaks to disk or sibling tests.
struct FileWatchConfigGuard {
    FileWatchConfigGuard()
        : config_( Configuration::get() )
        , previousNative_( config_.nativeFileWatchEnabled() )
        , previousPolling_( config_.pollingEnabled() )
    {
        // Force polling (not native): anyFileWatchEnabled() is native||polling,
        // and polling is the watcher the test harness itself keeps enabled on
        // every platform for determinism (qtests_main disables the flaky
        // native efsw watcher on Windows -- re-arming it here would put these
        // scenarios into exactly the configuration the harness avoids).
        config_.setPollingEnabled( true );
    }

    ~FileWatchConfigGuard()
    {
        config_.setNativeFileWatchEnabled( previousNative_ );
        config_.setPollingEnabled( previousPolling_ );
    }

    FileWatchConfigGuard( const FileWatchConfigGuard& ) = delete;
    FileWatchConfigGuard& operator=( const FileWatchConfigGuard& ) = delete;

private:
    Configuration& config_;
    bool previousNative_;
    bool previousPolling_;
};

struct SessionInfoWindowSnapshot {
    QString id;
    QByteArray geometry;
    int currentFileIndex = -1;
    std::vector<SessionInfo::OpenFile> openFiles;
};

class SessionInfoRestoreGuard {
public:
    explicit SessionInfoRestoreGuard( SessionInfo& sessionInfo )
        : sessionInfo_{ sessionInfo }
    {
        for ( const auto& windowId : sessionInfo_.windows() ) {
            snapshots_.push_back( { windowId, sessionInfo_.geometry( windowId ),
                                    sessionInfo_.currentFileIndex( windowId ),
                                    sessionInfo_.openFiles( windowId ) } );
        }
    }

    ~SessionInfoRestoreGuard()
    {
        QStringList originalWindowIds;
        for ( const auto& snapshot : snapshots_ ) {
            originalWindowIds.push_back( snapshot.id );
            sessionInfo_.add( snapshot.id );
            sessionInfo_.setGeometry( snapshot.id, snapshot.geometry );
            sessionInfo_.setCurrentFileIndex( snapshot.id, snapshot.currentFileIndex );
            sessionInfo_.setOpenFiles( snapshot.id, snapshot.openFiles );
        }

        for ( const auto& windowId : sessionInfo_.windows() ) {
            if ( !originalWindowIds.contains( windowId ) ) {
                sessionInfo_.remove( windowId );
            }
        }

        if ( snapshots_.empty() ) {
            for ( const auto& windowId : sessionInfo_.windows() ) {
                sessionInfo_.setGeometry( windowId, {} );
                sessionInfo_.setCurrentFileIndex( windowId, -1 );
                sessionInfo_.setOpenFiles( windowId, {} );
            }
        }

        sessionInfo_.save();
    }

private:
    SessionInfo& sessionInfo_;
    std::vector<SessionInfoWindowSnapshot> snapshots_;
};

QToolButton* findGroupChipButton( QTabBar* tabBar, int tabIndex )
{
    if ( tabBar == nullptr || tabIndex < 0 || tabIndex >= tabBar->count() ) {
        return nullptr;
    }

    for ( const auto side : { QTabBar::LeftSide, QTabBar::RightSide } ) {
        if ( auto* chip = qobject_cast<QToolButton*>( tabBar->tabButton( tabIndex, side ) );
             chip != nullptr && !chip->text().isEmpty() ) {
            return chip;
        }
    }

    return nullptr;
}

class FilterFavoritesRestoreGuard {
public:
    explicit FilterFavoritesRestoreGuard(
        const PredefinedFiltersCollection::Collection& seededFilters )
        : model_( FilterFavoritesModel::instance() )
        , savedStoredFavorites_( PredefinedFiltersCollection::getSynced().getFilters() )
    {
        auto& settings = PersistentInfo::getSettings( app_settings{} );
        settings.sync();
        settings.beginGroup( QStringLiteral( "PredefinedFiltersCollection" ) );
        for ( const auto& key : settings.allKeys() ) {
            savedSettingsValues_.insert( key, settings.value( key ) );
        }
        settings.endGroup();

        PredefinedFiltersCollection::get().saveToStorage( seededFilters );
        model_.synchronizeFromStorage();
    }

    ~FilterFavoritesRestoreGuard()
    {
        auto& settings = PersistentInfo::getSettings( app_settings{} );
        settings.beginGroup( QStringLiteral( "PredefinedFiltersCollection" ) );
        settings.remove( QString{} );
        for ( auto item = savedSettingsValues_.cbegin(); item != savedSettingsValues_.cend();
              ++item ) {
            settings.setValue( item.key(), item.value() );
        }
        settings.endGroup();
        settings.sync();

        auto& collection = PredefinedFiltersCollection::getSynced();
        collection.setFilters( savedStoredFavorites_ );
        model_.synchronizeFromStorage();
    }

    FilterFavoritesRestoreGuard( const FilterFavoritesRestoreGuard& ) = delete;
    FilterFavoritesRestoreGuard& operator=( const FilterFavoritesRestoreGuard& ) = delete;

private:
    FilterFavoritesModel& model_;
    PredefinedFiltersCollection::Collection savedStoredFavorites_;
    QMap<QString, QVariant> savedSettingsValues_;
};

void replaceFavoriteFiles( const QStringList& paths )
{
    auto& favorites = FavoriteFiles::getSynced();
    for ( const auto& favorite : favorites.favorites() ) {
        favorites.remove( favorite.fullPath() );
    }
    for ( const auto& path : paths ) {
        favorites.add( path );
    }
    favorites.save();
}

class FavoriteFilesRestoreGuard {
public:
    explicit FavoriteFilesRestoreGuard( const QStringList& seededPaths )
    {
        for ( const auto& favorite : FavoriteFiles::getSynced().favorites() ) {
            savedPaths_.push_back( favorite.fullPath() );
        }
        replaceFavoriteFiles( seededPaths );
    }

    ~FavoriteFilesRestoreGuard()
    {
        replaceFavoriteFiles( savedPaths_ );
    }

    FavoriteFilesRestoreGuard( const FavoriteFilesRestoreGuard& ) = delete;
    FavoriteFilesRestoreGuard& operator=( const FavoriteFilesRestoreGuard& ) = delete;

private:
    QStringList savedPaths_;
};

QString visibleMenuText( QString text )
{
    const auto escapedAmpersand = QStringLiteral( "\x01" );
    text.replace( QStringLiteral( "&&" ), escapedAmpersand );
    text.remove( QLatin1Char( '&' ) );
    text.replace( escapedAmpersand, QStringLiteral( "&" ) );
    return text;
}

QMenu* findTopLevelMenu( MainWindow* mainWindow, const QString& visibleTitle )
{
    for ( auto* action : mainWindow->menuBar()->actions() ) {
        if ( auto* menu = action->menu();
             menu != nullptr && visibleMenuText( action->text() ) == visibleTitle ) {
            return menu;
        }
    }
    return nullptr;
}

PredefinedFiltersCollection::Collection comboFavoriteRows( const QComboBox* combo )
{
    PredefinedFiltersCollection::Collection favorites;
    if ( combo == nullptr || combo->model() == nullptr ) {
        return favorites;
    }

    for ( int row = 0; row < combo->model()->rowCount(); ++row ) {
        const auto index = combo->model()->index( row, 0 );
        favorites.push_back( { index.data( FilterFavoritesModel::NameRole ).toString(),
                               index.data( FilterFavoritesModel::PatternRole ).toString(),
                               index.data( FilterFavoritesModel::RegexRole ).toBool() } );
    }
    return favorites;
}

QStringList favoriteActionPaths( const QMenu* menu )
{
    QStringList paths;
    if ( menu == nullptr ) {
        return paths;
    }
    for ( auto* action : menu->actions() ) {
        if ( action->data().isValid() && !action->data().toString().isEmpty() ) {
            paths.push_back( action->data().toString() );
        }
    }
    return paths;
}
} // namespace

SCENARIO( "Main window tests", "[ui]" )
{
    auto appSession = std::make_shared<Session>();
    WindowSession windowSession{ appSession, "Main", 0 };

    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<SafeQSignalSpy> activateSpy;
    std::unique_ptr<SafeQSignalSpy> exitSpy;
    QTimer::singleShot( 0, [ & ] {
        LOG_INFO << "Initialize main window";
        mainWindow.reset( new MainWindow( windowSession ) );
        exitSpy.reset( new SafeQSignalSpy( mainWindow.get(), SIGNAL( exitRequested() ) ) );
        activateSpy.reset( new SafeQSignalSpy( mainWindow.get(), SIGNAL( windowActivated() ) ) );
    } );

    QTest::qWait( 100 );
    mainWindow->show();
    QTest::qWait( 100 );
    REQUIRE( activateSpy->safeWait() );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    GIVEN( "Opened main window" )
    {
        auto toolBar = mainWindow->findChild<QToolBar*>();
        REQUIRE( toolBar != nullptr );

        auto filePathLabel = toolBar->findChild<PathLine*>();
        REQUIRE( filePathLabel != nullptr );

        auto tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
        REQUIRE( tabArea != nullptr );

        auto* saveLiveLogMenu
            = mainWindow->findChild<QMenu*>( QStringLiteral( "saveCurrentLiveLogMenu" ) );
        REQUIRE( saveLiveLogMenu != nullptr );
        REQUIRE_FALSE( saveLiveLogMenu->isEnabled() );
        REQUIRE(
            mainWindow->findChild<QAction*>( QStringLiteral( "saveCurrentLiveLogStripAnsiAction" ) )
            != nullptr );
        REQUIRE( mainWindow->findChild<QAction*>(
                     QStringLiteral( "saveCurrentLiveLogPreserveAnsiAction" ) )
                 != nullptr );

        const auto tempDirPath = makeTestDir( "mainwindow" );
        REQUIRE( QDir{ tempDirPath }.exists() );
        const auto testFilePath = QDir{ tempDirPath }.filePath( "klogg.conf" );
        {
            QFile testFile( testFilePath );
            REQUIRE( testFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
            testFile.write( "test\n" );
        }

        THEN( "Has no tabs" )
        {
            REQUIRE( tabArea->count() == 0 );
            AND_THEN( "Path label empty" )
            {
                REQUIRE( filePathLabel->text().isEmpty() );
            }
        }

        QAction* closeAction = mainWindow->findChild<QAction*>( QStringLiteral( "closeAction" ) );
        REQUIRE( closeAction != nullptr );

        // Find exitAction: it's the last action in the File menu (first menu in menu bar)
        auto* fileMenu = mainWindow->menuBar()->actions().constFirst()->menu();
        REQUIRE( fileMenu != nullptr );
        const auto fileActions = fileMenu->actions();
        REQUIRE_FALSE( fileActions.isEmpty() );
        auto* exitAction = fileActions.constLast();
        REQUIRE( exitAction != nullptr );

        WHEN( "Exit hotkey pressed" )
        {
            runInUiThread( [ exitAction ] {
                LOG_INFO << "ExitFromMainMenu";
                exitAction->trigger();
            } );

            THEN( "Exit signalled" )
            {
                REQUIRE( exitSpy->safeWait() );
            }
        }

        WHEN( "Load file" )
        {
            runInUiThread( [ &mainWindow, testFilePath ] {
                LOG_INFO << "Load file";
                mainWindow->loadInitialFile( testFilePath, false );
            } );

            THEN( "Path line has file name" )
            {
                REQUIRE( waitUiState(
                    [ & ] { return filePathLabel->text().contains( "klogg.conf" ); } ) );

                AND_THEN( "Has one tab" )
                {
                    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
                }
            }

            AND_WHEN( "Close tab hotkey pressed" )
            {
                // Wait for the background loading thread to finish before
                // closing the tab.  stopLoading() only sets an interrupt flag
                // -- it does not synchronously join the thread.  On Windows
                // runners the worker thread may still hold heap references or
                // unwind simdutf-internal state after isFirstLoadDone() returns
                // true, corrupting malloc and causing SIGSEGV on teardown.
                REQUIRE( waitUiState( [ & ] {
                    auto* crawler = qobject_cast<CrawlerWidget*>( tabArea->currentWidget() );
                    return crawler != nullptr && crawler->isFirstLoadDone();
                } ) );

                // Let the worker thread fully unwind before destroying the
                // tab.  Even after isFirstLoadDone() returns true, the
                // background thread may still be cleaning up — closing the
                // tab during that window causes use-after-free.
                QTest::qWait( 200 );

                runInUiThread( [ closeAction ] {
                    LOG_INFO << "Close tab";
                    closeAction->trigger();
                } );

                THEN( "Has no tabs" )
                {
                    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 0; } ) );

                    AND_THEN( "Path label empty" )
                    {
                        REQUIRE( waitUiState( [ & ] { return filePathLabel->text().isEmpty(); } ) );
                    }
                }
            }
        }
    }
}

SCENARIO( "Tab group chip shows the full group name", "[ui][tabgroup]" )
{
    TabGroupCleanupGuard tabGroupCleanupGuard;

    auto appSession = std::make_shared<Session>();
    const auto windowId = QString( "tab-group-chip-%1" )
                              .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->resize( 1600, 900 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    auto* tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );
    auto* tabBar = tabArea->findChild<QTabBar*>();
    REQUIRE( tabBar != nullptr );

    const auto tempDirPath = makeTestDir( "tabgroup_chip" );
    REQUIRE( QDir{ tempDirPath }.exists() );
    const auto firstFilePath = QDir{ tempDirPath }.filePath( "group_a.log" );
    const auto secondFilePath = QDir{ tempDirPath }.filePath( "group_b.log" );
    for ( const auto& filePath : { firstFilePath, secondFilePath } ) {
        QFile testFile( filePath );
        REQUIRE( testFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        testFile.write( "line\n" );
    }

    runInUiThread( [ &mainWindow, firstFilePath, secondFilePath ] {
        mainWindow->loadInitialFile( firstFilePath, false );
        mainWindow->loadInitialFile( secondFilePath, false );
    } );

    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 2; } ) );
    REQUIRE( waitUiState( [ & ] { return tabBar->count() == 2 && tabBar->isVisible(); } ) );

    QString groupId;
    runInUiThread( [ &groupId, firstFilePath ] {
        auto& groupManager = TabGroupManager::get();
        groupManager.createGroup( "C", QColor( "#D96C1A" ) );
        groupId = groupManager.groups().back().id;
        groupManager.addTabToGroup( groupId, firstFilePath );
        groupManager.save();
    } );

    // runInUiThread dispatches synchronously (PreciseTimer → qWait flushes
    // the event queue), but GroupManager::save() inside the dispatched lambda
    // may write via QSettings which syncs asynchronously on some platforms.
    // waitUiState polls up to 10 s, so the assertion either succeeds quickly
    // or fails with a clear timeout instead of a misleading empty-groupid
    // diagnostic.
    REQUIRE( waitUiState( [ & ] { return !groupId.isEmpty(); } ) );

    auto verifyGroupChipName = [ tabBar ]( const QString& expectedName ) -> int {
        REQUIRE( waitUiState( [ tabBar, &expectedName ] {
            auto* chip = findGroupChipButton( tabBar, 0 );
            return chip != nullptr && chip->text() == expectedName
                   && chip->width() >= chip->sizeHint().width() - 4;
        } ) );

        auto* chip = findGroupChipButton( tabBar, 0 );
        REQUIRE( chip != nullptr );
        const int sizeHintWidth = chip->sizeHint().width();
        REQUIRE( sizeHintWidth > chip->iconSize().width() + 8 );
        REQUIRE( chip->width() >= sizeHintWidth - 4 );
        return sizeHintWidth;
    };

    const int singleLetterWidth = verifyGroupChipName( "C" );

    const auto renameGroupAndVerify
        = [ &runInUiThread, &groupId, &verifyGroupChipName ]( const QString& groupName ) -> int {
        runInUiThread( [ &groupId, groupName ] {
            auto& groupManager = TabGroupManager::get();
            groupManager.renameGroup( groupId, groupName );
            groupManager.save();
        } );
        return verifyGroupChipName( groupName );
    };

    const int coreWidth = renameGroupAndVerify( "Core" );
    const int compileGroupWidth = renameGroupAndVerify( "Compile Group" );
    const int daemonLogsWidth = renameGroupAndVerify( "Compile Core Daemon Logs" );

    REQUIRE( coreWidth > singleLetterWidth );
    REQUIRE( compileGroupWidth > coreWidth );
    REQUIRE( daemonLogsWidth > compileGroupWidth );
}

// Helper: write raw bytes to a temp file and return its path
static QString writeTestFile( const QString& dirPath, const QString& name,
                              const QByteArray& content )
{
    const auto path = QDir{ dirPath }.filePath( name );
    QFile f( path );
    REQUIRE( f.open( QIODevice::WriteOnly ) );
    f.write( content );
    f.close();
    return path;
}

// Helper: read the merged temp file produced by openMerged
static QByteArray readMergedFile( Session& session, const std::vector<QString>& sources,
                                  const QString& tempDir )
{
    auto* view = session.openMerged( sources, []() { return new CrawlerWidget(); }, tempDir );
    REQUIRE( view != nullptr );

    const auto mergedPath = session.getFilename( view );
    QFile mergedFile( mergedPath );
    REQUIRE( mergedFile.open( QIODevice::ReadOnly ) );
    const auto result = mergedFile.readAll();

    session.close( view );
    return result;
}

SCENARIO( "Session::openMerged produces correct merged file", "[session][merge]" )
{
    auto appSession = std::make_shared<Session>();
    const auto tempDirPath = makeTestDir( "session_merge" );
    REQUIRE( QDir{ tempDirPath }.exists() );

    GIVEN( "Two files that both end with newlines" )
    {
        const auto file1 = writeTestFile( tempDirPath, "a.log", "line1\nline2\n" );
        const auto file2 = writeTestFile( tempDirPath, "b.log", "line3\nline4\n" );

        WHEN( "They are merged" )
        {
            const auto merged = readMergedFile( *appSession, { file1, file2 }, tempDirPath );

            THEN( "Content is concatenated without extra separator lines" )
            {
                REQUIRE( merged == QByteArray( "line1\nline2\nline3\nline4\n" ) );
            }
        }
    }

    GIVEN( "First file does not end with newline" )
    {
        const auto file1 = writeTestFile( tempDirPath, "no_nl.log", "line1\nline2" );
        const auto file2 = writeTestFile( tempDirPath, "with_nl.log", "line3\n" );

        WHEN( "They are merged" )
        {
            const auto merged = readMergedFile( *appSession, { file1, file2 }, tempDirPath );

            THEN( "A newline is inserted between files" )
            {
                REQUIRE( merged == QByteArray( "line1\nline2\nline3\n" ) );
            }
        }
    }

    GIVEN( "Last file does not end with newline" )
    {
        const auto file1 = writeTestFile( tempDirPath, "first.log", "line1\n" );
        const auto file2 = writeTestFile( tempDirPath, "last.log", "line2" );

        WHEN( "They are merged" )
        {
            const auto merged = readMergedFile( *appSession, { file1, file2 }, tempDirPath );

            THEN( "No trailing newline is appended to the last file" )
            {
                REQUIRE( merged == QByteArray( "line1\nline2" ) );
            }
        }
    }

    GIVEN( "Files containing non-UTF-8 bytes (Latin-1)" )
    {
        // Latin-1 bytes: 0xE9 = 'e with acute', 0xF1 = 'n with tilde'
        const QByteArray latin1Content = QByteArray( "caf\xe9\n", 5 );
        const QByteArray latin1Content2 = QByteArray( "ni\xf1o\n", 5 );
        const auto file1 = writeTestFile( tempDirPath, "latin1_a.log", latin1Content );
        const auto file2 = writeTestFile( tempDirPath, "latin1_b.log", latin1Content2 );

        WHEN( "They are merged" )
        {
            const auto merged = readMergedFile( *appSession, { file1, file2 }, tempDirPath );

            THEN( "Raw bytes are preserved exactly" )
            {
                REQUIRE( merged == latin1Content + latin1Content2 );
            }
        }
    }

    GIVEN( "Files containing binary-like content with null bytes" )
    {
        const QByteArray binaryContent = QByteArray( "abc\x00\x01\x02\n", 7 );
        const QByteArray textContent = QByteArray( "text\n", 5 );
        const auto file1 = writeTestFile( tempDirPath, "binary.log", binaryContent );
        const auto file2 = writeTestFile( tempDirPath, "text.log", textContent );

        WHEN( "They are merged" )
        {
            const auto merged = readMergedFile( *appSession, { file1, file2 }, tempDirPath );

            THEN( "Binary bytes including nulls are preserved" )
            {
                REQUIRE( merged == binaryContent + textContent );
            }
        }
    }

    GIVEN( "An empty file merged with a non-empty file" )
    {
        const auto file1 = writeTestFile( tempDirPath, "empty.log", QByteArray() );
        const auto file2 = writeTestFile( tempDirPath, "nonempty.log", "content\n" );

        WHEN( "They are merged" )
        {
            const auto merged = readMergedFile( *appSession, { file1, file2 }, tempDirPath );

            THEN( "Only the non-empty content appears, no extra newlines" )
            {
                REQUIRE( merged == QByteArray( "content\n" ) );
            }
        }
    }

    GIVEN( "Three files with mixed newline endings" )
    {
        const auto file1 = writeTestFile( tempDirPath, "f1.log", "a\n" );
        const auto file2 = writeTestFile( tempDirPath, "f2.log", "b" );
        const auto file3 = writeTestFile( tempDirPath, "f3.log", "c\n" );

        WHEN( "They are merged" )
        {
            const auto merged = readMergedFile( *appSession, { file1, file2, file3 }, tempDirPath );

            THEN( "Separator newline added only between f2 and f3" )
            {
                REQUIRE( merged == QByteArray( "a\nb\nc\n" ) );
            }
        }
    }

    GIVEN( "Merged file path is a valid file in tempDir" )
    {
        const auto file1 = writeTestFile( tempDirPath, "p1.log", "hello\n" );
        const auto file2 = writeTestFile( tempDirPath, "p2.log", "world\n" );

        WHEN( "openMerged is called" )
        {
            auto* view = appSession->openMerged(
                { file1, file2 }, []() { return new CrawlerWidget(); }, tempDirPath );
            REQUIRE( view != nullptr );

            THEN( "getFilename returns a real file path inside tempDir" )
            {
                const auto mergedPath = appSession->getFilename( view );
                REQUIRE( QFileInfo::exists( mergedPath ) );
                REQUIRE( mergedPath.startsWith( tempDirPath ) );
                REQUIRE( mergedPath.contains( "klogg_merged_" ) );
            }

            appSession->close( view );
        }
    }
}

SCENARIO( "MainWindow close keeps persisted open files for session restore", "[ui][session]" )
{
    auto appSession = std::make_shared<Session>();
    auto& sessionInfo = SessionInfo::getSynced();
    auto windowIds = sessionInfo.windows();
    const auto windowId = windowIds.isEmpty()
                              ? QString( "close-session-%1" )
                                    .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) )
                              : windowIds.front();

    if ( windowIds.isEmpty() ) {
        sessionInfo.add( windowId );
    }
    else {
        for ( auto i = windowIds.size() - 1; i > 0; --i ) {
            sessionInfo.remove( windowIds.at( i ) );
        }
    }
    sessionInfo.setOpenFiles( windowId, {} );
    sessionInfo.setCurrentFileIndex( windowId, -1 );
    sessionInfo.save();

    WindowSession windowSession{ appSession, windowId, 0 };

    auto& config = Configuration::get();
    const auto previousMinimizeToTray = config.minimizeToTray();
    config.setMinimizeToTray( false );
    config.save();

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    auto tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );

    const auto tempDirPath = makeTestDir( "restore_session" );
    REQUIRE( QDir{ tempDirPath }.exists() );
    const auto testFilePath = QDir{ tempDirPath }.filePath( "restore.log" );
    {
        QFile testFile( testFilePath );
        REQUIRE( testFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        testFile.write( "line\n" );
    }

    runInUiThread(
        [ &mainWindow, testFilePath ] { mainWindow->loadInitialFile( testFilePath, false ); } );
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
    REQUIRE( waitUiState(
        [ & ] { return SessionInfo::getSynced().openFiles( windowId ).size() == 1; } ) );

    runInUiThread( [ &mainWindow ] { mainWindow->close(); } );
    REQUIRE( waitUiState( [ & ] { return !mainWindow->isVisible(); } ) );

    const auto persistedOpenFiles = SessionInfo::getSynced().openFiles( windowId );
    REQUIRE( persistedOpenFiles.size() == 1 );
    REQUIRE( persistedOpenFiles.front().fileName == testFilePath );

    config.setMinimizeToTray( previousMinimizeToTray );
    config.save();
}

SCENARIO( "MainWindow close preserves restored ADB capture files", "[ui][session][adb]" )
{
    auto appSession = std::make_shared<Session>();
    auto& sessionInfo = SessionInfo::getSynced();
    auto windowIds = sessionInfo.windows();
    const auto windowId = windowIds.isEmpty()
                              ? QString( "close-adb-session-%1" )
                                    .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) )
                              : windowIds.front();

    if ( windowIds.isEmpty() ) {
        sessionInfo.add( windowId );
    }
    else {
        for ( auto i = windowIds.size() - 1; i > 0; --i ) {
            sessionInfo.remove( windowIds.at( i ) );
        }
    }

    const auto captureId
        = QString( "adb_capture_%1" ).arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    QString capturePath;
    {
        CaptureStore captureStore( captureId );
        captureStore.appendUtf8( QByteArray( "line\n" ) );
        captureStore.finishInput();
        capturePath = captureStore.capturePath();
    }
    REQUIRE( QDir{ capturePath }.exists() );

    const AdbLogcatSessionData adbSessionData{
        QStringLiteral( "adb" ),
        QStringLiteral( "serial-1" ),
        QStringLiteral( "Pixel Test" ),
        QString{},
        captureId,
        QString{},
    };
    // Persisted through the typed session spec, exactly what current klogg
    // writes. The default process backend persists as the transitional
    // legacy_process discriminator (without any raw command fields), which
    // must keep restoring during the Task 6 migration.
    const auto sourceSpec = klogg::livelog::serializeSpec(
        klogg::livelog::sessionSpecFromSessionData( adbSessionData ) );

    sessionInfo.setOpenFiles(
        windowId, { SessionInfo::OpenFile( adbSessionData.documentId(), 0, {}, "adb_logcat",
                                           adbSessionData.displayName(), sourceSpec ) } );
    sessionInfo.setCurrentFileIndex( windowId, 0 );
    sessionInfo.save();

    WindowSession windowSession{ appSession, windowId, 0 };

    auto& config = Configuration::get();
    const auto previousMinimizeToTray = config.minimizeToTray();
    config.setMinimizeToTray( false );
    config.save();

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    auto tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );

    runInUiThread( [ &mainWindow ] { mainWindow->reloadSession(); } );
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
    auto* disconnectAction
        = mainWindow->findChild<QAction*>( QStringLiteral( "disconnectSourceAction" ) );
    auto* reconnectAction
        = mainWindow->findChild<QAction*>( QStringLiteral( "reconnectSourceAction" ) );
    REQUIRE( disconnectAction != nullptr );
    REQUIRE( reconnectAction != nullptr );
    CHECK_FALSE( disconnectAction->isEnabled() );
    CHECK_FALSE( reconnectAction->isEnabled() );

    runInUiThread( [ &mainWindow ] { mainWindow->close(); } );
    REQUIRE( waitUiState( [ & ] { return !mainWindow->isVisible(); } ) );

    REQUIRE( QDir{ capturePath }.exists() );
    const auto persistedOpenFiles = SessionInfo::getSynced().openFiles( windowId );
    REQUIRE( persistedOpenFiles.size() == 1 );
    REQUIRE( persistedOpenFiles.front().sourceType == QStringLiteral( "adb_logcat" ) );

    config.setMinimizeToTray( previousMinimizeToTray );
    config.save();
}

SCENARIO( "Closing one of several windows deletes its discarded live capture",
          "[ui][session][adb][capture-cleanup]" )
{
    auto appSession = std::make_shared<Session>();
    auto& sessionInfo = SessionInfo::getSynced();
    SessionInfoRestoreGuard sessionInfoRestoreGuard{ sessionInfo };
    const auto existingWindowIds = sessionInfo.windows();
    const auto targetWindowId
        = QString( "discard-adb-session-%1" )
              .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    const auto survivorWindowId
        = QString( "survivor-session-%1" )
              .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    for ( const auto& windowId : existingWindowIds ) {
        sessionInfo.remove( windowId );
    }
    sessionInfo.add( targetWindowId );
    sessionInfo.add( survivorWindowId );

    const auto captureId = QString( "discard_capture_%1" )
                               .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    QString capturePath;
    {
        CaptureStore captureStore( captureId );
        captureStore.appendUtf8( QByteArrayLiteral( "line\n" ) );
        captureStore.finishInput();
        capturePath = captureStore.capturePath();
    }
    REQUIRE( QDir{ capturePath }.exists() );

    const AdbLogcatSessionData sessionData{
        QStringLiteral( "adb" ),
        QStringLiteral( "serial-discard" ),
        QStringLiteral( "Discarded Pixel" ),
        QString{},
        captureId,
        QString{},
    };
    const auto sourceSpec = klogg::livelog::serializeSpec(
        klogg::livelog::sessionSpecFromSessionData( sessionData ) );
    sessionInfo.setOpenFiles(
        targetWindowId,
        { SessionInfo::OpenFile( sessionData.documentId(), 0, {}, "adb_logcat",
                                 sessionData.displayName(), sourceSpec ) } );
    sessionInfo.setCurrentFileIndex( targetWindowId, 0 );
    sessionInfo.save();

    WindowSession windowSession{ appSession, targetWindowId, 0 };
    auto& config = Configuration::get();
    const auto previousMinimizeToTray = config.minimizeToTray();
    config.setMinimizeToTray( false );
    config.save();

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );
    QTest::qWait( 100 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto* tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );
    QTimer::singleShot( 0, Qt::PreciseTimer, mainWindow.get(),
                        [ &mainWindow ] { mainWindow->reloadSession(); } );
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );

    QTimer::singleShot( 0, Qt::PreciseTimer, mainWindow.get(),
                        [ &mainWindow ] { mainWindow->close(); } );
    REQUIRE( waitUiState( [ & ] { return !mainWindow->isVisible(); } ) );

    CHECK_FALSE( QDir{ capturePath }.exists() );
    const auto remainingWindows = SessionInfo::getSynced().windows();
    CHECK_FALSE( remainingWindows.contains( targetWindowId ) );
    CHECK( remainingWindows.contains( survivorWindowId ) );

    config.setMinimizeToTray( previousMinimizeToTray );
    config.save();
}

SCENARIO( "MainWindow restored iOS live log tabs show disconnected state", "[ui][session][ios]" )
{
    auto appSession = std::make_shared<Session>();
    auto& sessionInfo = SessionInfo::getSynced();
    SessionInfoRestoreGuard sessionInfoRestoreGuard{ sessionInfo };
    const auto windowIds = sessionInfo.windows();
    const auto windowId = QString( "restore-ios-session-%1" )
                              .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );

    sessionInfo.add( windowId );
    for ( const auto& existingWindowId : windowIds ) {
        sessionInfo.remove( existingWindowId );
    }

    const AdbLogcatSessionData iosSessionData{
        QStringLiteral( "pymobiledevice3" ),
        QStringLiteral( "00008030-001C195E36D8802E" ),
        QStringLiteral( "iPhone Test" ),
        QString{},
        QString( "ios_capture_%1" ).arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) ),
        QString{},
        LiveLogSourceType::IosLogStream,
    };
    // Persisted through the typed session spec (native backend; the recorded
    // pymobiledevice3 executable is deliberately not carried into the new
    // schema). Restores as a disconnected native tab.
    const auto sourceSpec = klogg::livelog::serializeSpec(
        klogg::livelog::sessionSpecFromSessionData( iosSessionData ) );

    sessionInfo.setOpenFiles(
        windowId, { SessionInfo::OpenFile( iosSessionData.documentId(), 0, {},
                                           iosSessionData.persistedSourceType(),
                                           iosSessionData.displayName(), sourceSpec ) } );
    sessionInfo.setCurrentFileIndex( windowId, 0 );
    sessionInfo.save();

    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    auto tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );

    runInUiThread( [ &mainWindow ] { mainWindow->reloadSession(); } );
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
    REQUIRE( tabArea->tabText( 0 ) == QStringLiteral( "iPhone Test" ) );

    mainWindow->close();
    sessionInfo.remove( windowId );
    sessionInfo.save();
}

SCENARIO( "Session restore clears unavailable ADB output bindings", "[ui][session][adb]" )
{
    auto appSession = std::make_shared<Session>();
    const auto tempDirPath = makeTestDir( "restore_adb_output" );
    REQUIRE( QDir{ tempDirPath }.exists() );

    const auto parentAsFile = QDir{ tempDirPath }.filePath( "not_a_directory" );
    {
        QFile parentFile( parentAsFile );
        REQUIRE( parentFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        parentFile.write( "x" );
    }

    const AdbLogcatSessionData adbSessionData{
        QStringLiteral( "adb" ),
        QStringLiteral( "serial-restore" ),
        QStringLiteral( "Restore Device" ),
        QString{},
        QString( "restore_capture_%1" ).arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) ),
        QDir{ parentAsFile }.filePath( "capture.log" ),
    };

    auto* view
        = appSession->openAdbLogcat( adbSessionData, []() { return new CrawlerWidget(); }, false );
    REQUIRE( view != nullptr );

    REQUIRE( appSession->getAssociatedPath( view ).isEmpty() );
    auto* adbSource = appSession->getAdbLogcatSource( view );
    REQUIRE( adbSource != nullptr );
    REQUIRE( adbSource->sessionData().boundOutputFile.isEmpty() );

    appSession->close( view );
}

// Regression gate for the folder-tab crash: opening/switching-to/closing a
// FolderCrawlerWidget tab inside MainWindow used to EXC_BAD_ACCESS because
// currentTabChanged did a static_cast<CrawlerWidget*> on the folder widget and
// dereferenced the garbage pointer. None of the existing itests opened a
// folder tab through MainWindow, so the bug was invisible. This scenario
// drives the real path (Session::openFolder + MainWindow::openFolderByPath +
// TabbedCrawlerWidget::addCrawler, which calls setCurrentIndex -> the crash
// site) and asserts it stays alive.
SCENARIO( "Folder tab in MainWindow does not crash on open/switch/close", "[ui][folder]" )
{
    TabGroupCleanupGuard tabGroupCleanupGuard;

    auto appSession = std::make_shared<Session>();
    const auto windowId
        = QString( "folder-tab-%1" ).arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->resize( 1600, 900 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    auto* tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );

    // closeAction triggers the private closeTab(ActionInitiator) slot via the
    // Qt signal/slot mechanism (the slot + ActionInitiator enum are private, so
    // they cannot be named directly from the test).
    QAction* closeAction = mainWindow->findChild<QAction*>( QStringLiteral( "closeAction" ) );
    REQUIRE( closeAction != nullptr );

    // Folder with a couple of readable files (enumerateFolderFiles skips empty
    // dirs by showing a modal message box and returning early).
    const auto tempDirPath = makeTestDir( "foldertab" );
    REQUIRE( QDir{ tempDirPath }.exists() );
    for ( const auto& name : { "a.log", "b.log" } ) {
        QFile f( QDir{ tempDirPath }.filePath( name ) );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        f.write( "hello world\n" );
    }
    // A standalone file to open as a regular CrawlerWidget tab alongside.
    const auto standaloneFile = QDir{ tempDirPath }.filePath( "standalone.log" );
    {
        QFile f( standaloneFile );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        f.write( "single file line\n" );
    }

    GIVEN( "An open MainWindow with no tabs" )
    {
        // Make closes non-interactive: closeAction triggers the User-initiator
        // path, which would otherwise pop a modal confirm dialog
        // (confirmTabClose defaults to true) and block the headless test.
        Configuration::get().setConfirmTabClose( false );

        REQUIRE( tabArea->count() == 0 );

        WHEN( "A folder tab is opened via openFolderByPath" )
        {
            // addCrawler -> setCurrentIndex -> currentTabChanged: the original
            // crash site (static_cast<CrawlerWidget*> on a FolderCrawlerWidget).
            runInUiThread(
                [ &mainWindow, tempDirPath ] { mainWindow->openFolderByPath( tempDirPath ); } );

            THEN( "The folder tab is added without crashing" )
            {
                REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
                // Settle so currentTabChanged fully applies its folder fallback
                // (signal routing is async on some platforms).
                QTest::qWait( 200 );
                REQUIRE( qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() )
                         != nullptr );
                // The tab label is the bare folder basename, matching single-file
                // tabs (no "[Folder]" prefix).
                REQUIRE( tabArea->tabText( 0 ) == QDir( tempDirPath ).dirName() );

                AND_WHEN( "A file tab is also opened" )
                {
                    runInUiThread( [ &mainWindow, standaloneFile ] {
                        mainWindow->loadInitialFile( standaloneFile, false );
                    } );

                    THEN( "Both tabs coexist and file tab is current" )
                    {
                        REQUIRE( waitUiState( [ & ] { return tabArea->count() == 2; } ) );
                        QTest::qWait( 200 );
                        REQUIRE( qobject_cast<CrawlerWidget*>( tabArea->currentWidget() )
                                 != nullptr );

                        AND_WHEN( "Switching back to the folder tab (index 0)" )
                        {
                            runInUiThread( [ tabArea ] { tabArea->setCurrentIndex( 0 ); } );

                            THEN( "No crash and folder tab is current" )
                            {
                                // Settle deterministically: setCurrentIndex fires
                                // currentChanged asynchronously (MainWindow wires
                                // optionsChanged/applyConfiguration on it), and a
                                // fixed delay flaked on the slower ubuntu-20.04 CI.
                                REQUIRE( waitUiState( [ & ] {
                                    return qobject_cast<FolderCrawlerWidget*>(
                                               tabArea->currentWidget() )
                                           != nullptr;
                                } ) );

                                AND_WHEN( "Switching back to the file tab (index 1)" )
                                {
                                    runInUiThread( [ tabArea ] { tabArea->setCurrentIndex( 1 ); } );

                                    THEN( "No crash and file tab is current" )
                                    {
                                        REQUIRE( waitUiState( [ & ] {
                                            return qobject_cast<CrawlerWidget*>(
                                                       tabArea->currentWidget() )
                                                   != nullptr;
                                        } ) );

                                        AND_THEN( "Closing the folder tab does not crash" )
                                        {
                                            // Switch to the folder tab and close
                                            // the current tab via closeAction
                                            // (closeTab is a private slot).
                                            runInUiThread( [ closeAction, tabArea ] {
                                                tabArea->setCurrentIndex( 0 );
                                                closeAction->trigger();
                                            } );

                                            REQUIRE( waitUiState(
                                                [ & ] { return tabArea->count() == 1; } ) );
                                            QTest::qWait( 200 );
                                            // Remaining tab is the file tab.
                                            REQUIRE( qobject_cast<CrawlerWidget*>(
                                                         tabArea->currentWidget() )
                                                     != nullptr );
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        WHEN( "A folder tab is opened as the only tab and then closed" )
        {
            runInUiThread(
                [ &mainWindow, tempDirPath ] { mainWindow->openFolderByPath( tempDirPath ); } );
            REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
            QTest::qWait( 200 );
            REQUIRE( qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() ) != nullptr );

            // closeTab on the folder current tab exercises the folder close
            // branch (BEFORE the CrawlerWidget assert) and then the no-tab-left
            // else branch in currentTabChanged. Triggered via closeAction
            // (closeTab is a private slot).
            runInUiThread( [ closeAction ] { closeAction->trigger(); } );

            THEN( "No assert-abort and no tabs remain" )
            {
                REQUIRE( waitUiState( [ & ] { return tabArea->count() == 0; } ) );
                QTest::qWait( 200 );
            }
        }
    }
}

// F5: MainWindow dispatches focus-search / wrap / go-to-line / QuickFind
// lifecycle through AbstractCrawlerWidget so they work on folder tabs too.
SCENARIO( "Folder tab receives the polymorphic MainWindow dispatch", "[ui][folder]" )
{
    TabGroupCleanupGuard tabGroupCleanupGuard;
    // Deterministic followAction enabled state (must precede MainWindow
    // construction: the action reads anyFileWatchEnabled() at creation).
    FileWatchConfigGuard fileWatchConfigGuard;

    auto appSession = std::make_shared<Session>();
    const auto windowId = QString( "folder-dispatch-%1" )
                              .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->resize( 1600, 900 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    auto* tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );

    const auto tempDirPath = makeTestDir( "folderdispatch" );
    REQUIRE( QDir{ tempDirPath }.exists() );
    {
        QFile f( QDir{ tempDirPath }.filePath( "a.log" ) );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        f.write( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" );
    }

    runInUiThread( [ &mainWindow, tempDirPath ] { mainWindow->openFolderByPath( tempDirPath ); } );
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
    QTest::qWait( 200 );
    auto* folderWidget = qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() );
    REQUIRE( folderWidget != nullptr );

    WHEN( "the wrap action is toggled" )
    {
        auto* wrapAction = mainWindow->findChild<QAction*>( QStringLiteral( "textWrapAction" ) );
        REQUIRE( wrapAction != nullptr );

        // State-agnostic: the default wrap state comes from the machine's
        // saved Configuration, so drive both directions.
        runInUiThread( [ wrapAction ] { wrapAction->setChecked( true ); } );

        THEN( "the folder views wrap and unwrap" )
        {
            REQUIRE( waitUiState( [ & ] { return folderWidget->isTextWrapEnabled(); } ) );
            REQUIRE( folderWidget->mainView()->isTextWrapEnabled() );

            runInUiThread( [ wrapAction ] { wrapAction->setChecked( false ); } );
            REQUIRE( waitUiState( [ & ] { return !folderWidget->isTextWrapEnabled(); } ) );
            REQUIRE_FALSE( folderWidget->mainView()->isTextWrapEnabled() );
        }
    }

    WHEN( "the focus-search shortcut is pressed" )
    {
        THEN( "the folder search input gains focus" )
        {
            pressConfiguredShortcut( mainWindow.get(), ShortcutAction::MainWindowFocusSearchInput );
            REQUIRE( waitUiState( [ & ] {
                return folderWidget->searchToolbar()->searchLineEdit()->lineEdit()->hasFocus();
            } ) );
        }
    }

    WHEN( "menu state is inspected on the folder tab" )
    {
        auto* goToLine = mainWindow->findChild<QAction*>( QStringLiteral( "goToLineAction" ) );
        auto* follow = mainWindow->findChild<QAction*>( QStringLiteral( "followAction" ) );
        auto* wrap = mainWindow->findChild<QAction*>( QStringLiteral( "textWrapAction" ) );

        THEN( "document actions are enabled" )
        {
            // The folder branch of currentTabChanged explicitly re-enables the
            // wrap toggle and syncs its checked state from the folder views.
            REQUIRE( wrap != nullptr );
            REQUIRE( wrap->isEnabled() );
            REQUIRE( wrap->isChecked() == folderWidget->isTextWrapEnabled() );
            // Go-to-line is routed polymorphically and stays available.
            REQUIRE( goToLine != nullptr );
            REQUIRE( goToLine->isEnabled() );
            // Follow applies to the file shown in the folder main view and
            // stays available whenever file watching is enabled.
            REQUIRE( follow != nullptr );
            REQUIRE( follow->isEnabled() );
        }

        AND_WHEN( "follow is toggled on for the folder tab" )
        {
            runInUiThread( [ follow ] { follow->trigger(); } );

            THEN( "the folder main view follows and the action shows it" )
            {
                // RED: today followSet is routed through the signal mux, whose
                // current document is null for folder tabs, so the folder main
                // view never enters follow mode.
                REQUIRE(
                    waitUiState( [ & ] { return folderWidget->mainView()->isFollowEnabled(); } ) );
                REQUIRE( follow->isChecked() );
            }

            AND_WHEN( "switching to a file tab and back to the folder tab" )
            {
                const auto filePath = QDir( tempDirPath ).absoluteFilePath( "a.log" );
                runInUiThread(
                    [ &mainWindow, filePath ] { mainWindow->loadInitialFile( filePath, false ); } );
                REQUIRE( waitUiState( [ & ] { return tabArea->count() == 2; } ) );
                // Let the file tab's background load finish (and its worker
                // thread unwind) before switching away / tearing down.
                REQUIRE( waitUiState( [ & ] {
                    auto* crawler = qobject_cast<CrawlerWidget*>( tabArea->widget( 1 ) );
                    return crawler != nullptr && crawler->isFirstLoadDone();
                } ) );
                QTest::qWait( 200 );

                runInUiThread( [ tabArea ] { tabArea->setCurrentIndex( 0 ); } );
                REQUIRE( waitUiState( [ & ] {
                    return qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() )
                           != nullptr;
                } ) );
                QTest::qWait( 200 );

                THEN( "the follow action still reflects the folder main view" )
                {
                    // RED: today switching to a folder tab forces the action
                    // unchecked (disableFileSpecificActions) instead of syncing
                    // the checked state from the folder main view's follow mode.
                    REQUIRE( waitUiState( [ & ] { return follow->isChecked(); } ) );
                    REQUIRE( folderWidget->mainView()->isFollowEnabled() );
                }
            }
        }
    }

    WHEN( "a result file is open and Copy Path is triggered" )
    {
        // The info line shows the file in the folder main view; Copy Path must
        // agree with it (not blindly copy the folder path).
        auto* copyPath
            = mainWindow->findChild<QAction*>( QStringLiteral( "copyPathToClipboardAction" ) );
        REQUIRE( copyPath != nullptr );

        runInUiThread( [ folderWidget ] { folderWidget->searchFor( "ERROR" ); } );
        REQUIRE( waitUiState( [ & ] { return !folderWidget->isSearchActive(); } ) );

        const auto expectedPath = QDir( tempDirPath ).absoluteFilePath( "a.log" );
        runInUiThread( [ folderWidget ] { folderWidget->selectResultRow( 1_lnum ); } );
        REQUIRE(
            waitUiState( [ & ] { return folderWidget->currentMainFilePath() == expectedPath; } ) );

        THEN( "the clipboard holds the main-view file path" )
        {
            runInUiThread( [ copyPath ] { copyPath->trigger(); } );
            REQUIRE( waitUiState( [ & ] {
                return QApplication::clipboard()->text()
                       == QDir::toNativeSeparators( expectedPath );
            } ) );
        }
    }
}

// F9: "Go to top" and "Follow file" are document-level actions: on a folder
// tab they must apply to the file shown in the folder MAIN view. Today both
// are routed through the signal mux, whose current document is null for
// folder tabs (signalMux_.setCurrentDocument( nullptr ) in currentTabChanged),
// so they are silent no-ops there.
SCENARIO( "Folder tab go-to-top and follow actions apply to the main view file", "[ui][folder]" )
{
    TabGroupCleanupGuard tabGroupCleanupGuard;
    // Deterministic followAction enabled state (must precede MainWindow
    // construction: the action reads anyFileWatchEnabled() at creation).
    FileWatchConfigGuard fileWatchConfigGuard;

    auto appSession = std::make_shared<Session>();
    const auto windowId = QString( "folder-doc-actions-%1" )
                              .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    // The singleShot above is queued on the UI event loop; if it has not run
    // by now (starved loop on a loaded CI runner) every subsequent deref is
    // UB, so fail loudly instead of crashing through resize().
    REQUIRE( mainWindow != nullptr );
    mainWindow->resize( 1600, 900 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    auto* tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );

    auto* goToTopAction = mainWindow->findChild<QAction*>( QStringLiteral( "goToTopAction" ) );
    REQUIRE( goToTopAction != nullptr );
    auto* followAction = mainWindow->findChild<QAction*>( QStringLiteral( "followAction" ) );
    REQUIRE( followAction != nullptr );

    // 200 matching lines so the folder main view is comfortably scrollable
    // once the file is opened from a result row.
    const auto tempDirPath = makeTestDir( "folderdocactions" );
    REQUIRE( QDir{ tempDirPath }.exists() );
    const auto logFilePath = QDir( tempDirPath ).absoluteFilePath( "a.log" );
    {
        QFile f( logFilePath );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        QByteArray payload;
        for ( int i = 0; i < 200; ++i ) {
            payload.append( "ERROR line " + QByteArray::number( i ) + "\n" );
        }
        f.write( payload );
    }

    GIVEN( "a folder tab with a file opened in the main view" )
    {
        runInUiThread(
            [ &mainWindow, tempDirPath ] { mainWindow->openFolderByPath( tempDirPath ); } );
        REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
        QTest::qWait( 200 );
        auto* folderWidget = qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() );
        REQUIRE( folderWidget != nullptr );

        runInUiThread( [ folderWidget ] { folderWidget->searchFor( QStringLiteral( "ERROR" ) ); } );
        REQUIRE( waitUiState( [ & ] { return !folderWidget->isSearchActive(); } ) );
        REQUIRE( folderWidget->filteredView() != nullptr );

        // Results rows: group header at 0, then one data row per match. Row 2
        // is a data row; selecting it in the results view opens its file in
        // the main view (newSelection -> onResultSelected, the same path
        // FolderCrawlerWidget::selectResultRow drives).
        runInUiThread(
            [ folderWidget ] { folderWidget->filteredView()->selectAndDisplayLine( 2_lnum ); } );
        REQUIRE(
            waitUiState( [ & ] { return folderWidget->currentMainFilePath() == logFilePath; } ) );
        // The on-demand index + layout of the main-view file are async: wait
        // until the 200-line file is actually scrollable, then settle so the
        // worker thread unwinds before the views are driven further.
        REQUIRE( waitUiState(
            [ & ] { return folderWidget->mainView()->verticalScrollBar()->maximum() > 0; } ) );
        QTest::qWait( 200 );

        auto scrollMainViewToBottom = [ & ] {
            runInUiThread( [ folderWidget ] {
                auto* scrollBar = folderWidget->mainView()->verticalScrollBar();
                scrollBar->setValue( scrollBar->maximum() );
            } );
            REQUIRE( waitUiState(
                [ & ] { return folderWidget->mainView()->verticalScrollBar()->value() > 0; } ) );
        };

        WHEN( "the go-to-top action is triggered" )
        {
            scrollMainViewToBottom();

            runInUiThread( [ goToTopAction ] { goToTopAction->trigger(); } );

            THEN( "the folder main view scrolls back to the top" )
            {
                // RED: jumpToTop is mux-routed; the mux document is null on
                // folder tabs, so the main view never scrolls.
                REQUIRE( waitUiState( [ & ] {
                    return folderWidget->mainView()->verticalScrollBar()->value() == 0;
                } ) );
            }
        }

        WHEN( "the go-to-top shortcut is pressed" )
        {
            scrollMainViewToBottom();

            pressConfiguredShortcut( mainWindow.get(), ShortcutAction::MainWindowGoToTop );

            THEN( "the folder main view scrolls back to the top" )
            {
                // RED: the shortcut fires the same mux-routed goToTopAction.
                REQUIRE( waitUiState( [ & ] {
                    return folderWidget->mainView()->verticalScrollBar()->value() == 0;
                } ) );
            }
        }

        WHEN( "the follow action is toggled on" )
        {
            REQUIRE( waitUiState( [ & ] { return followAction->isEnabled(); } ) );

            runInUiThread( [ followAction ] { followAction->trigger(); } );

            THEN( "the folder main view enters follow mode" )
            {
                // RED: followSet is mux-routed; the mux document is null on
                // folder tabs, so the main view's follow mode is never set.
                REQUIRE(
                    waitUiState( [ & ] { return folderWidget->mainView()->isFollowEnabled(); } ) );
                REQUIRE( followAction->isChecked() );
            }
        }

        WHEN( "the results view has focus and go-to-top is triggered" )
        {
            scrollMainViewToBottom();

            runInUiThread( [ folderWidget ] {
                folderWidget->filteredView()->setFocus( Qt::OtherFocusReason );
            } );
            REQUIRE( waitUiState( [ & ] { return folderWidget->filteredView()->hasFocus(); } ) );

            const auto selectedRowsBefore = folderWidget->filteredView()->getSelectedLinesText();
            REQUIRE_FALSE( selectedRowsBefore.isEmpty() );

            runInUiThread( [ goToTopAction ] { goToTopAction->trigger(); } );

            THEN( "the main view scrolls to top and the results selection is untouched" )
            {
                // RED: same null-mux-document no-op regardless of which folder
                // view holds focus.
                REQUIRE( waitUiState( [ & ] {
                    return folderWidget->mainView()->verticalScrollBar()->value() == 0;
                } ) );
                // The action targets the folder MAIN view only: the results
                // (filtered) view's selection must not be moved by it.
                REQUIRE( folderWidget->filteredView()->getSelectedLinesText()
                         == selectedRowsBefore );
            }
        }
    }
}

// Field repro: on a folder tab, clicking a search result whose file lives at a
// very long (deeply nested, non-ASCII) path left the toolbar PathLine blank.
// Pins two observable contracts of updateInfoLine's folder branch:
//   1. the label's text is the main-view file's full path (data layer), and
//   2. the label stays visible in the toolbar (a QLabel without wordWrap has
//      minimumSizeHint == sizeHint == full text width; a path wider than the
//      toolbar can overflow the widget into the toolbar's extension popup,
//      which renders as a blank path bar).
SCENARIO( "Folder tab info line shows the main-view file path for a long nested path",
          "[ui][folder]" )
{
    TabGroupCleanupGuard tabGroupCleanupGuard;
    FileWatchConfigGuard fileWatchConfigGuard;

    auto appSession = std::make_shared<Session>();
    const auto windowId = QString( "folder-infoline-%1" )
                              .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    REQUIRE( mainWindow != nullptr );
    mainWindow->resize( 1600, 900 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    auto* tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );

    auto* toolBar = mainWindow->findChild<QToolBar*>();
    REQUIRE( toolBar != nullptr );
    auto* filePathLabel = toolBar->findChild<PathLine*>();
    REQUIRE( filePathLabel != nullptr );

    // Field-report shape: an ordinary folder root with the matched file buried
    // in deep non-ASCII directories, so the full path (both byte count and
    // rendered width) far exceeds the toolbar's available label width.
    const auto tempDirPath = makeTestDir( "folderinfoline" );
    REQUIRE( QDir{ tempDirPath }.exists() );
    const auto deepDir
        = QDir( tempDirPath )
              .filePath( QStringLiteral(
                  "测试机进入相册点击快捷翻胶囊测试机已显示有图片但设备空间首页显示未连接且拔插APP"
                  "显示未连接"
                  "/2026-08-15_11-38-20@interconnection/common/ap_log/2026-08-15_11-36-51" ) );
    REQUIRE( QDir{}.mkpath( deepDir ) );
    // Locale guard: under a non-UTF-8 locale (e.g. the CI TSan container's
    // default POSIX), some Qt builds transcode file names via the locale
    // codec and silently DROP the non-ASCII bytes -- mkpath then creates a
    // directory literally named "APP", and every later path comparison fails
    // through a 10s waitUiState timeout (PR #62 TSan leg). Fail here with a
    // message that names the cause instead.
    REQUIRE( QDir( deepDir ).exists() );
    const auto logFilePath = QDir( deepDir ).filePath( "android_log_20260815113820.log" );
    {
        QFile f( logFilePath );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        QByteArray payload;
        for ( int i = 0; i < 20; ++i ) {
            payload.append( "ERROR line " + QByteArray::number( i ) + "\n" );
        }
        f.write( payload );
    }

    GIVEN( "a folder tab where a result click opens the long-path file" )
    {
        runInUiThread(
            [ &mainWindow, tempDirPath ] { mainWindow->openFolderByPath( tempDirPath ); } );
        REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
        QTest::qWait( 200 );
        auto* folderWidget = qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() );
        REQUIRE( folderWidget != nullptr );

        runInUiThread( [ folderWidget ] { folderWidget->searchFor( QStringLiteral( "ERROR" ) ); } );
        REQUIRE( waitUiState( [ & ] { return !folderWidget->isSearchActive(); } ) );
        REQUIRE( folderWidget->filteredView() != nullptr );

        // Row 0 is the group header, row 2 a data row: selecting it opens the
        // file in the main view (newSelection -> onResultSelected).
        runInUiThread(
            [ folderWidget ] { folderWidget->filteredView()->selectAndDisplayLine( 2_lnum ); } );
        REQUIRE(
            waitUiState( [ & ] { return folderWidget->currentMainFilePath() == logFilePath; } ) );
        // The main-view index completes async; mainViewFileChanged (fired at
        // its completion) is what re-runs updateInfoLine, so settle the event
        // loop before asserting the label.
        QTest::qWait( 200 );

        THEN( "the toolbar path line shows the file path" )
        {
            INFO( "currentMainFilePath: " << folderWidget->currentMainFilePath().toStdString() );
            INFO( "label text: '" << filePathLabel->text().toStdString() << "'" );
            REQUIRE( filePathLabel->text() == QDir::toNativeSeparators( logFilePath ) );
        }

        THEN( "the toolbar path line stays visible" )
        {
            INFO( "label visible: " << filePathLabel->isVisible() );
            REQUIRE( filePathLabel->isVisible() );
        }
    }
}

// Codex P1 (background-follow leak): the direct connection
// FolderCrawlerWidget::followModeChanged -> MainWindow::changeFollowMode
// (mainwindow.cpp:2328) has no currency guard. A HIDDEN folder tab can still
// emit followModeChanged: its main view's selectAndDisplayLine unconditionally
// calls disableFollow() -> followModeChanged(false) (abstractlogview.cpp:1970
// -> 3563, relayed by foldercrawlerwidget.cpp:478). changeFollowMode unchecks
// the shared followAction, and followAction::toggled dispatches followSet to
// the CURRENT document (mainwindow.cpp:802) -- so a background folder event
// silently kills the follow mode of a following file tab. This test pins the
// expected behavior: only the current tab's follow transitions may touch the
// shared action / other tabs' state.
TEST_CASE( "Background folder tab must not change the current tab's follow state", "[ui][folder]" )
{
    TabGroupCleanupGuard tabGroupCleanupGuard;
    // Deterministic followAction enabled state (must precede MainWindow
    // construction: the action reads anyFileWatchEnabled() at creation).
    FileWatchConfigGuard fileWatchConfigGuard;

    auto appSession = std::make_shared<Session>();
    const auto windowId = QString( "folder-bg-follow-%1" )
                              .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    REQUIRE( mainWindow != nullptr );
    mainWindow->resize( 1600, 900 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    auto* tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );
    auto* followAction = mainWindow->findChild<QAction*>( QStringLiteral( "followAction" ) );
    REQUIRE( followAction != nullptr );

    // 200 all-ERROR lines so every line matches the folder search below and
    // the folder main view is comfortably scrollable once a.log is opened
    // from a result row.
    const auto tempDirPath = makeTestDir( "folderbgfollow" );
    REQUIRE( QDir{ tempDirPath }.exists() );
    const auto logFilePath = QDir( tempDirPath ).absoluteFilePath( "a.log" );
    {
        QFile f( logFilePath );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        QByteArray payload;
        for ( int i = 0; i < 200; ++i ) {
            payload.append( "ERROR line " + QByteArray::number( i ) + "\n" );
        }
        f.write( payload );
    }
    // A standalone file to open as the second (file) tab.
    const auto standaloneFile = QDir( tempDirPath ).absoluteFilePath( "standalone.log" );
    {
        QFile f( standaloneFile );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        QByteArray payload;
        for ( int i = 0; i < 200; ++i ) {
            payload.append( "standalone line " + QByteArray::number( i ) + "\n" );
        }
        f.write( payload );
    }

    GIVEN( "a folder tab with follow armed, hidden behind a following file tab" )
    {
        runInUiThread(
            [ &mainWindow, tempDirPath ] { mainWindow->openFolderByPath( tempDirPath ); } );
        REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
        QTest::qWait( 200 );
        auto* folderWidget = qobject_cast<FolderCrawlerWidget*>( tabArea->widget( 0 ) );
        REQUIRE( folderWidget != nullptr );

        // Open a.log in the folder main view from a result row. Results rows:
        // group header at 0, then one data row per match -- row 1 is the
        // first data row.
        runInUiThread( [ folderWidget ] { folderWidget->searchFor( QStringLiteral( "ERROR" ) ); } );
        REQUIRE( waitUiState( [ & ] { return !folderWidget->isSearchActive(); } ) );
        runInUiThread( [ folderWidget ] { folderWidget->selectResultRow( 1_lnum ); } );
        REQUIRE(
            waitUiState( [ & ] { return folderWidget->currentMainFilePath() == logFilePath; } ) );
        // The on-demand index + layout of the main-view file are async; wait
        // until it is scrollable, then settle so the worker thread unwinds.
        REQUIRE( waitUiState(
            [ & ] { return folderWidget->mainView()->verticalScrollBar()->maximum() > 0; } ) );
        QTest::qWait( 200 );

        // Arm the folder main view's follow FIRST so the later emission is a
        // genuine "follow turned off" transition from the folder tab, and so
        // the folder->MainWindow uplink (connected when the folder tab was
        // current) has carried a real state before going background.
        runInUiThread( [ folderWidget ] { folderWidget->followSet( true ); } );
        REQUIRE( waitUiState( [ & ] { return folderWidget->mainView()->isFollowEnabled(); } ) );

        // Open the standalone file as the SECOND tab; it becomes current.
        runInUiThread( [ &mainWindow, standaloneFile ] {
            mainWindow->loadInitialFile( standaloneFile, false );
        } );
        REQUIRE( waitUiState( [ & ] { return tabArea->count() == 2; } ) );
        auto* crawler = qobject_cast<CrawlerWidget*>( tabArea->widget( 1 ) );
        REQUIRE( crawler != nullptr );
        // Let the file tab's background load finish (and its worker thread
        // unwind) before driving follow / switching context.
        REQUIRE( waitUiState( [ & ] { return crawler->isFirstLoadDone(); } ) );
        QTest::qWait( 200 );

        // Enable follow on the now-current file tab via the shared action.
        runInUiThread( [ followAction ] { followAction->trigger(); } );
        REQUIRE( waitUiState( [ & ] { return crawler->isFollowEnabled(); } ) );
        REQUIRE( followAction->isChecked() );

        WHEN( "the hidden folder tab emits followModeChanged(false)" )
        {
            // selectAndDisplayLine -> disableFollow -> followModeChanged(false)
            // on the folder main view, relayed to MainWindow over the
            // unguarded direct connection -- all while the file tab is
            // current.
            runInUiThread(
                [ folderWidget ] { folderWidget->mainView()->selectAndDisplayLine( 0_lnum ); } );
            // Pump events so every queued leg of the emission has landed
            // before the state is sampled.
            QTest::qWait( 200 );

            THEN( "the current file tab's follow state is untouched" )
            {
                // Without the currency guard in onFolderFollowModeChanged the
                // hidden folder tab's emission would uncheck the shared
                // followAction and toggled would dispatch followSet(false) to
                // the CURRENT document -- both of these assertions flipped
                // false before the guard existed.
                REQUIRE( waitUiState( [ & ] { return crawler->isFollowEnabled(); } ) );
                REQUIRE( followAction->isChecked() );
            }
        }
    }
}

// F8: the QuickFindMux snapshots the folder's active pane view at tab
// activation; panes created/switched afterwards must be re-registered or
// QuickFind drives a stale (or freed) view.
SCENARIO( "Folder QuickFind re-registers on pane changes", "[ui][folder]" )
{
    TabGroupCleanupGuard tabGroupCleanupGuard;

    auto appSession = std::make_shared<Session>();
    const auto windowId
        = QString( "folder-qf-%1" ).arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->resize( 1600, 900 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    auto* tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );

    const auto tempDirPath = makeTestDir( "folderqf" );
    REQUIRE( QDir{ tempDirPath }.exists() );
    {
        QFile f( QDir{ tempDirPath }.filePath( "a.log" ) );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        f.write( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" );
    }

    runInUiThread( [ &mainWindow, tempDirPath ] { mainWindow->openFolderByPath( tempDirPath ); } );
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
    QTest::qWait( 200 );
    auto* folderWidget = qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() );
    REQUIRE( folderWidget != nullptr );

    // Search, snapshot pane 0 (keep-results) and search again in a fresh pane.
    runInUiThread( [ folderWidget ] { folderWidget->searchFor( "ERROR" ); } );
    REQUIRE( waitUiState( [ & ] { return !folderWidget->isSearchActive(); } ) );
    runInUiThread( [ folderWidget ] {
        folderWidget->searchToolbar()->setKeepResultsChecked( true );
        folderWidget->searchFor( "beta" );
    } );
    REQUIRE( waitUiState( [ & ] { return !folderWidget->isSearchActive(); } ) );
    REQUIRE( waitUiState( [ & ] { return folderWidget->paneCount() == 2; } ) );

    WHEN( "the new active pane's view initiates a QuickFind change" )
    {
        THEN( "the session QuickFind pattern follows" )
        {
            // The pane-1 view was never registered at tab activation (only the
            // original pane-0 view was); its changeQuickFind signal must reach
            // the mux after the pane lifecycle re-registration.
            Q_EMIT folderWidget->filteredView()->changeQuickFind( QStringLiteral( "beta" ),
                                                                  QuickFindMux::Forward );
            REQUIRE( waitUiState( [ & ] {
                return appSession->quickFindPattern()->getPattern() == QStringLiteral( "beta" );
            } ) );
        }
    }

    WHEN( "the original (mux-registered) pane is closed" )
    {
        // Closing pane 0 destroys the view the mux registered at activation.
        // The re-registration disconnects the dead view (QPointer guard in
        // QuickFindMux::unregisterAllSearchables) and registers the surviving
        // pane. NOTE: the guard is EXERCISED here (a probe shows the nulled
        // entry at re-registration) but not VERIFIED -- with the guard broken
        // the disconnect on the freed view is benign-in-practice in this run,
        // so only an ASAN build would catch it. The functional assertions
        // below verify QuickFind follows the surviving pane.
        runInUiThread(
            [ folderWidget ] { Q_EMIT folderWidget->resultsTabs()->tabCloseRequested( 0 ); } );
        REQUIRE( waitUiState( [ & ] { return folderWidget->paneCount() == 1; } ) );

        THEN( "QuickFind still follows the surviving pane" )
        {
            Q_EMIT folderWidget->filteredView()->changeQuickFind( QStringLiteral( "gamma" ),
                                                                  QuickFindMux::Forward );
            REQUIRE( waitUiState( [ & ] {
                return appSession->quickFindPattern()->getPattern() == QStringLiteral( "gamma" );
            } ) );
        }
    }

    WHEN( "a background folder tab's panes change" )
    {
        // A second folder tab (now current): the first folder goes to the
        // background. Its pane changes must NOT steal the mux from the
        // foreground document (MainWindow::onFolderSearchablesChanged guards
        // on the sender being the current document).
        const auto tempDirPath2 = makeTestDir( "folderqf2" );
        REQUIRE( QDir{ tempDirPath2 }.exists() );
        {
            QFile f( QDir{ tempDirPath2 }.filePath( "c.log" ) );
            REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
            f.write( "line0\nERROR delta\n" );
        }
        runInUiThread(
            [ &mainWindow, tempDirPath2 ] { mainWindow->openFolderByPath( tempDirPath2 ); } );
        REQUIRE( waitUiState( [ & ] { return tabArea->count() == 2; } ) );
        QTest::qWait( 200 );
        auto* foreground = qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() );
        REQUIRE( foreground != nullptr );
        REQUIRE( foreground != folderWidget );

        // Drive the BACKGROUND folder's pane lifecycle: pane 1 is active from
        // the keep-results setup, so switching to 0 emits searchablesChanged.
        runInUiThread( [ folderWidget ] { folderWidget->resultsTabs()->setCurrentIndex( 0 ); } );
        QTest::qWait( 200 );

        THEN( "the mux stays with the foreground document" )
        {
            Q_EMIT foreground->filteredView()->changeQuickFind( QStringLiteral( "delta" ),
                                                                QuickFindMux::Forward );
            REQUIRE( waitUiState( [ & ] {
                return appSession->quickFindPattern()->getPattern() == QStringLiteral( "delta" );
            } ) );
        }
    }
}

SCENARIO( "Tab switches coalesce session persistence into a debounced write", "[ui][session]" )
{
    // Regression test for the tab-switch stall: currentTabChanged used to end
    // with an unconditional persistSessionState() -- a full session rewrite +
    // QSettings sync (CFPreferencesSynchronize XPC on macOS) per switch, plus
    // a getSynced() re-read. Under preferences-daemon contention each sync can
    // spike to hundreds of ms, which is the "occasional long stall on tab
    // switch". Persistence is now debounced: frequent triggers coalesce into a
    // single write; closeEvent still flushes synchronously.
    auto appSession = std::make_shared<Session>();
    const auto windowId
        = QString( "tab-debounce-%1" ).arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    auto* tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );

    const auto tempDirPath = makeTestDir( "tabdebounce" );
    REQUIRE( QDir{ tempDirPath }.exists() );
    for ( const auto& name : { "a.log", "b.log" } ) {
        QFile f( QDir{ tempDirPath }.filePath( name ) );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        f.write( "debounce line\n" );
    }

    runInUiThread( [ &mainWindow, tempDirPath ] {
        mainWindow->loadInitialFile( QDir{ tempDirPath }.filePath( "a.log" ), false );
    } );
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
    runInUiThread( [ &mainWindow, tempDirPath ] {
        mainWindow->loadInitialFile( QDir{ tempDirPath }.filePath( "b.log" ), false );
    } );
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 2; } ) );

    // Let any setup-triggered persistence fire and settle (the debounce
    // interval is sub-second), then start counting from a quiet baseline.
    QTest::qWait( 1200 );
    auto& saveCount = SessionInfo::saveCountForTesting();
    saveCount.store( 0 );

    runInUiThread( [ tabArea ] { tabArea->setCurrentIndex( 0 ); } );
    runInUiThread( [ tabArea ] { tabArea->setCurrentIndex( 1 ); } );

    // No synchronous session rewrite on the switch path...
    REQUIRE( saveCount.load() == 0 );

    // ...both switches coalesce into exactly one debounced write...
    REQUIRE( waitUiState( [ & ] { return saveCount.load() == 1; } ) );

    // ...and there is no write storm behind it.
    QTest::qWait( 400 );
    REQUIRE( saveCount.load() == 1 );

    runInUiThread( [ &mainWindow ] { mainWindow->close(); } );
    REQUIRE( waitUiState( [ & ] { return !mainWindow->isVisible(); } ) );
}

TEST_CASE( "Shared Filter Favorites model updates real file and folder toolbars synchronously",
           "[ui][filter-favorites]" )
{
    TabGroupCleanupGuard tabGroupCleanupGuard;
    const PredefinedFiltersCollection::Collection initial{ { QStringLiteral( "Alpha" ),
                                                             QStringLiteral( "ERROR" ), false } };
    FilterFavoritesRestoreGuard filterFavoritesGuard{ initial };
    auto& sessionInfo = SessionInfo::getSynced();
    SessionInfoRestoreGuard sessionInfoRestoreGuard{ sessionInfo };

    auto appSession = std::make_shared<Session>();
    const auto windowId = QString( "filter-favorites-apply-%1" )
                              .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, Qt::PreciseTimer,
                        [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );
    QTest::qWait( 100 );
    REQUIRE( mainWindow != nullptr );
    mainWindow->resize( 1600, 900 );
    mainWindow->show();
    QTest::qWait( 100 );

    const auto tempDirPath = makeTestDir( "filterfavoritesapply" );
    const auto standaloneFile = QDir( tempDirPath ).absoluteFilePath( "standalone.log" );
    QFile file( standaloneFile );
    REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( file.write( "ERROR one\n" ) > 0 );
    file.close();

    auto* tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );
    mainWindow->loadInitialFile( standaloneFile, false );
    REQUIRE( waitUiState( [ tabArea ] { return tabArea->count() == 1; } ) );
    auto* fileCrawler = qobject_cast<CrawlerWidget*>( tabArea->widget( 0 ) );
    REQUIRE( fileCrawler != nullptr );
    REQUIRE( waitUiState( [ fileCrawler ] { return fileCrawler->isFirstLoadDone(); } ) );
    QTest::qWait( 200 );

    mainWindow->openFolderByPath( tempDirPath );
    REQUIRE( waitUiState( [ tabArea ] { return tabArea->count() == 2; } ) );
    auto* folderCrawler = qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() );
    REQUIRE( folderCrawler != nullptr );
    QTest::qWait( 200 );

    auto* fileCombo = fileCrawler->findChild<PredefinedFiltersComboBox*>();
    auto* folderCombo = folderCrawler->findChild<PredefinedFiltersComboBox*>();
    REQUIRE( fileCombo != nullptr );
    REQUIRE( folderCombo != nullptr );
    REQUIRE( comboFavoriteRows( fileCombo ) == initial );
    REQUIRE( comboFavoriteRows( folderCombo ) == initial );

    const PredefinedFiltersCollection::Collection expected{
        initial.at( 0 ), { QStringLiteral( "Beta" ), QStringLiteral( "WARN" ), false }
    };
    const auto commit = FilterFavoritesModel::instance().replaceFavorites( initial, expected );
    REQUIRE( commit.status == PredefinedFiltersCollection::CommitStatus::Success );
    REQUIRE( comboFavoriteRows( fileCombo ) == expected );
    REQUIRE( comboFavoriteRows( folderCombo ) == expected );
    REQUIRE( tabArea->currentWidget() == folderCrawler );
}

TEST_CASE( "Filter Favorites import validates before replacing every document toolbar",
           "[ui][filter-favorites][import]" )
{
    TabGroupCleanupGuard tabGroupCleanupGuard;
    const PredefinedFiltersCollection::Collection initialFavorites{
        { QStringLiteral( "Alpha" ), QStringLiteral( "ERROR" ), false },
        { QStringLiteral( "Beta" ), QStringLiteral( "WARN.*" ), true },
    };
    FilterFavoritesRestoreGuard filterFavoritesGuard{ initialFavorites };
    auto& sessionInfo = SessionInfo::getSynced();
    SessionInfoRestoreGuard sessionInfoRestoreGuard{ sessionInfo };

    auto appSession = std::make_shared<Session>();
    const auto windowId = QString( "filter-favorites-import-%1" )
                              .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, Qt::PreciseTimer,
                        [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );
    QTest::qWait( 100 );
    REQUIRE( mainWindow != nullptr );
    mainWindow->resize( 1600, 900 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    const auto tempDirPath = makeTestDir( "filterfavoritesimport" );
    REQUIRE( QDir{ tempDirPath }.exists() );
    const auto standaloneFile = QDir( tempDirPath ).absoluteFilePath( "standalone.log" );
    const auto folderFile = QDir( tempDirPath ).absoluteFilePath( "folder.log" );
    for ( const auto& path : { standaloneFile, folderFile } ) {
        QFile file( path );
        REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        file.write( "ERROR one\nWARN two\n" );
    }

    auto* tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );
    runInUiThread(
        [ &mainWindow, standaloneFile ] { mainWindow->loadInitialFile( standaloneFile, false ); } );
    REQUIRE( waitUiState( [ tabArea ] { return tabArea->count() == 1; } ) );
    auto* fileCrawler = qobject_cast<CrawlerWidget*>( tabArea->widget( 0 ) );
    REQUIRE( fileCrawler != nullptr );
    REQUIRE( waitUiState( [ fileCrawler ] { return fileCrawler->isFirstLoadDone(); } ) );
    QTest::qWait( 200 );

    runInUiThread( [ &mainWindow, tempDirPath ] { mainWindow->openFolderByPath( tempDirPath ); } );
    REQUIRE( waitUiState( [ tabArea ] { return tabArea->count() == 2; } ) );
    auto* folderCrawler = qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() );
    REQUIRE( folderCrawler != nullptr );
    QTest::qWait( 200 );

    auto* fileCombo = fileCrawler->findChild<PredefinedFiltersComboBox*>();
    auto* folderCombo = folderCrawler->findChild<PredefinedFiltersComboBox*>();
    REQUIRE( fileCombo != nullptr );
    REQUIRE( folderCombo != nullptr );
    REQUIRE( comboFavoriteRows( fileCombo ) == initialFavorites );
    REQUIRE( comboFavoriteRows( folderCombo ) == initialFavorites );

    const PredefinedFiltersCollection::Collection importedFavorites{
        { QStringLiteral( "Gamma" ), QStringLiteral( "INFO|NOTICE" ), true },
        { QStringLiteral( "Delta" ), QStringLiteral( "plain text" ), false },
    };
    const auto importPath = QDir( tempDirPath ).absoluteFilePath( "valid-import.conf" );
    REQUIRE( PredefinedFiltersCollection::saveToFile( importPath, importedFavorites ) );

    // Make storage newer than the shared model. Import must first observe this
    // external state, then replace it authoritatively with the validated file.
    const PredefinedFiltersCollection::Collection externalFavorites{
        { QStringLiteral( "External" ), QStringLiteral( "EXTERNAL" ), false },
    };
    PredefinedFiltersCollection::getSynced().saveToStorage( externalFavorites );
    auto& favoritesModel = FilterFavoritesModel::instance();
    REQUIRE( favoritesModel.favorites() == initialFavorites );
    QSignalSpy resetSpy( &favoritesModel, &QAbstractItemModel::modelReset );

    const bool validImportInvoked
        = QMetaObject::invokeMethod( mainWindow.get(), "importFilterFavoritesFromFile",
                                     Qt::DirectConnection, Q_ARG( QString, importPath ) );
    REQUIRE( validImportInvoked );
    CHECK( resetSpy.count() == 2 );
    CHECK( favoritesModel.favorites() == importedFavorites );
    CHECK( comboFavoriteRows( fileCombo ) == importedFavorites );
    CHECK( comboFavoriteRows( folderCombo ) == importedFavorites );
    CHECK( tabArea->currentWidget() == folderCrawler );

    const auto malformedPath = QDir( tempDirPath ).absoluteFilePath( "invalid-import.conf" );
    {
        QFile malformedFile( malformedPath );
        REQUIRE( malformedFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        malformedFile.write( "not a filter favorites file\n" );
    }

    QStringList warningTexts;
    [[maybe_unused]] const klogg::ui::ScopedMessageHandler messageHandler{
        [ &warningTexts ]( klogg::ui::MessageKind kind, QWidget*, const QString&,
                           const QString& text ) {
            if ( kind == klogg::ui::MessageKind::Warning ) {
                warningTexts.push_back( text );
            }
        }
    };

    const bool invalidImportInvoked
        = QMetaObject::invokeMethod( mainWindow.get(), "importFilterFavoritesFromFile",
                                     Qt::DirectConnection, Q_ARG( QString, malformedPath ) );
    REQUIRE( invalidImportInvoked );
    REQUIRE( warningTexts.size() == 1 );
    CHECK_FALSE( warningTexts.front().isEmpty() );
    CHECK( favoritesModel.favorites() == importedFavorites );
    CHECK( PredefinedFiltersCollection::getSynced().getFilters() == importedFavorites );
    CHECK( comboFavoriteRows( fileCombo ) == importedFavorites );
    CHECK( comboFavoriteRows( folderCombo ) == importedFavorites );

    const auto unwritableExportPath
        = QDir( tempDirPath ).absoluteFilePath( QStringLiteral( "export-destination" ) );
    const auto blockedExportPath = unwritableExportPath + QStringLiteral( ".conf" );
    REQUIRE( QDir{}.mkpath( blockedExportPath ) );
    const bool failedExportInvoked
        = QMetaObject::invokeMethod( mainWindow.get(), "exportFilterFavoritesToFile",
                                     Qt::DirectConnection, Q_ARG( QString, unwritableExportPath ) );
    REQUIRE( failedExportInvoked );
    REQUIRE( warningTexts.size() == 2 );
    CHECK_FALSE( warningTexts.back().isEmpty() );
    CHECK( QFileInfo( blockedExportPath ).isDir() );
}

TEST_CASE( "Filter Favorites import maps every load status to explicit warning behavior",
           "[ui][filter-favorites][import]" )
{
    TabGroupCleanupGuard tabGroupCleanupGuard;
    FilterFavoritesRestoreGuard filterFavoritesGuard{ {} };
    auto& sessionInfo = SessionInfo::getSynced();
    SessionInfoRestoreGuard sessionInfoRestoreGuard{ sessionInfo };

    auto appSession = std::make_shared<Session>();
    const auto windowId = QString( "filter-favorites-import-errors-%1" )
                              .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, Qt::PreciseTimer,
                        [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );
    QTest::qWait( 100 );
    REQUIRE( mainWindow != nullptr );

    const auto tempDirPath = makeTestDir( "filterfavoritesimporterrors" );
    REQUIRE( QDir{ tempDirPath }.exists() );

    const auto validPath = QDir( tempDirPath ).absoluteFilePath( "valid.conf" );
    const PredefinedFiltersCollection::Collection importedFavorites{
        { QStringLiteral( "Imported" ), QStringLiteral( "pattern" ), false }
    };
    REQUIRE( PredefinedFiltersCollection::saveToFile( validPath, importedFavorites ) );

    const auto missingPath = QDir( tempDirPath ).absoluteFilePath( "missing.conf" );
    REQUIRE_FALSE( QFileInfo::exists( missingPath ) );

    const auto malformedPath = QDir( tempDirPath ).absoluteFilePath( "malformed.conf" );
    {
        QFile malformedFile( malformedPath );
        REQUIRE( malformedFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        REQUIRE( malformedFile.write( "not a filter favorites file\n" ) > 0 );
    }

    const auto unsupportedPath = QDir( tempDirPath ).absoluteFilePath( "future.conf" );
    {
        QFile unsupportedFile( unsupportedPath );
        REQUIRE( unsupportedFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        REQUIRE( unsupportedFile.write( "[PredefinedFiltersCollection]\nversion=999\n" ) > 0 );
    }

    QStringList warningTitles;
    QStringList warningTexts;
    [[maybe_unused]] const klogg::ui::ScopedMessageHandler messageHandler{
        [ & ]( klogg::ui::MessageKind kind, QWidget*, const QString& title, const QString& text ) {
            if ( kind == klogg::ui::MessageKind::Warning ) {
                warningTitles.push_back( title );
                warningTexts.push_back( text );
            }
        }
    };

    using LoadStatus = PredefinedFiltersCollection::LoadStatus;
    struct ImportCase {
        QString path;
        LoadStatus expectedStatus;
        QString expectedWarning;
    };
    const QList<ImportCase> cases{
        { validPath, LoadStatus::Success, {} },
        { missingPath, LoadStatus::MissingFile,
          QStringLiteral( "The selected filter favorites file does not exist." ) },
        { malformedPath, LoadStatus::MalformedFile,
          QStringLiteral( "The selected file is not a valid filter favorites file." ) },
        { unsupportedPath, LoadStatus::UnsupportedVersion,
          QStringLiteral(
              "The selected filter favorites file was created by a newer version of klogg." ) },
    };

    for ( const auto& importCase : cases ) {
        CHECK( PredefinedFiltersCollection::tryLoadFromFile( importCase.path ).status
               == importCase.expectedStatus );
        const auto warningCountBefore = warningTexts.size();
        const bool invoked
            = QMetaObject::invokeMethod( mainWindow.get(), "importFilterFavoritesFromFile",
                                         Qt::DirectConnection, Q_ARG( QString, importCase.path ) );
        REQUIRE( invoked );

        if ( importCase.expectedWarning.isEmpty() ) {
            CHECK( warningTexts.size() == warningCountBefore );
        }
        else {
            REQUIRE( warningTexts.size() == warningCountBefore + 1 );
            CHECK( warningTitles.back() == QStringLiteral( "klogg" ) );
            CHECK( warningTexts.back() == importCase.expectedWarning );
        }
    }

    CHECK( FilterFavoritesModel::instance().favorites() == importedFavorites );
}

TEST_CASE( "FavoriteFiles removal is idempotent", "[ui][favorite-files]" )
{
    const auto tempDirPath = makeTestDir( "favoritefilesremove" );
    REQUIRE( QDir{ tempDirPath }.exists() );
    const auto favoritePath = QDir( tempDirPath ).absoluteFilePath( "favorite.log" );
    FavoriteFilesRestoreGuard favoriteFilesGuard{ { favoritePath } };

    auto& favorites = FavoriteFiles::getSynced();
    favorites.remove( QDir( tempDirPath ).absoluteFilePath( "not-a-favorite.log" ) );

    const auto current = favorites.favorites();
    REQUIRE( current.size() == 1 );
    CHECK( current.front().fullPath() == favoritePath );
}

TEST_CASE( "Favorite Files menu has stable commands and refreshes on show", "[ui][menu-text]" )
{
    TabGroupCleanupGuard tabGroupCleanupGuard;
    auto& sessionInfo = SessionInfo::getSynced();
    SessionInfoRestoreGuard sessionInfoRestoreGuard{ sessionInfo };

    const auto tempDirPath = makeTestDir( "favoritefilesmenu" );
    REQUIRE( QDir{ tempDirPath }.exists() );
    const auto firstFavorite = QDir( tempDirPath ).absoluteFilePath( "alpha.log" );
    const auto replacementFavorite = QDir( tempDirPath ).absoluteFilePath( "beta.log" );
    const auto folderFile = QDir( tempDirPath ).absoluteFilePath( "folder.log" );
    for ( const auto& path : { firstFavorite, replacementFavorite, folderFile } ) {
        QFile file( path );
        REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        file.write( "line\n" );
    }
    FavoriteFilesRestoreGuard favoriteFilesGuard{ { firstFavorite } };

    auto appSession = std::make_shared<Session>();
    const auto windowId = QString( "favorite-files-menu-%1" )
                              .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, Qt::PreciseTimer,
                        [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );
    QTest::qWait( 100 );
    REQUIRE( mainWindow != nullptr );
    mainWindow->show();
    QTest::qWait( 100 );

    auto* favoritesMenu = mainWindow->findChild<QMenu*>( QStringLiteral( "favoritesMenu" ) );
    auto* addCurrentFileAction
        = mainWindow->findChild<QAction*>( QStringLiteral( "addToFavoritesMenuAction" ) );
    auto* removeFavoriteFileAction
        = mainWindow->findChild<QAction*>( QStringLiteral( "removeFromFavoritesAction" ) );
    auto* toggleCurrentFileFavoriteAction
        = mainWindow->findChild<QAction*>( QStringLiteral( "addToFavoritesAction" ) );
    REQUIRE( favoritesMenu != nullptr );
    REQUIRE( addCurrentFileAction != nullptr );
    REQUIRE( removeFavoriteFileAction != nullptr );
    REQUIRE( toggleCurrentFileFavoriteAction != nullptr );
    CHECK( visibleMenuText( favoritesMenu->title() ) == QStringLiteral( "Favorite Files" ) );

    const auto stableActions = favoritesMenu->actions();
    REQUIRE( stableActions.size() >= 3 );
    CHECK( stableActions.at( 0 ) == addCurrentFileAction );
    CHECK( stableActions.at( 1 ) == removeFavoriteFileAction );
    REQUIRE( stableActions.at( 2 )->isSeparator() );
    CHECK( visibleMenuText( addCurrentFileAction->text() )
           == QStringLiteral( "Add Current File" ) );
    CHECK( visibleMenuText( removeFavoriteFileAction->text() )
           == QStringLiteral( "Remove Favorite File..." ) );
    CHECK( visibleMenuText( toggleCurrentFileFavoriteAction->text() )
           == QStringLiteral( "Add Current File to Favorites" ) );
    CHECK_FALSE( toggleCurrentFileFavoriteAction->text().endsWith( QStringLiteral( "..." ) ) );

    // No file document is active at construction: Add is unavailable, while
    // Remove reflects the seeded persistent collection.
    CHECK_FALSE( addCurrentFileAction->isEnabled() );
    CHECK( removeFavoriteFileAction->isEnabled() );
    CHECK( favoriteActionPaths( favoritesMenu ) == QStringList{ firstFavorite } );
    int dynamicFavoriteActionCount = 0;
    for ( const auto* action : favoritesMenu->actions() ) {
        if ( action->objectName() == QStringLiteral( "favoriteFileAction" ) ) {
            ++dynamicFavoriteActionCount;
            CHECK( action->menuRole() == QAction::NoRole );
        }
    }
    CHECK( dynamicFavoriteActionCount == 1 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::PreciseTimer, uiObject, std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };
    auto* tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );
    runInUiThread( [ &mainWindow, tempDirPath ] { mainWindow->openFolderByPath( tempDirPath ); } );
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 1; } ) );
    REQUIRE( qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() ) != nullptr );
    QTest::qWait( 200 );

    // A folder is not a file document, so showing the menu keeps Add disabled.
    Q_EMIT favoritesMenu->aboutToShow();
    QCoreApplication::processEvents();
    CHECK_FALSE( addCurrentFileAction->isEnabled() );
    CHECK( removeFavoriteFileAction->isEnabled() );

    // Simulate another window/process changing persisted favorites. aboutToShow
    // is the menu's synchronization boundary: stale entries and enabled state
    // must be rebuilt from FavoriteFiles::getSynced() each time.
    replaceFavoriteFiles( { replacementFavorite } );
    Q_EMIT favoritesMenu->aboutToShow();
    QCoreApplication::processEvents();
    CHECK( favoriteActionPaths( favoritesMenu ) == QStringList{ replacementFavorite } );
    CHECK_FALSE( addCurrentFileAction->isEnabled() );
    CHECK( removeFavoriteFileAction->isEnabled() );

    replaceFavoriteFiles( {} );
    Q_EMIT favoritesMenu->aboutToShow();
    QCoreApplication::processEvents();
    CHECK( favoriteActionPaths( favoritesMenu ).isEmpty() );
    CHECK_FALSE( addCurrentFileAction->isEnabled() );
    CHECK_FALSE( removeFavoriteFileAction->isEnabled() );

    // The toolbar action is an immediate toggle, not the chooser command used
    // by the menu. Its text must describe the active-file operation without an
    // ellipsis in both states.
    runInUiThread( [ &mainWindow, replacementFavorite ] {
        mainWindow->loadInitialFile( replacementFavorite, false );
    } );
    REQUIRE( waitUiState( [ & ] { return tabArea->count() == 2; } ) );
    auto* fileCrawler = qobject_cast<CrawlerWidget*>( tabArea->currentWidget() );
    REQUIRE( fileCrawler != nullptr );
    REQUIRE( waitUiState( [ fileCrawler ] { return fileCrawler->isFirstLoadDone(); } ) );
    QTest::qWait( 200 );

    CHECK( toggleCurrentFileFavoriteAction->isEnabled() );
    CHECK( visibleMenuText( toggleCurrentFileFavoriteAction->text() )
           == QStringLiteral( "Add Current File to Favorites" ) );

    // Another window adds the current file after this window rendered its stale
    // "Add" state. Trigger-time synchronization must derive the operation from
    // the fresh collection, not QAction::data: this click removes the favorite.
    replaceFavoriteFiles( { replacementFavorite } );
    CHECK( visibleMenuText( toggleCurrentFileFavoriteAction->text() )
           == QStringLiteral( "Add Current File to Favorites" ) );
    toggleCurrentFileFavoriteAction->trigger();
    CHECK( FavoriteFiles::getSynced().favorites().empty() );
    CHECK( visibleMenuText( toggleCurrentFileFavoriteAction->text() )
           == QStringLiteral( "Add Current File to Favorites" ) );

    // A second click observes the now-empty collection and adds the current file.
    toggleCurrentFileFavoriteAction->trigger();
    const auto currentFavorites = FavoriteFiles::getSynced().favorites();
    REQUIRE( currentFavorites.size() == 1 );
    CHECK( currentFavorites.front().fullPath() == replacementFavorite );
    CHECK( visibleMenuText( toggleCurrentFileFavoriteAction->text() )
           == QStringLiteral( "Remove Current File from Favorites" ) );
    CHECK_FALSE( toggleCurrentFileFavoriteAction->text().endsWith( QStringLiteral( "..." ) ) );
}

TEST_CASE( "File and Help menu labels use the public wording contract", "[ui][menu-text]" )
{
    auto& sessionInfo = SessionInfo::getSynced();
    SessionInfoRestoreGuard sessionInfoRestoreGuard{ sessionInfo };
    auto appSession = std::make_shared<Session>();
    const auto windowId = QString( "main-menu-text-%1" )
                              .arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, Qt::PreciseTimer,
                        [ & ] { mainWindow.reset( new MainWindow( windowSession ) ); } );
    QTest::qWait( 100 );
    REQUIRE( mainWindow != nullptr );

    auto* fileMenu = findTopLevelMenu( mainWindow.get(), QStringLiteral( "File" ) );
    auto* helpMenu = findTopLevelMenu( mainWindow.get(), QStringLiteral( "Help" ) );
    REQUIRE( fileMenu != nullptr );
    REQUIRE( helpMenu != nullptr );

    auto* openFile = mainWindow->findChild<QAction*>( QStringLiteral( "openAction" ) );
    auto* openFolder = mainWindow->findChild<QAction*>( QStringLiteral( "openFolderAction" ) );
    auto* openAdb = mainWindow->findChild<QAction*>( QStringLiteral( "openAdbLogcatAction" ) );
    auto* openClipboard
        = mainWindow->findChild<QAction*>( QStringLiteral( "openClipboardAction" ) );
    auto* openUrl = mainWindow->findChild<QAction*>( QStringLiteral( "openUrlAction" ) );
    auto* documentation
        = mainWindow->findChild<QAction*>( QStringLiteral( "showDocumentationAction" ) );
    auto* reportIssue = mainWindow->findChild<QAction*>( QStringLiteral( "reportIssueAction" ) );
    auto* goToTop = mainWindow->findChild<QAction*>( QStringLiteral( "goToTopAction" ) );
    auto* trayIcon = mainWindow->findChild<QSystemTrayIcon*>();

    REQUIRE( openFile != nullptr );
    REQUIRE( openFolder != nullptr );
    REQUIRE( openAdb != nullptr );
    REQUIRE( openClipboard != nullptr );
    REQUIRE( openUrl != nullptr );
    REQUIRE( documentation != nullptr );
    REQUIRE( reportIssue != nullptr );
    REQUIRE( goToTop != nullptr );
    REQUIRE( trayIcon != nullptr );

    CHECK( visibleMenuText( openFile->text() ) == QStringLiteral( "Open..." ) );
    CHECK( visibleMenuText( openFolder->text() ) == QStringLiteral( "Open Folder..." ) );
    CHECK( visibleMenuText( openAdb->text() ) == QStringLiteral( "Open ADB Logcat..." ) );
    CHECK( visibleMenuText( openClipboard->text() ) == QStringLiteral( "Open from Clipboard" ) );
    CHECK( visibleMenuText( openUrl->text() ) == QStringLiteral( "Open from URL..." ) );
    CHECK( visibleMenuText( documentation->text() ) == QStringLiteral( "Documentation" ) );
    CHECK( visibleMenuText( reportIssue->text() ) == QStringLiteral( "Report Issue" ) );
    CHECK( trayIcon->toolTip() == QStringLiteral( "klogg log viewer" ) );

    goToTop->setText( QStringLiteral( "stale text" ) );
    mainWindow->reTranslateUI();
    CHECK( goToTop->text() == QStringLiteral( "Go to &Top" ) );
}
