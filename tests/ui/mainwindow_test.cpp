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
#include <QClipboard>
#include <QLineEdit>
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
#include "foldercrawlerwidget.h"
#include "folderfilteredview.h"
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

// Regression gate for the folder-tab crash: opening/switching-to/closing a
// FolderCrawlerWidget tab inside MainWindow used to EXC_BAD_ACCESS because
// currentTabChanged did a static_cast<CrawlerWidget*> on the folder widget and
// dereferenced the garbage pointer. None of the existing itests opened a
// folder tab through MainWindow, so the bug was invisible. This scenario
// drives the real path (Session::openFolder + MainWindow::openFolderByPath +
// TabbedCrawlerWidget::addCrawler, which calls setCurrentIndex -> the crash
// site) and asserts it stays alive.
SCENARIO( "Folder tab in MainWindow does not crash on open/switch/close",
          "[ui][folder]" )
{
    TabGroupCleanupGuard tabGroupCleanupGuard;

    auto appSession = std::make_shared<Session>();
    const auto windowId = QString( "folder-tab-%1" ).arg(
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
            runInUiThread( [ &mainWindow, tempDirPath ] {
                mainWindow->openFolderByPath( tempDirPath );
            } );

            THEN( "The folder tab is added without crashing" )
            {
                REQUIRE( waitUiState( [&] { return tabArea->count() == 1; } ) );
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
                        REQUIRE( waitUiState( [&] { return tabArea->count() == 2; } ) );
                        QTest::qWait( 200 );
                        REQUIRE( qobject_cast<CrawlerWidget*>(
                                     tabArea->currentWidget() )
                                 != nullptr );

                        AND_WHEN( "Switching back to the folder tab (index 0)" )
                        {
                            runInUiThread( [ tabArea ] {
                                tabArea->setCurrentIndex( 0 );
                            } );

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
                                    runInUiThread( [ tabArea ] {
                                        tabArea->setCurrentIndex( 1 );
                                    } );

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
                                                [&] { return tabArea->count() == 1; } ) );
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
            runInUiThread( [ &mainWindow, tempDirPath ] {
                mainWindow->openFolderByPath( tempDirPath );
            } );
            REQUIRE( waitUiState( [&] { return tabArea->count() == 1; } ) );
            QTest::qWait( 200 );
            REQUIRE( qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() )
                     != nullptr );

            // closeTab on the folder current tab exercises the folder close
            // branch (BEFORE the CrawlerWidget assert) and then the no-tab-left
            // else branch in currentTabChanged. Triggered via closeAction
            // (closeTab is a private slot).
            runInUiThread( [ closeAction ] {
                closeAction->trigger();
            } );

            THEN( "No assert-abort and no tabs remain" )
            {
                REQUIRE( waitUiState( [&] { return tabArea->count() == 0; } ) );
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

    auto appSession = std::make_shared<Session>();
    const auto windowId = QString( "folder-dispatch-%1" ).arg(
        QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [&] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->resize( 1600, 900 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::VeryCoarseTimer, uiObject,
                            std::forward<decltype( func )>( func ) );
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

    runInUiThread( [ &mainWindow, tempDirPath ] {
        mainWindow->openFolderByPath( tempDirPath );
    } );
    REQUIRE( waitUiState( [&] { return tabArea->count() == 1; } ) );
    QTest::qWait( 200 );
    auto* folderWidget = qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() );
    REQUIRE( folderWidget != nullptr );

    WHEN( "the wrap action is toggled" )
    {
        auto* wrapAction = mainWindow->findChild<QAction*>( QStringLiteral( "textWrapAction" ) );
        REQUIRE( wrapAction != nullptr );

        // State-agnostic: the default wrap state comes from the machine's
        // saved Configuration, so drive both directions.
        runInUiThread( [ wrapAction ] {
            wrapAction->setChecked( true );
        } );

        THEN( "the folder views wrap and unwrap" )
        {
            REQUIRE( waitUiState( [ & ] { return folderWidget->isTextWrapEnabled(); } ) );
            REQUIRE( folderWidget->mainView()->isTextWrapEnabled() );

            runInUiThread( [ wrapAction ] {
                wrapAction->setChecked( false );
            } );
            REQUIRE( waitUiState( [ & ] { return !folderWidget->isTextWrapEnabled(); } ) );
            REQUIRE_FALSE( folderWidget->mainView()->isTextWrapEnabled() );
        }
    }

    WHEN( "the focus-search shortcut is pressed" )
    {
        THEN( "the folder search input gains focus" )
        {
            pressConfiguredShortcut( mainWindow.get(),
                                     ShortcutAction::MainWindowFocusSearchInput );
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

        THEN( "document actions are enabled, file-only actions are not" )
        {
            // The folder branch of currentTabChanged explicitly re-enables the
            // wrap toggle and syncs its checked state from the folder views.
            REQUIRE( wrap != nullptr );
            REQUIRE( wrap->isEnabled() );
            REQUIRE( wrap->isChecked() == folderWidget->isTextWrapEnabled() );
            // Go-to-line is routed polymorphically and stays available.
            REQUIRE( goToLine != nullptr );
            REQUIRE( goToLine->isEnabled() );
            // Follow stays file/live-source-only.
            REQUIRE( follow != nullptr );
            REQUIRE_FALSE( follow->isChecked() );
        }
    }

    WHEN( "a result file is open and Copy Path is triggered" )    {
        // The info line shows the file in the folder main view; Copy Path must
        // agree with it (not blindly copy the folder path).
        auto* copyPath = mainWindow->findChild<QAction*>(
            QStringLiteral( "copyPathToClipboardAction" ) );
        REQUIRE( copyPath != nullptr );

        runInUiThread( [ folderWidget ] {
            folderWidget->searchFor( "ERROR" );
        } );
        REQUIRE( waitUiState( [ & ] { return !folderWidget->isSearchActive(); } ) );

        const auto expectedPath = QDir( tempDirPath ).absoluteFilePath( "a.log" );
        runInUiThread( [ folderWidget ] {
            folderWidget->selectResultRow( 1_lnum );
        } );
        // Qt 5.12 VeryCoarseTimer (ubuntu-20.04 AppImage) may delay the
        // single-shot dispatch, shrinking the 10s waitUiState budget.
        // Give the timer a generous settle window so the async file-open
        // gets a full budget on slower CI runners.
        QTest::qWait( 2000 );
        REQUIRE( waitUiState(
            [ & ] { return folderWidget->currentMainFilePath() == expectedPath; } ) );

        THEN( "the clipboard holds the main-view file path" )
        {
            runInUiThread( [ copyPath ] {
                copyPath->trigger();
            } );
            REQUIRE( waitUiState( [ & ] {
                return QApplication::clipboard()->text() == QDir::toNativeSeparators( expectedPath );
            } ) );
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
    const auto windowId = QString( "folder-qf-%1" ).arg(
        QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    WindowSession windowSession{ appSession, windowId, 0 };

    std::unique_ptr<MainWindow> mainWindow;
    QTimer::singleShot( 0, [&] { mainWindow.reset( new MainWindow( windowSession ) ); } );

    QTest::qWait( 100 );
    mainWindow->resize( 1600, 900 );
    mainWindow->show();
    QTest::qWait( 100 );

    auto runInUiThread = [ uiObject = mainWindow.get() ]( auto&& func ) {
        QTimer::singleShot( 0, Qt::VeryCoarseTimer, uiObject,
                            std::forward<decltype( func )>( func ) );
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

    runInUiThread( [ &mainWindow, tempDirPath ] {
        mainWindow->openFolderByPath( tempDirPath );
    } );
    REQUIRE( waitUiState( [&] { return tabArea->count() == 1; } ) );
    QTest::qWait( 200 );
    auto* folderWidget = qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() );
    REQUIRE( folderWidget != nullptr );

    // Search, snapshot pane 0 (keep-results) and search again in a fresh pane.
    runInUiThread( [ folderWidget ] {
        folderWidget->searchFor( "ERROR" );
    } );
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
            Q_EMIT folderWidget->filteredView()->changeQuickFind(
                QStringLiteral( "beta" ), QuickFindMux::Forward );
            REQUIRE( waitUiState( [ & ] {
                return appSession->quickFindPattern()->getPattern()
                       == QStringLiteral( "beta" );
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
        runInUiThread( [ folderWidget ] {
            Q_EMIT folderWidget->resultsTabs()->tabCloseRequested( 0 );
        } );
        REQUIRE( waitUiState( [ & ] { return folderWidget->paneCount() == 1; } ) );

        THEN( "QuickFind still follows the surviving pane" )
        {
            Q_EMIT folderWidget->filteredView()->changeQuickFind(
                QStringLiteral( "gamma" ), QuickFindMux::Forward );
            REQUIRE( waitUiState( [ & ] {
                return appSession->quickFindPattern()->getPattern()
                       == QStringLiteral( "gamma" );
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
        runInUiThread( [ &mainWindow, tempDirPath2 ] {
            mainWindow->openFolderByPath( tempDirPath2 );
        } );
        REQUIRE( waitUiState( [&] { return tabArea->count() == 2; } ) );
        QTest::qWait( 200 );
        auto* foreground = qobject_cast<FolderCrawlerWidget*>( tabArea->currentWidget() );
        REQUIRE( foreground != nullptr );
        REQUIRE( foreground != folderWidget );

        // Drive the BACKGROUND folder's pane lifecycle: pane 1 is active from
        // the keep-results setup, so switching to 0 emits searchablesChanged.
        runInUiThread( [ folderWidget ] {
            folderWidget->resultsTabs()->setCurrentIndex( 0 );
        } );
        QTest::qWait( 200 );

        THEN( "the mux stays with the foreground document" )
        {
            Q_EMIT foreground->filteredView()->changeQuickFind(
                QStringLiteral( "delta" ), QuickFindMux::Forward );
            REQUIRE( waitUiState( [ & ] {
                return appSession->quickFindPattern()->getPattern()
                       == QStringLiteral( "delta" );
            } ) );
        }
    }
}
