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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include <QMenu>
#include <QMenuBar>
#include <QSignalSpy>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUuid>

#include <QToolBar>

#include "test_utils.h"

#include "adblogcatsource.h"
#include "capturestore.h"
#include "crawlerwidget.h"
#include "log.h"
#include "mainwindow.h"
#include "persistentinfo.h"
#include "session.h"
#include "sessioninfo.h"
#include "tabgroup.h"

namespace {
QString makeTestDir( const QString& prefix )
{
    const auto dirPath = QDir::cleanPath( QDir::currentPath() + QDir::separator()
                                          + QLatin1String( "test_tmp" ) + QDir::separator()
                                          + prefix + QLatin1Char( '_' )
                                          + QUuid::createUuid().toString( QUuid::WithoutBraces ) );
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
} // namespace

SCENARIO( "Main window tests", "[ui]" )
{
    auto appSession = std::make_shared<Session>();
    WindowSession windowSession{ appSession, "Main", 0 };

    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<SafeQSignalSpy> activateSpy;
    std::unique_ptr<SafeQSignalSpy> exitSpy;
    QTimer::singleShot( 0, [&] {
        LOG_INFO << "Initialize main window";
        mainWindow.reset( new MainWindow( windowSession ) );
        exitSpy.reset( new SafeQSignalSpy( mainWindow.get(), SIGNAL( exitRequested() ) ) );
        activateSpy.reset( new SafeQSignalSpy( mainWindow.get(), SIGNAL( windowActivated() ) ) );
    } );

    QTest::qWait( 100 );
    mainWindow->show();
    QTest::qWait( 100 );
    REQUIRE( activateSpy->safeWait() );

    auto runInUiThread = [uiObject = mainWindow.get()]( auto&& func ) {
        QTimer::singleShot( 0, Qt::VeryCoarseTimer, uiObject,
                            std::forward<decltype( func )>( func ) );
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

        auto* saveLiveLogMenu = mainWindow->findChild<QMenu*>(
            QStringLiteral( "saveCurrentLiveLogMenu" ) );
        REQUIRE( saveLiveLogMenu != nullptr );
        REQUIRE_FALSE( saveLiveLogMenu->isEnabled() );
        REQUIRE( mainWindow->findChild<QAction*>(
                     QStringLiteral( "saveCurrentLiveLogStripAnsiAction" ) )
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

        QAction* closeAction = mainWindow->findChild<QAction*>(
            QStringLiteral( "closeAction" ) );
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
            runInUiThread( [&mainWindow, testFilePath] {
                LOG_INFO << "Load file";
                mainWindow->loadInitialFile( testFilePath, false );
            } );

            THEN( "Path line has file name" )
            {
                REQUIRE(
                    waitUiState( [&] { return filePathLabel->text().contains( "klogg.conf" ); } ) );

                AND_THEN( "Has one tab" )
                {
                    REQUIRE( waitUiState( [&] { return tabArea->count() == 1; } ) );
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
                REQUIRE( waitUiState( [&] {
                    auto* crawler = qobject_cast<CrawlerWidget*>(tabArea->currentWidget());
                    return crawler != nullptr && crawler->isFirstLoadDone();
                } ) );

                // Let the worker thread fully unwind before destroying the
                // tab.  Even after isFirstLoadDone() returns true, the
                // background thread may still be cleaning up — closing the
                // tab during that window causes use-after-free.
                QTest::qWait( 200 );

                runInUiThread( [closeAction] {
                    LOG_INFO << "Close tab";
                    closeAction->trigger();
                } );

                THEN( "Has no tabs" )
                {
                    REQUIRE( waitUiState( [&] { return tabArea->count() == 0; } ) );

                    AND_THEN( "Path label empty" )
                    {
                        REQUIRE( waitUiState( [&] { return filePathLabel->text().isEmpty(); } ) );
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
    const auto windowId = QString( "tab-group-chip-%1" ).arg(
        QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [&] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->resize( 1600, 900 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [uiObject = mainWindow.get()]( auto&& func ) {
        QTimer::singleShot( 0, Qt::VeryCoarseTimer, uiObject,
                            std::forward<decltype( func )>( func ) );
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

    runInUiThread( [&mainWindow, firstFilePath, secondFilePath] {
        mainWindow->loadInitialFile( firstFilePath, false );
        mainWindow->loadInitialFile( secondFilePath, false );
    } );

    REQUIRE( waitUiState( [&] { return tabArea->count() == 2; } ) );
    REQUIRE( waitUiState( [&] { return tabBar->count() == 2 && tabBar->isVisible(); } ) );

    QString groupId;
    runInUiThread( [ &groupId, firstFilePath ] {
        auto& groupManager = TabGroupManager::get();
        groupManager.createGroup( "C", QColor( "#D96C1A" ) );
        groupId = groupManager.groups().back().id;
        groupManager.addTabToGroup( groupId, firstFilePath );
        groupManager.save();
    } );

    // runInUiThread's internal QTest::qWait(100) is not always enough on
    // slow runners (Ubuntu 20.04 docker has missed the 100 ms budget,
    // leaving groupId empty before this REQUIRE).  waitUiState polls
    // up to 10 s, so the assertion either succeeds quickly on a healthy
    // runner or fails with a clear timeout instead of a misleading
    // "groupId is empty" diagnostic.
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

    const auto renameGroupAndVerify = [ &runInUiThread, &groupId, &verifyGroupChipName ](
                                          const QString& groupName ) -> int {
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
    auto* view = session.openMerged(
        sources, []() { return new CrawlerWidget(); }, tempDir );
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
            const auto merged
                = readMergedFile( *appSession, { file1, file2, file3 }, tempDirPath );

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
                              ? QString( "close-session-%1" ).arg(
                                    QUuid::createUuid().toString( QUuid::WithoutBraces ) )
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
    QTimer::singleShot( 0, [&] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [uiObject = mainWindow.get()]( auto&& func ) {
        QTimer::singleShot( 0, Qt::VeryCoarseTimer, uiObject,
                            std::forward<decltype( func )>( func ) );
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

    runInUiThread( [&mainWindow, testFilePath] { mainWindow->loadInitialFile( testFilePath, false ); } );
    REQUIRE( waitUiState( [&] { return tabArea->count() == 1; } ) );
    REQUIRE( waitUiState( [&] { return SessionInfo::getSynced().openFiles( windowId ).size() == 1; } ) );

    runInUiThread( [&mainWindow] { mainWindow->close(); } );
    REQUIRE( waitUiState( [&] { return !mainWindow->isVisible(); } ) );

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
                              ? QString( "close-adb-session-%1" ).arg(
                                    QUuid::createUuid().toString( QUuid::WithoutBraces ) )
                              : windowIds.front();

    if ( windowIds.isEmpty() ) {
        sessionInfo.add( windowId );
    }
    else {
        for ( auto i = windowIds.size() - 1; i > 0; --i ) {
            sessionInfo.remove( windowIds.at( i ) );
        }
    }

    const auto captureId = QString( "adb_capture_%1" ).arg(
        QUuid::createUuid().toString( QUuid::WithoutBraces ) );
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
    const auto sourceSpec = QString::fromUtf8(
        QJsonDocument( adbSessionData.toJson() ).toJson( QJsonDocument::Compact ) );

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
    QTimer::singleShot( 0, [&] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::VeryCoarseTimer, uiObject,
                            std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    auto tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );

    runInUiThread( [&mainWindow] { mainWindow->reloadSession(); } );
    REQUIRE( waitUiState( [&] { return tabArea->count() == 1; } ) );

    runInUiThread( [&mainWindow] { mainWindow->close(); } );
    REQUIRE( waitUiState( [&] { return !mainWindow->isVisible(); } ) );

    REQUIRE( QDir{ capturePath }.exists() );
    const auto persistedOpenFiles = SessionInfo::getSynced().openFiles( windowId );
    REQUIRE( persistedOpenFiles.size() == 1 );
    REQUIRE( persistedOpenFiles.front().sourceType == QStringLiteral( "adb_logcat" ) );

    config.setMinimizeToTray( previousMinimizeToTray );
    config.save();
}

SCENARIO( "MainWindow restored iOS live log tabs show disconnected state",
          "[ui][session][ios]" )
{
    auto appSession = std::make_shared<Session>();
    auto& sessionInfo = SessionInfo::getSynced();
    SessionInfoRestoreGuard sessionInfoRestoreGuard{ sessionInfo };
    const auto windowIds = sessionInfo.windows();
    const auto windowId = QString( "restore-ios-session-%1" ).arg(
        QUuid::createUuid().toString( QUuid::WithoutBraces ) );

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
    const auto sourceSpec = QString::fromUtf8(
        QJsonDocument( iosSessionData.toJson() ).toJson( QJsonDocument::Compact ) );

    sessionInfo.setOpenFiles(
        windowId, { SessionInfo::OpenFile( iosSessionData.documentId(), 0, {},
                                           iosSessionData.persistedSourceType(),
                                           iosSessionData.displayName(), sourceSpec ) } );
    sessionInfo.setCurrentFileIndex( windowId, 0 );
    sessionInfo.save();

    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [&] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::VeryCoarseTimer, uiObject,
                            std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    auto tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabArea != nullptr );

    runInUiThread( [&mainWindow] { mainWindow->reloadSession(); } );
    REQUIRE( waitUiState( [&] { return tabArea->count() == 1; } ) );
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

    auto* view = appSession->openAdbLogcat( adbSessionData, []() { return new CrawlerWidget(); },
                                            false );
    REQUIRE( view != nullptr );

    REQUIRE( appSession->getAssociatedPath( view ).isEmpty() );
    auto* adbSource = appSession->getAdbLogcatSource( view );
    REQUIRE( adbSource != nullptr );
    REQUIRE( adbSource->sessionData().boundOutputFile.isEmpty() );

    appSession->close( view );
}
