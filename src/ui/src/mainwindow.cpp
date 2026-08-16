/*
 * Copyright (C) 2009, 2010, 2011, 2013, 2014 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

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

// This file implements MainWindow. It is responsible for creating and
// managing the menus, the toolbar, and the CrawlerWidget. It also
// load/save the settings on opening/closing of the app

#include "configuration.h"
#include "containers.h"
#include "log.h"
#include <QNetworkReply>
#include <cassert>
#include <exception>

#include <iterator>
#include <qaction.h>
#include <qapplication.h>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif // Q_OS_WIN

#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListView>
#include <QMenuBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>
#include <QResource>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QStringListModel>
#include <QTemporaryFile>
#include <QTextBrowser>
#include <QToolBar>
#include <QToolTip>
#include <QUrl>
#include <QUrlQuery>
#include <QWindow>

#include "mainwindow.h"

#include "adblogcatdialog.h"
#include "adblogcatsource.h"
#include "clipboard.h"
#include "crawlerwidget.h"
#include "decompressor.h"
#include "foldercrawlerwidget.h"
#include "abstractlogview.h"
#include "folderenumeration.h"
#include "dispatch_to.h"
#include "downloader.h"
#include "encodings.h"
#include "favoritefiles.h"
#include "highlightersdialog.h"
#include "highlightersmenu.h"
#include "issuereporter.h"
#include "ioslogdialog.h"
#include "klogg_version.h"
#include "logger.h"
#include "mainwindowtext.h"
#include "mergefileorder.h"
#include "droppathclassification.h"
#include "openfilehelper.h"
#include "pathutils.h"
#include "optionsdialog.h"
#include "predefinedfilters.h"
#include "predefinedfiltersdialog.h"
#include "progress.h"
#include "readablesize.h"
#include "recentfiles.h"
#include "recentfolders.h"
#include "sessioninfo.h"
#include "shortcuts.h"
#include "styles.h"
#include "tabgroup.h"
#include "tabbedcrawlerwidget.h"

namespace {

void signalCrawlerToFollowFile( CrawlerWidget* crawler_widget )
{
    dispatchToMainThread( [ crawler_widget ]() { crawler_widget->followSet( true ); } );
}

// Debounce window for session persistence (see scheduleSessionPersistence).
constexpr int SessionPersistenceDebounceMs = 750;

static constexpr auto ClipboardMaxTry = 5;

class ScopedMainWindowShortcutSuspender {
  public:
    explicit ScopedMainWindowShortcutSuspender( QWidget* window )
    {
        if ( !window ) {
            return;
        }

        for ( auto* action : window->findChildren<QAction*>() ) {
            const auto shortcuts = action->shortcuts();
            if ( shortcuts.isEmpty() ) {
                continue;
            }
            actionShortcuts_.push_back( { action, shortcuts } );
            action->setShortcuts( {} );
        }

        for ( auto* shortcut : window->findChildren<QShortcut*>() ) {
            if ( !shortcut->isEnabled() ) {
                continue;
            }
            disabledShortcuts_.push_back( shortcut );
            shortcut->setEnabled( false );
        }
    }

    ~ScopedMainWindowShortcutSuspender()
    {
        for ( const auto& actionShortcut : actionShortcuts_ ) {
            if ( actionShortcut.action ) {
                actionShortcut.action->setShortcuts( actionShortcut.shortcuts );
            }
        }

        for ( auto* shortcut : disabledShortcuts_ ) {
            if ( shortcut ) {
                shortcut->setEnabled( true );
            }
        }
    }

  private:
    struct ActionShortcuts {
        QAction* action;
        QList<QKeySequence> shortcuts;
    };

    std::vector<ActionShortcuts> actionShortcuts_;
    std::vector<QShortcut*> disabledShortcuts_;
};

} // namespace

QTranslator MainWindow::mTranslator;
QTranslator MainWindow::mQtTranslator;

MainWindow::MainWindow( WindowSession session )
    : session_( std::move( session ) )
    , mainIcon_()
    , iconLoader_( this )
    , signalMux_()
    , quickFindMux_( session_.getQuickFindPattern() )
    , mainTabWidget_()
    , tempDir_( QDir::temp().filePath( "klogg_temp_" ) )
{
    createActions();
    createMenus();
    createToolBars();

    setAcceptDrops( true );

    // Default geometry
    const QRect geometry = QApplication::primaryScreen()->availableGeometry();
    setGeometry( geometry.x() + 20, geometry.y() + 40, geometry.width() - 140,
                 geometry.height() - 140 );

    mainIcon_.addFile( ":/images/hicolor/16x16/klogg.png" );
    // mainIcon_.addFile( ":/images/hicolor/24x24/klogg.png" );
    mainIcon_.addFile( ":/images/hicolor/32x32/klogg.png" );
    mainIcon_.addFile( ":/images/hicolor/48x48/klogg.png" );

    setWindowIcon( mainIcon_ );
    readSettings();

    createTrayIcon();

    // Connect the signals to the mux (they will be forwarded to the
    // "current" crawlerwidget

    // Send actions to the crawlerwidget
    signalMux_.connect( this, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );
    // textWrapSet / goToLine / followSet / jumpToTop / enteringQuickFind /
    // exitingQuickFind are NOT mux-routed: they are dispatched polymorphically
    // via currentDocument() (AbstractCrawlerWidget virtuals) so folder tabs
    // receive them too.
    connect( &quickFindWidget_, &QuickFindWidget::close, this, [ this ] {
        if ( auto* document = currentDocument() ) {
            document->exitingQuickFind();
        }
    } );

    // Actions from the CrawlerWidget
    signalMux_.connect( SIGNAL( followModeChanged( bool ) ), this,
                        SLOT( changeFollowMode( bool ) ) );
    signalMux_.connect(
        SIGNAL( newSelection( LineNumber, LinesCount, LineColumn, LineLength ) ), this,
        SLOT( lineNumberHandler( LineNumber, LinesCount, LineColumn, LineLength ) ) );
    signalMux_.connect( SIGNAL( searchPendingLinesChanged() ), this,
                        SLOT( refreshLineNumberField() ) );
    signalMux_.connect( SIGNAL( sendToScratchpad( QString ) ), this,
                        SLOT( sendToScratchpad( QString ) ) );

    signalMux_.connect( SIGNAL( replaceDataInScratchpad( QString ) ), this,
                        SLOT( replaceDataInScratchpad( QString ) ) );

    // Register for progress status bar
    signalMux_.connect( SIGNAL( loadingProgressed( int ) ), this,
                        SLOT( updateLoadingProgress( int ) ) );
    signalMux_.connect( SIGNAL( loadingFinished( LoadingStatus ) ), this,
                        SLOT( handleLoadingFinished( LoadingStatus ) ) );

    signalMux_.connect( SIGNAL( filteredViewChanged() ), this,
                        SLOT( handleFilteredViewChanged() ) );

    // Reconnect countdown timer (fires every second to update the info line)
    reconnectCountdownTimer_ = new QTimer( this );
    reconnectCountdownTimer_->setInterval( 1000 );
    connect( reconnectCountdownTimer_, &QTimer::timeout,
             this, &MainWindow::updateReconnectCountdown );

    // Configure the main tabbed widget
    mainTabWidget_.setDocumentMode( true );
    mainTabWidget_.setMovable( true );
    // mainTabWidget_.setTabShape( QTabWidget::Triangular );
    mainTabWidget_.setTabsClosable( true );

    scratchPad_.setWindowIcon( mainIcon_ );
    scratchPad_.setWindowTitle( tr( "klogg - scratchpad" ) );

    connect( &mainTabWidget_, &TabbedCrawlerWidget::tabCloseRequested, this,
             [ this ]( int index ) { this->closeTab( index, ActionInitiator::User ); } );
    connect( &mainTabWidget_, &TabbedCrawlerWidget::currentChanged, this,
             &MainWindow::currentTabChanged );
    connect( &mainTabWidget_, &TabbedCrawlerWidget::tabsReordered, this,
             &MainWindow::scheduleSessionPersistence );

    auto& groupManager = TabGroupManager::getSynced();
    connect( &groupManager, &TabGroupManager::groupsChanged, this,
             &MainWindow::scheduleSessionPersistence );

    // Session persistence debounce: frequent triggers (every tab switch used
    // to synchronously rewrite the session + sync QSettings -- a stall of
    // hundreds of ms when the preferences daemon is contended) now coalesce
    // into a single write once activity settles. closeEvent flushes directly.
    sessionPersistenceTimer_.setSingleShot( true );
    sessionPersistenceTimer_.setInterval( SessionPersistenceDebounceMs );
    connect( &sessionPersistenceTimer_, &QTimer::timeout, this,
             &MainWindow::persistSessionState );

    // Establish the QuickFindWidget and mux ( to send requests from the
    // QFWidget to the right window )
    connect( &quickFindWidget_, SIGNAL( patternConfirmed( const QString&, bool, bool, bool ) ),
             &quickFindMux_, SLOT( confirmPattern( const QString&, bool, bool, bool ) ) );
    connect( &quickFindWidget_, SIGNAL( patternUpdated( const QString&, bool, bool, bool ) ),
             &quickFindMux_, SLOT( setNewPattern( const QString&, bool, bool, bool ) ) );
    connect( &quickFindWidget_, SIGNAL( cancelSearch() ), &quickFindMux_, SLOT( cancelSearch() ) );
    connect( &quickFindWidget_, SIGNAL( searchForward() ), &quickFindMux_,
             SLOT( searchForward() ) );
    connect( &quickFindWidget_, SIGNAL( searchBackward() ), &quickFindMux_,
             SLOT( searchBackward() ) );
    connect( &quickFindWidget_, SIGNAL( searchNext() ), &quickFindMux_, SLOT( searchNext() ) );

    // QuickFind changes coming from the views
    connect( &quickFindMux_, SIGNAL( patternChanged( const QString&, bool, bool, bool ) ), this,
             SLOT( changeQFPattern( const QString&, bool, bool, bool ) ) );
    connect( &quickFindMux_, SIGNAL( notify( const QFNotification& ) ), &quickFindWidget_,
             SLOT( notify( const QFNotification& ) ) );
    connect( &quickFindMux_, SIGNAL( clearNotification() ), &quickFindWidget_,
             SLOT( clearNotification() ) );

    // Construct the QuickFind bar
    quickFindWidget_.hide();

    QWidget* central_widget = new QWidget();
    auto* main_layout = new QVBoxLayout();
    main_layout->setContentsMargins( 0, 0, 0, 0 );
    main_layout->addWidget( &mainTabWidget_ );
    main_layout->addWidget( &quickFindWidget_ );
    central_widget->setLayout( main_layout );

    setCentralWidget( central_widget );

    updateTitleBar( "" );
    loadIcons();
    reTranslateUI();
}

void MainWindow::reloadGeometry()
{
    QByteArray geometry;

    session_.restoreGeometry( &geometry );
    restoreGeometry( geometry );

    // Prevent restoring in minimized state (can happen if session was saved while minimized)
    if ( windowState().testFlag( Qt::WindowMinimized ) ) {
        setWindowState( windowState() & ~Qt::WindowMinimized );
    }
}

void MainWindow::reloadSession()
{
    const auto& config = Configuration::get();
    const auto followFileOnLoad = config.followFileOnLoad() && config.anyFileWatchEnabled();

    suspendSessionPersistence_ = true;

    int current_file_index = -1;
    const auto openedFiles
        = session_.restore( [] { return new CrawlerWidget(); }, &current_file_index );

    for ( const auto& open_file : openedFiles ) {
        const auto& documentInfo = open_file.first;

        if ( documentInfo.kind == DocumentKind::Folder ) {
            // Folder tab: FolderCrawlerWidget already built by Session::openFolder
            // during restore (view_factory is not used for folders). Add it to the
            // tab bar with its display name; do NOT follow-file or register adb.
            auto* folder_widget = static_cast<FolderCrawlerWidget*>( open_file.second );
            if ( folder_widget != nullptr ) {
                mainTabWidget_.addCrawler( folder_widget, documentInfo.documentId,
                                           documentInfo.displayName, documentInfo.toolTip );
            }
            continue;
        }

        auto* crawler_widget = static_cast<CrawlerWidget*>( open_file.second );

        if ( crawler_widget ) {
            mainTabWidget_.addCrawler(
                crawler_widget, documentInfo.documentId,
                documentInfo.kind == DocumentKind::File ? QString{} : documentInfo.displayName,
                documentInfo.toolTip );

            if ( documentInfo.kind == DocumentKind::AdbLogcat ) {
                registerAdbLogcatSource( crawler_widget );
            }

            if ( followFileOnLoad && documentInfo.kind == DocumentKind::File ) {
                signalCrawlerToFollowFile( crawler_widget );
            }
        }
    }

    if ( current_file_index >= 0 ) {
        mainTabWidget_.setCurrentIndex( current_file_index );

        if ( followFileOnLoad ) {
            followAction->setChecked( true );
        }
    }

    updateOpenedFilesMenu();
    suspendSessionPersistence_ = false;
    scheduleSessionPersistence();
}

void MainWindow::loadInitialFile( QString fileName, bool followFile )
{
    LOG_DEBUG << "loadInitialFile";

    // Is there a file passed as argument?
    if ( !fileName.isEmpty() ) {
        // Explicit folder routing for the CLI entry point (klogg <folder>):
        // makes the CLI intent self-documenting at its real entry point. The
        // loadFile directory guard remains as the backstop for the other
        // callers (loadFileNonInteractive/recent/favorites).
        if ( isDirectoryPath( fileName ) ) {
            openFolderByPath( fileName );
            return;
        }
        loadFile( fileName, followFile );
    }
}

void MainWindow::reTranslateUI()
{
    using namespace klogg::mainwindow;
    // menu
    auto transMenu = []( const char* text ) -> auto {
        return QApplication::translate( "klogg::mainwindow::menu", text );
    };
    fileMenu->setTitle( transMenu( menu::fileTitle ) );
    editMenu->setTitle( transMenu( menu::editTitle ) );
    viewMenu->setTitle( transMenu( menu::viewTitle ) );
    openedFilesMenu->setTitle( transMenu( menu::openedFilesTitle ) );
    toolsMenu->setTitle( transMenu( menu::toolsTitle ) );
    highlightersMenu->setTitle( transMenu( menu::highlightersTitle ) );
    favoritesMenu->setTitle( transMenu( menu::favoritesTitle ) );
    helpMenu->setTitle( transMenu( menu::helpTitle ) );

    // toolbar
    toolBar->setToolTip(
        QApplication::translate( "klogg::mainwindow::toolbar", toolbar::toolbarTitle ) );

    // action
    auto transAction = []( const char* text ) -> auto {
        return QApplication::translate( "klogg::mainwindow::action", text );
    };
    newWindowAction->setText( transAction( action::newWindowText ) );
    newWindowAction->setStatusTip( transAction( action::newWindowStatusTip ) );

    openAction->setText( transAction( action::openText ) );
    openAction->setStatusTip( transAction( action::openStatusTip ) );
    openAdbLogcatAction->setText( tr( "Open ADB Logcat..." ) );
    openAdbLogcatAction->setStatusTip( tr( "Open Android logcat as a live source" ) );
    openIosLogStreamAction->setText( tr( "Open iOS Log Stream..." ) );
    openIosLogStreamAction->setStatusTip( tr( "Open iOS device logs as a live source" ) );

    recentFilesCleanup->setText( transAction( action::recentFilesCleanupText ) );
    recentFoldersCleanup->setText( transAction( action::recentFoldersCleanupText ) );

    closeAction->setText( transAction( action::closeText ) );
    closeAction->setStatusTip( transAction( action::closeStatusTip ) );

    closeAllAction->setText( transAction( action::closeAllText ) );
    closeAllAction->setStatusTip( transAction( action::closeAllStatusTip ) );

    exitAction->setText( transAction( action::exitText ) );
    exitAction->setStatusTip( transAction( action::exitStatusTip ) );

    copyAction->setText( transAction( action::copyText ) );
    copyAction->setStatusTip( transAction( action::copyStatusTip ) );

    selectAllAction->setText( transAction( action::selectAllText ) );
    selectAllAction->setStatusTip( transAction( action::selectAllStatusTip ) );

    goToLineAction->setText( transAction( action::goToLineText ) );
    goToLineAction->setStatusTip( transAction( action::goToLineStatusTip ) );

    findAction->setText( transAction( action::findText ) );
    findAction->setStatusTip( transAction( action::findStatusTip ) );

    clearLogAction->setText( transAction( action::clearLogText ) );
    clearLogAction->setStatusTip( transAction( action::clearLogStatusTip ) );
    saveCurrentLiveLogMenu->setTitle( tr( "Save Live Log As" ) );
    saveCurrentLiveLogStripAnsiAction->setText( tr( "Without ANSI Sequences..." ) );
    saveCurrentLiveLogStripAnsiAction->setStatusTip(
        tr( "Persist the current live capture to a file after removing ANSI sequences" ) );
    saveCurrentLiveLogPreserveAnsiAction->setText( tr( "With ANSI Sequences..." ) );
    saveCurrentLiveLogPreserveAnsiAction->setStatusTip(
        tr( "Persist the current live capture to a file without modifying ANSI sequences" ) );
    disconnectSourceAction->setText( tr( "Disconnect Source" ) );
    disconnectSourceAction->setStatusTip( tr( "Stop streaming from the current live source" ) );
    reconnectSourceAction->setText( tr( "Reconnect Source" ) );
    reconnectSourceAction->setStatusTip( tr( "Reconnect the current live source" ) );

    openContainingFolderAction->setText( transAction( action::openContainingFolderText ) );
    openContainingFolderAction->setStatusTip(
        transAction( action::openContainingFolderStatusTip ) );

    openInEditorAction->setText( transAction( action::openInEditorText ) );
    openInEditorAction->setStatusTip( transAction( action::openInEditorStatusTip ) );

    copyPathToClipboardAction->setText( transAction( action::copyPathToClipboardText ) );
    copyPathToClipboardAction->setStatusTip( transAction( action::copyPathToClipboardStatusTip ) );

    openClipboardAction->setText( transAction( action::openClipboardText ) );
    openClipboardAction->setStatusTip( transAction( action::openClipboardStatusTip ) );

    openUrlAction->setText( transAction( action::openUrlText ) );
    openUrlAction->setStatusTip( transAction( action::openUrlStatusTip ) );

    overviewVisibleAction->setText( transAction( action::overviewVisibleText ) );

    lineNumbersVisibleInMainAction->setText( transAction( action::lineNumbersVisibleInMainText ) );
    lineNumbersVisibleInFilteredAction->setText(
        transAction( action::lineNumbersVisibleInFilteredText ) );

    followAction->setText( transAction( action::followText ) );
    textWrapAction->setText( transAction( action::wrapText ) );
    reloadAction->setText( transAction( action::reloadText ) );
    stopAction->setText( transAction( action::stopText ) );

    optionsAction->setText( transAction( action::optionsText ) );
    optionsAction->setStatusTip( transAction( action::optionsStatusTip ) );

    editHighlightersAction->setText( transAction( action::editHighlightersText ) );
    editHighlightersAction->setStatusTip( transAction( action::editHighlightersStatusTip ) );

    showDocumentationAction->setText( transAction( action::showDocumentationText ) );
    showDocumentationAction->setStatusTip( transAction( action::showDocumentationStatusTip ) );

    aboutAction->setText( transAction( action::aboutText ) );
    aboutAction->setStatusTip( transAction( action::aboutStatusTip ) );

    aboutQtAction->setText( transAction( action::aboutQtText ) );
    aboutQtAction->setStatusTip( transAction( action::aboutQtStatusTip ) );

    reportIssueAction->setText( transAction( action::reportIssueText ) );
    reportIssueAction->setStatusTip( transAction( action::reportIssueStatusTip ) );

    generateDumpAction->setText( transAction( action::generateDumpText ) );
    generateDumpAction->setStatusTip( transAction( action::generateDumpStatusTip ) );

    checkForNewVersionAction->setText( transAction( action::checkForNewVersionText ) );
    checkForNewVersionAction->setStatusTip(
        transAction( action::checkForNewVersionStatusTip ) );

    showScratchPadAction->setText( transAction( action::showScratchPadText ) );
    showScratchPadAction->setStatusTip( transAction( action::showScratchPadStatusTip ) );

    auto curFavoritesIconText = addToFavoritesAction->data().toBool()
                                    ? transAction( action::addToFavoritesText )
                                    : transAction( action::removeFromFavoritesText );
    addToFavoritesAction->setText( curFavoritesIconText );
    addToFavoritesMenuAction->setText( transAction( action::addToFavoritesText ) );

    removeFromFavoritesAction->setText( transAction( action::removeFromFavoritesText ) );

    selectOpenFileAction->setText( transAction( action::selectOpenFileText ) );

    predefinedFiltersDialogAction->setText( transAction( action::predefinedFiltersDialogText ) );
    predefinedFiltersDialogAction->setStatusTip(
        transAction( action::predefinedFiltersDialogStatusTip ) );
    importFilterFavoritesAction->setText( transAction( action::importFilterFavoritesText ) );
    importFilterFavoritesAction->setStatusTip(
        transAction( action::importFilterFavoritesStatusTip ) );
    exportFilterFavoritesAction->setText( transAction( action::exportFilterFavoritesText ) );
    exportFilterFavoritesAction->setStatusTip(
        transAction( action::exportFilterFavoritesStatusTip ) );

    // trayIcon
    trayIcon_->setToolTip( QApplication::translate( "klogg::mainwindow::trayicon",
                                                    klogg::mainwindow::trayicon::trayiconTip ) );
}

int MainWindow::installLanguage( QString lang )
{
    if ( lang.isEmpty() ) {
        return -1;
    }

    QApplication::removeTranslator( &mTranslator );
    QApplication::removeTranslator( &mQtTranslator );

    QString qtPath( ":/i18n/qt_" + lang + ".qm" );
    QResource qtTranslations( qtPath );
    if ( !mQtTranslator.load( qtTranslations.data(), (int)qtTranslations.size() ) ) {
        LOG_ERROR << "load fail";
        return -1;
    }
    if ( !QApplication::installTranslator( &mQtTranslator ) ) {
        LOG_ERROR << "install fail";
        return -1;
    }

    QString appPath( ":/i18n/" + lang + ".qm" );
    QResource appTranslations( appPath );
    if ( !mTranslator.load( appTranslations.data(), (int)appTranslations.size() ) ) {
        LOG_ERROR << "load fail";
        return -1;
    }
    if ( !QApplication::installTranslator( &mTranslator ) ) {
        LOG_ERROR << "install fail";
        return -1;
    }

    return 0;
}

// Menu actions
void MainWindow::createActions()
{
    const auto& config = Configuration::get();
    const auto shortcuts = config.shortcuts();

    using namespace klogg::mainwindow;

    newWindowAction = new QAction( tr( action::newWindowText ), this );
    newWindowAction->setStatusTip( tr( action::newWindowStatusTip ) );
    connect( newWindowAction, &QAction::triggered, [ = ] { Q_EMIT newWindow(); } );
    newWindowAction->setVisible( config.allowMultipleWindows() );

    openAction = new QAction( tr( action::openText ), this );
    openAction->setStatusTip( tr( action::openStatusTip ) );
    connect( openAction, &QAction::triggered, [ this ]( auto ) { this->open(); } );

    openFolderAction = new QAction( tr( "Open Folder..." ), this );
    openFolderAction->setStatusTip( tr( "Search every file in a folder (like grep -EIrn)" ) );
    connect( openFolderAction, &QAction::triggered, [ this ]( auto ) { this->openFolder(); } );

    openAdbLogcatAction = new QAction( tr( "Open ADB Logcat..." ), this );
    openAdbLogcatAction->setStatusTip( tr( "Open Android logcat as a live source" ) );
    connect( openAdbLogcatAction, &QAction::triggered, this,
             [ this ]( auto ) { this->openAdbLogcat(); } );

    openIosLogStreamAction = new QAction( tr( "Open iOS Log Stream..." ), this );
    openIosLogStreamAction->setStatusTip( tr( "Open iOS device logs as a live source" ) );
#ifndef Q_OS_MAC
    openIosLogStreamAction->setVisible( false );
#endif
    connect( openIosLogStreamAction, &QAction::triggered, this,
             [ this ]( auto ) { this->openIosLogStream(); } );

    recentFilesCleanup = new QAction( tr( action::recentFilesCleanupText ), this );
    connect( recentFilesCleanup, &QAction::triggered, this,
             [ this ]( auto ) { this->clearRecentFileActions(); } );

    recentFoldersCleanup = new QAction( tr( action::recentFoldersCleanupText ), this );
    connect( recentFoldersCleanup, &QAction::triggered, this,
             [ this ]( auto ) { this->clearRecentFolderActions(); } );

    closeAction = new QAction( tr( action::closeText ), this );
    closeAction->setObjectName( QStringLiteral( "closeAction" ) );
    closeAction->setStatusTip( tr( action::closeStatusTip ) );
    connect( closeAction, &QAction::triggered, this,
             [ this ]( auto ) { this->closeTab( ActionInitiator::User ); } );

    closeAllAction = new QAction( tr( action::closeAllText ), this );
    closeAllAction->setStatusTip( tr( action::closeAllStatusTip ) );
    connect( closeAllAction, &QAction::triggered, this,
             [ this ]( auto ) { this->closeAll( ActionInitiator::User ); } );

    recentFilesGroup = new QActionGroup( this );
    connect( recentFilesGroup, &QActionGroup::triggered, this, &MainWindow::openFileFromRecent );
    for ( auto i = 0u; i < recentFileActions.size(); ++i ) {
        recentFileActions[ i ] = new QAction( this );
        connect( recentFileActions[ i ], &QAction::hovered, [ this, a = recentFileActions[ i ] ]() {
            QToolTip::showText( QCursor::pos(), a->toolTip(), this );
        } );
        recentFileActions[ i ]->setVisible( false );
        recentFileActions[ i ]->setActionGroup( recentFilesGroup );
    }

    recentFoldersGroup = new QActionGroup( this );
    connect( recentFoldersGroup, &QActionGroup::triggered, this, &MainWindow::openFolderFromRecent );
    for ( auto i = 0u; i < recentFolderActions.size(); ++i ) {
        recentFolderActions[ i ] = new QAction( this );
        connect( recentFolderActions[ i ], &QAction::hovered,
                 [ this, a = recentFolderActions[ i ] ]() {
                     QToolTip::showText( QCursor::pos(), a->toolTip(), this );
                 } );
        recentFolderActions[ i ]->setVisible( false );
        recentFolderActions[ i ]->setActionGroup( recentFoldersGroup );
    }

    exitAction = new QAction( tr( action::exitText ), this );
    exitAction->setStatusTip( tr( action::exitStatusTip ) );
    connect( exitAction, &QAction::triggered, this, &MainWindow::exitRequested );

    copyAction = new QAction( tr( action::copyText ), this );
    copyAction->setStatusTip( tr( action::copyStatusTip ) );
    connect( copyAction, &QAction::triggered, this, [ this ]( auto ) { this->copy(); } );

    selectAllAction = new QAction( tr( action::selectAllText ), this );
    selectAllAction->setStatusTip( tr( action::selectAllStatusTip ) );
    connect( selectAllAction, &QAction::triggered, this, [ this ]( auto ) { this->selectAll(); } );

    goToLineAction = new QAction( tr( action::goToLineText ), this );
    goToLineAction->setObjectName( QStringLiteral( "goToLineAction" ) );
    goToLineAction->setStatusTip( tr( action::goToLineStatusTip ) );
    // Dispatched polymorphically so folder tabs jump in their main view too
    // (single-file: CrawlerWidget::goToLine override).
    connect( goToLineAction, &QAction::triggered, this, [ this ] {
        if ( auto* document = currentDocument() ) {
            document->goToLine();
        }
    } );

    findAction = new QAction( tr( action::findText ), this );
    findAction->setStatusTip( tr( action::findStatusTip ) );
    connect( findAction, &QAction::triggered, this, [ this ]( auto ) { this->find(); } );

    clearLogAction = new QAction( tr( action::clearLogText ), this );
    clearLogAction->setStatusTip( tr( action::clearLogStatusTip ) );
    connect( clearLogAction, &QAction::triggered, this, [ this ]( auto ) { this->clearLog(); } );

    saveCurrentLiveLogMenu = new QMenu( tr( "Save Live Log As" ), this );
    saveCurrentLiveLogMenu->setObjectName( QStringLiteral( "saveCurrentLiveLogMenu" ) );
    saveCurrentLiveLogStripAnsiAction = new QAction( tr( "Without ANSI Sequences..." ), this );
    saveCurrentLiveLogStripAnsiAction->setObjectName(
        QStringLiteral( "saveCurrentLiveLogStripAnsiAction" ) );
    saveCurrentLiveLogStripAnsiAction->setStatusTip(
        tr( "Persist the current live capture to a file after removing ANSI sequences" ) );
    connect( saveCurrentLiveLogStripAnsiAction, &QAction::triggered, this,
             [ this ]( auto ) { this->saveCurrentLiveLog( LiveLogSaveAnsiMode::Strip ); } );

    saveCurrentLiveLogPreserveAnsiAction = new QAction( tr( "With ANSI Sequences..." ), this );
    saveCurrentLiveLogPreserveAnsiAction->setObjectName(
        QStringLiteral( "saveCurrentLiveLogPreserveAnsiAction" ) );
    saveCurrentLiveLogPreserveAnsiAction->setStatusTip(
        tr( "Persist the current live capture to a file without modifying ANSI sequences" ) );
    connect( saveCurrentLiveLogPreserveAnsiAction, &QAction::triggered, this,
             [ this ]( auto ) { this->saveCurrentLiveLog( LiveLogSaveAnsiMode::Preserve ); } );
    saveCurrentLiveLogMenu->addAction( saveCurrentLiveLogStripAnsiAction );
    saveCurrentLiveLogMenu->addAction( saveCurrentLiveLogPreserveAnsiAction );
    saveCurrentLiveLogMenu->setEnabled( false );

    disconnectSourceAction = new QAction( tr( "Disconnect Source" ), this );
    disconnectSourceAction->setStatusTip(
        tr( "Stop streaming from the current live source" ) );
    connect( disconnectSourceAction, &QAction::triggered, this,
             [ this ]( auto ) { this->disconnectCurrentSource(); } );

    reconnectSourceAction = new QAction( tr( "Reconnect Source" ), this );
    reconnectSourceAction->setStatusTip( tr( "Reconnect the current live source" ) );
    connect( reconnectSourceAction, &QAction::triggered, this,
             [ this ]( auto ) { this->reconnectCurrentSource(); } );

    openContainingFolderAction = new QAction( tr( action::openContainingFolderText ), this );
    openContainingFolderAction->setStatusTip( tr( action::openContainingFolderStatusTip ) );
    connect( openContainingFolderAction, &QAction::triggered, this,
             [ this ]( auto ) { this->openContainingFolder(); } );

    openInEditorAction = new QAction( tr( action::openInEditorText ), this );
    openInEditorAction->setStatusTip( tr( action::openInEditorStatusTip ) );
    connect( openInEditorAction, &QAction::triggered, this,
             [ this ]( auto ) { this->openInEditor(); } );

    copyPathToClipboardAction = new QAction( tr( action::copyPathToClipboardText ), this );
    copyPathToClipboardAction->setObjectName( QStringLiteral( "copyPathToClipboardAction" ) );
    copyPathToClipboardAction->setStatusTip( tr( action::copyPathToClipboardStatusTip ) );
    connect( copyPathToClipboardAction, &QAction::triggered, this,
             [ this ]( auto ) { this->copyFullPath(); } );

    openClipboardAction = new QAction( tr( action::openClipboardText ), this );
    openClipboardAction->setStatusTip( tr( action::openClipboardStatusTip ) );
    connect( openClipboardAction, &QAction::triggered, this,
             [ this ]( auto ) { this->openClipboard(); } );

    openUrlAction = new QAction( tr( action::openUrlText ), this );
    openUrlAction->setStatusTip( tr( action::openUrlStatusTip ) );
    connect( openUrlAction, &QAction::triggered, this, [ this ]( auto ) { this->openUrl(); } );

    overviewVisibleAction = new QAction( tr( action::overviewVisibleText ), this );
    overviewVisibleAction->setCheckable( true );
    overviewVisibleAction->setChecked( config.isOverviewVisible() );
    connect( overviewVisibleAction, &QAction::toggled, this,
             &MainWindow::toggleOverviewVisibility );

    lineNumbersVisibleInMainAction
        = new QAction( tr( action::lineNumbersVisibleInMainText ), this );
    lineNumbersVisibleInMainAction->setCheckable( true );
    lineNumbersVisibleInMainAction->setChecked( config.mainLineNumbersVisible() );
    connect( lineNumbersVisibleInMainAction, &QAction::toggled, this,
             &MainWindow::toggleMainLineNumbersVisibility );

    lineNumbersVisibleInFilteredAction
        = new QAction( tr( action::lineNumbersVisibleInFilteredText ), this );
    lineNumbersVisibleInFilteredAction->setCheckable( true );
    lineNumbersVisibleInFilteredAction->setChecked( config.filteredLineNumbersVisible() );
    connect( lineNumbersVisibleInFilteredAction, &QAction::toggled, this,
             &MainWindow::toggleFilteredLineNumbersVisibility );

    followAction = new QAction( tr( action::followText ), this );
    followAction->setObjectName( QStringLiteral( "followAction" ) );
    followAction->setCheckable( true );
    followAction->setEnabled( config.anyFileWatchEnabled() );
    // Dispatched polymorphically so folder tabs follow the file shown in their
    // main view too (single-file: the CrawlerWidget::followSet signal override
    // fans out to both views, exactly as the old mux relay did).
    connect( followAction, &QAction::toggled, this, [ this ]( bool checked ) {
        if ( auto* document = currentDocument() ) {
            document->followSet( checked );
        }
    } );

    goToTopAction = new QAction( tr( action::goToTopText ), this );
    goToTopAction->setObjectName( QStringLiteral( "goToTopAction" ) );
    // Dispatched polymorphically (the goToLineAction precedent) so folder tabs
    // top their main view (single-file: CrawlerWidget::jumpToTop tops both
    // views).
    connect( goToTopAction, &QAction::triggered, this, [ this ] {
        if ( auto* document = currentDocument() ) {
            document->jumpToTop();
        }
    } );

    textWrapAction = new QAction( tr( action::wrapText ), this );
    textWrapAction->setObjectName( QStringLiteral( "textWrapAction" ) );
    textWrapAction->setCheckable( true );
    textWrapAction->setEnabled( true );
    // Dispatched polymorphically so folder tabs wrap their views too
    // (single-file: CrawlerWidget::textWrapSet override emits its signal).
    connect( textWrapAction, &QAction::toggled, this, [ this ]( bool checked ) {
        if ( auto* document = currentDocument() ) {
            document->textWrapSet( checked );
        }
    } );

    reloadAction = new QAction( tr( action::reloadText ), this );
    signalMux_.connect( reloadAction, SIGNAL( triggered() ), SLOT( reload() ) );

    stopAction = new QAction( tr( action::stopText ), this );
    stopAction->setEnabled( true );
    // Single-file tabs stop the file load; a folder tab's long-running
    // operation is the grep search, stopped through the polymorphic dispatch.
    connect( stopAction, &QAction::triggered, this, [ this ] {
        if ( auto* crawler = currentCrawlerWidget() ) {
            crawler->stopLoading();
        }
        else if ( auto* document = currentDocument() ) {
            document->stopSearch();
        }
    } );

    optionsAction = new QAction( tr( action::optionsText ), this );
    optionsAction->setMenuRole( QAction::PreferencesRole );
    optionsAction->setStatusTip( tr( action::optionsStatusTip ) );
    connect( optionsAction, &QAction::triggered, this, [ this ]( auto ) { this->options(); } );

    editHighlightersAction = new QAction( tr( action::editHighlightersText ), this );
    editHighlightersAction->setMenuRole( QAction::NoRole );
    editHighlightersAction->setStatusTip( tr( action::editHighlightersStatusTip ) );
    connect( editHighlightersAction, &QAction::triggered, this,
             [ this ]( auto ) { this->editHighlighters(); } );

    showDocumentationAction = new QAction( tr( action::showDocumentationText ), this );
    showDocumentationAction->setStatusTip( tr( action::showDocumentationStatusTip ) );
    connect( showDocumentationAction, &QAction::triggered, this,
             [ this ]( auto ) { this->documentation(); } );

    aboutAction = new QAction( tr( action::aboutText ), this );
    aboutAction->setStatusTip( tr( action::aboutStatusTip ) );
    connect( aboutAction, &QAction::triggered, this, [ this ]( auto ) { this->about(); } );

    aboutQtAction = new QAction( tr( action::aboutQtText ), this );
    aboutQtAction->setStatusTip( tr( action::aboutQtStatusTip ) );
    connect( aboutQtAction, &QAction::triggered, this, [ this ]( auto ) { this->aboutQt(); } );

    reportIssueAction = new QAction( tr( action::reportIssueText ), this );
    reportIssueAction->setStatusTip( tr( action::reportIssueStatusTip ) );
    connect( reportIssueAction, &QAction::triggered, this,
             []( auto ) { IssueReporter::reportIssue( IssueTemplate::Bug ); } );

    generateDumpAction = new QAction( tr( action::generateDumpText ), this );
    generateDumpAction->setStatusTip( tr( action::generateDumpStatusTip ) );
    connect( generateDumpAction, &QAction::triggered, this,
             [ this ]( auto ) { this->generateDump(); } );

    checkForNewVersionAction = new QAction( tr( action::checkForNewVersionText ), this );
    checkForNewVersionAction->setStatusTip( tr( action::checkForNewVersionStatusTip ) );
    connect( checkForNewVersionAction, &QAction::triggered, this,
             [ this ]( auto ) { this->checkForNewVersion(); } );

    showScratchPadAction = new QAction( tr( action::showScratchPadText ), this );
    showScratchPadAction->setStatusTip( tr( action::showScratchPadStatusTip ) );
    connect( showScratchPadAction, &QAction::triggered, this,
             [ this ]( auto ) { this->showScratchPad(); } );

    mergeTabsAction = new QAction( tr( "Merge Tabs..." ), this );
    mergeTabsAction->setStatusTip( tr( "Merge selected tabs into a single view" ) );
    connect( mergeTabsAction, &QAction::triggered, this,
             [ this ]( auto ) { this->mergeTabs(); } );

    encodingGroup = new QActionGroup( this );
    connect( encodingGroup, &QActionGroup::triggered, this, &MainWindow::encodingChanged );

    favoritesGroup = new QActionGroup( this );
    connect( favoritesGroup, &QActionGroup::triggered, this, &MainWindow::openFileFromFavorites );

    openedFilesGroup = new QActionGroup( this );
    connect( openedFilesGroup, &QActionGroup::triggered, this, &MainWindow::switchToOpenedFile );

    addToFavoritesAction = new QAction( tr( action::addToFavoritesText ), this );
    addToFavoritesAction->setData( true );
    connect( addToFavoritesAction, &QAction::triggered, this,
             [ this ]( auto ) { this->addToFavorites(); } );

    addToFavoritesMenuAction = new QAction( tr( action::addToFavoritesText ), this );
    connect( addToFavoritesMenuAction, &QAction::triggered, this,
             [ this ]( auto ) { this->addToFavorites(); } );

    removeFromFavoritesAction = new QAction( tr( action::removeFromFavoritesText ), this );
    connect( removeFromFavoritesAction, &QAction::triggered, this,
             [ this ]( auto ) { this->removeFromFavorites(); } );

    selectOpenFileAction = new QAction( tr( action::selectOpenFileText ), this );
    connect( selectOpenFileAction, &QAction::triggered, this,
             [ this ]( auto ) { this->selectOpenedFile(); } );

    predefinedFiltersDialogAction = new QAction( tr( action::predefinedFiltersDialogText ), this );
    predefinedFiltersDialogAction->setStatusTip( tr( action::predefinedFiltersDialogStatusTip ) );
    connect( predefinedFiltersDialogAction, &QAction::triggered, this,
             [ this ]( auto ) { this->editPredefinedFilters(); } );

    importFilterFavoritesAction = new QAction( tr( action::importFilterFavoritesText ), this );
    importFilterFavoritesAction->setStatusTip( tr( action::importFilterFavoritesStatusTip ) );
    connect( importFilterFavoritesAction, &QAction::triggered, this,
             [ this ]( auto ) { this->importFilterFavorites(); } );

    exportFilterFavoritesAction = new QAction( tr( action::exportFilterFavoritesText ), this );
    exportFilterFavoritesAction->setStatusTip( tr( action::exportFilterFavoritesStatusTip ) );
    connect( exportFilterFavoritesAction, &QAction::triggered, this,
             [ this ]( auto ) { this->exportFilterFavorites(); } );

    updateShortcuts();
}

void MainWindow::updateShortcuts()
{
    const auto& config = Configuration::get();
    const auto shortcuts = config.shortcuts();

    for ( auto& shortcut : shortcuts_ ) {
        shortcut.second->deleteLater();
    }

    shortcuts_.clear();
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowOpenQfForward,
                                      [ this ] { displayQuickFindBar( QuickFindMux::Forward ); } );
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowOpenQfBackward,
                                      [ this ] { displayQuickFindBar( QuickFindMux::Backward ); } );
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowFocusSearchInput, [ this ] {
                                          if ( auto* document = currentDocument() ) {
                                              document->focusSearchEdit();
                                          }
                                      } );
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowFullScreen,
                                      [ this ] { this->showFullScreen(); } );
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowMax,
                                      [ this ] { this->showMaximized(); } );
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowMin,
                                      [ this ] { this->showMinimized(); } );
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowNextTab,
                                      [ this ] { mainTabWidget_.selectNextTab(); } );
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowPreviousTab,
                                      [ this ] { mainTabWidget_.selectPreviousTab(); } );

    auto setShortcuts = [ &shortcuts ]( auto* action, const auto& actionName ) {
        action->setShortcuts( ShortcutAction::shortcutKeys( actionName, shortcuts ) );
    };

    setShortcuts( newWindowAction, ShortcutAction::MainWindowNewWindow );
    setShortcuts( openAction, ShortcutAction::MainWindowOpenFile );
    setShortcuts( closeAction, ShortcutAction::MainWindowCloseFile );
    setShortcuts( closeAllAction, ShortcutAction::MainWindowCloseAll );
    setShortcuts( exitAction, ShortcutAction::MainWindowQuit );
    setShortcuts( copyAction, ShortcutAction::MainWindowCopy );
    setShortcuts( selectAllAction, ShortcutAction::MainWindowSelectAll );
    setShortcuts( findAction, ShortcutAction::MainWindowOpenQf );
    setShortcuts( clearLogAction, ShortcutAction::MainWindowClearFile );
    setShortcuts( openContainingFolderAction, ShortcutAction::MainWindowOpenContainingFolder );
    setShortcuts( openInEditorAction, ShortcutAction::MainWindowOpenInEditor );
    setShortcuts( copyPathToClipboardAction, ShortcutAction::MainWindowCopyPathToClipboard );
    setShortcuts( openClipboardAction, ShortcutAction::MainWindowOpenFromClipboard );
    setShortcuts( openUrlAction, ShortcutAction::MainWindowOpenFromUrl );
    setShortcuts( followAction, ShortcutAction::MainWindowFollowFile );
    setShortcuts( goToTopAction, ShortcutAction::MainWindowGoToTop );
    setShortcuts( textWrapAction, ShortcutAction::MainWindowTextWrap );
    setShortcuts( reloadAction, ShortcutAction::MainWindowReload );
    setShortcuts( stopAction, ShortcutAction::MainWindowStop );
    setShortcuts( showScratchPadAction, ShortcutAction::MainWindowScratchpad );
    setShortcuts( selectOpenFileAction, ShortcutAction::MainWindowSelectOpenFile );
    setShortcuts( goToLineAction, ShortcutAction::LogViewJumpToLine );
    setShortcuts( optionsAction, ShortcutAction::MainWindowPreference );
    setShortcuts( disconnectSourceAction, ShortcutAction::MainWindowDisconnectSource );
    setShortcuts( reconnectSourceAction, ShortcutAction::MainWindowReconnectSource );
}

void MainWindow::loadIcons()
{
    openAction->setIcon( iconLoader_.load( "icons8-open-file" ) );
    stopAction->setIcon( iconLoader_.load( "icons8-delete" ) );
    reloadAction->setIcon( iconLoader_.load( "icons8-restore-page" ) );
    followAction->setIcon( iconLoader_.load( "icons8-fast-forward" ) );
    goToTopAction->setIcon( iconLoader_.load( "icons8-up" ) );
    textWrapAction->setIcon( iconLoader_.load( "text-wrap" ) );
    showScratchPadAction->setIcon( iconLoader_.load( "icons8-create" ) );
    addToFavoritesAction->setIcon( iconLoader_.load( "icons8-star" ) );
    addToFavoritesMenuAction->setIcon( iconLoader_.load( "icons8-star" ) );

}

void MainWindow::createMenus()
{
    using namespace klogg::mainwindow;

    fileMenu = menuBar()->addMenu( tr( menu::fileTitle ) );
    fileMenu->setToolTipsVisible( true );
    fileMenu->addAction( newWindowAction );
    fileMenu->addAction( openAction );
    fileMenu->addAction( openFolderAction );
    fileMenu->addAction( openAdbLogcatAction );
    fileMenu->addAction( openIosLogStreamAction );
    fileMenu->addAction( openClipboardAction );
    fileMenu->addAction( openUrlAction );
    recentFilesMenu = fileMenu->addMenu( tr( "Open Recent" ) );
    for ( auto i = 0u; i < recentFileActions.size(); ++i ) {
        recentFilesMenu->addAction( recentFileActions[ i ] );
    }
    recentFilesMenu->addSeparator();
    recentFilesMenu->addAction( recentFilesCleanup );
    recentFilesMenu->setEnabled( false );

    recentFoldersMenu = fileMenu->addMenu( tr( "Open Recent Fol&der" ) );
    for ( auto i = 0u; i < recentFolderActions.size(); ++i ) {
        recentFoldersMenu->addAction( recentFolderActions[ i ] );
    }
    recentFoldersMenu->addSeparator();
    recentFoldersMenu->addAction( recentFoldersCleanup );
    recentFoldersMenu->setEnabled( false );

    fileMenu->addSeparator();

    fileMenu->addAction( closeAction );
    fileMenu->addAction( closeAllAction );
    fileMenu->addSeparator();

    fileMenu->addAction( optionsAction );
    fileMenu->addSeparator();
    fileMenu->addAction( exitAction );

    editMenu = menuBar()->addMenu( tr( menu::editTitle ) );
    editMenu->addAction( copyAction );
    editMenu->addAction( selectAllAction );
    editMenu->addSeparator();
    editMenu->addAction( findAction );
    editMenu->addSeparator();
    editMenu->addAction( goToLineAction );
    editMenu->addSeparator();
    editMenu->addAction( copyPathToClipboardAction );
    editMenu->addAction( openContainingFolderAction );
    editMenu->addSeparator();
    editMenu->addAction( openInEditorAction );
    editMenu->addMenu( saveCurrentLiveLogMenu );
    editMenu->addAction( clearLogAction );
    editMenu->addAction( disconnectSourceAction );
    editMenu->addAction( reconnectSourceAction );
    editMenu->setEnabled( false );

    viewMenu = menuBar()->addMenu( tr( menu::viewTitle ) );
    openedFilesMenu = viewMenu->addMenu( tr( menu::openedFilesTitle ) );
    viewMenu->addSeparator();
    viewMenu->addAction( overviewVisibleAction );
    viewMenu->addSeparator();
    viewMenu->addAction( lineNumbersVisibleInMainAction );
    viewMenu->addAction( lineNumbersVisibleInFilteredAction );
    viewMenu->addSeparator();
    viewMenu->addAction( textWrapAction );
    viewMenu->addSeparator();
    viewMenu->addAction( followAction );
    viewMenu->addAction( goToTopAction );
    viewMenu->addSeparator();
    viewMenu->addAction( reloadAction );

    toolsMenu = menuBar()->addMenu( tr( menu::toolsTitle ) );

    highlightersMenu = new HighlightersMenu( tr( menu::highlightersTitle ), menuBar() );
    menuBar()->addMenu( highlightersMenu );
    highlightersMenu->setApplyChange( [ this ]() {
        auto crawler = currentCrawlerWidget();
        if ( crawler != nullptr ) {
            crawler->applyConfiguration();
        }
    } );

    toolsMenu->addAction( predefinedFiltersDialogAction );
    toolsMenu->addAction( importFilterFavoritesAction );
    toolsMenu->addAction( exportFilterFavoritesAction );

    toolsMenu->addSeparator();
    toolsMenu->addAction( showScratchPadAction );
    toolsMenu->addSeparator();
    toolsMenu->addAction( mergeTabsAction );

    menuBar()->addMenu( EncodingMenu::generate( encodingGroup, menuBar() ) );
    menuBar()->addSeparator();

    favoritesMenu = menuBar()->addMenu( tr( menu::favoritesTitle ) );
    favoritesMenu->setToolTipsVisible( true );

    helpMenu = menuBar()->addMenu( tr( menu::helpTitle ) );
    helpMenu->addAction( showDocumentationAction );
    helpMenu->addSeparator();
    helpMenu->addAction( checkForNewVersionAction );
    helpMenu->addSeparator();
    helpMenu->addAction( reportIssueAction );
    helpMenu->addSeparator();
    helpMenu->addAction( generateDumpAction );
    helpMenu->addSeparator();
    helpMenu->addAction( aboutQtAction );
    helpMenu->addAction( aboutAction );
}

void MainWindow::createToolBars()
{
    infoLine = new PathLine();
    infoLine->setFrameStyle( QFrame::StyledPanel );
    infoLine->setFrameShadow( QFrame::Sunken );
    infoLine->setLineWidth( 0 );
    infoLine->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Minimum );

    sizeField = new QLabel();
    sizeField->setAlignment( Qt::AlignHCenter | Qt::AlignVCenter );

    dateField = new QLabel();
    dateField->setAlignment( Qt::AlignHCenter | Qt::AlignVCenter );

    encodingField = new QLabel();
    dateField->setAlignment( Qt::AlignHCenter | Qt::AlignVCenter );

    lineNbField = new QLabel();
    lineNbField->setAlignment( Qt::AlignRight | Qt::AlignVCenter );
    lineNbField->setContentsMargins( 2, 0, 2, 0 );

    toolBar = addToolBar( QApplication::translate( "klogg::mainwindow::toolbar",
                                                   klogg::mainwindow::toolbar::toolbarTitle ) );
    toolBar->setIconSize( QSize( 16, 16 ) );
    toolBar->setMovable( false );
    toolBar->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Minimum );
    toolBar->addAction( openAction );
    toolBar->addAction( reloadAction );
    toolBar->addAction( followAction );
    toolBar->addAction( goToTopAction );
    toolBar->addAction( textWrapAction );
    toolBar->addAction( addToFavoritesAction );
    toolBar->addWidget( infoLine );
    toolBar->addAction( stopAction );

    infoToolbarSeparators.reserve( 5 );
    infoToolbarSeparators.push_back( toolBar->addSeparator() );
    toolBar->addWidget( sizeField );
    infoToolbarSeparators.push_back( toolBar->addSeparator() );
    toolBar->addWidget( dateField );
    infoToolbarSeparators.push_back( toolBar->addSeparator() );
    toolBar->addWidget( encodingField );
    infoToolbarSeparators.push_back( toolBar->addSeparator() );
    toolBar->addWidget( lineNbField );
    infoToolbarSeparators.push_back( toolBar->addSeparator() );
    toolBar->addAction( showScratchPadAction );

    showInfoLabels( false );
}

void MainWindow::createTrayIcon()
{
    trayIcon_ = new QSystemTrayIcon( this );

    QMenu* trayMenu = new QMenu( this );
    QAction* openWindowAction = new QAction( tr( "Open window" ), this );
    QAction* quitAction = new QAction( tr( "Quit" ), this );

    trayMenu->addAction( openWindowAction );
    trayMenu->addAction( quitAction );

    connect( openWindowAction, &QAction::triggered, this, &QMainWindow::show );
    connect( quitAction, &QAction::triggered, [ this ] {
        this->isCloseFromTray_ = true;
        this->close();
    } );

    trayIcon_->setIcon( mainIcon_ );
    trayIcon_->setToolTip( tr( klogg::mainwindow::trayicon::trayiconTip ) );
    trayIcon_->setContextMenu( trayMenu );

    connect( trayIcon_, &QSystemTrayIcon::activated,
             [ this ]( QSystemTrayIcon::ActivationReason reason ) {
                 switch ( reason ) {
                 case QSystemTrayIcon::Trigger:
                     if ( !this->isVisible() ) {
                         this->show();
                     }
                     else {
                         this->hide();
                     }
                     break;
                 default:
                     break;
                 }
             } );

    if ( Configuration::get().minimizeToTray() ) {
        trayIcon_->show();
    }
}
//
// Q_SLOTS:
//

// Opens the file selection dialog to select a new log file
void MainWindow::open()
{
    QString defaultDir = ".";

    // Default to the path of the current file if there is one
    if ( auto current = currentCrawlerWidget() ) {
        QString current_file = session_.getAssociatedPath( current );
        QFileInfo fileInfo = QFileInfo( current_file );
        if ( fileInfo.exists() ) {
            defaultDir = fileInfo.path();
        }
    }

    const auto selectedFiles = QFileDialog::getOpenFileUrls(
        this, tr( "Open file" ), QUrl::fromLocalFile( defaultDir ), tr( "All files (*)" ) );

    std::vector<QUrl> localFiles;
    std::vector<QUrl> remoteFiles;

    std::partition_copy( selectedFiles.cbegin(), selectedFiles.cend(),
                         std::back_inserter( localFiles ), std::back_inserter( remoteFiles ),
                         []( const QUrl& url ) { return url.isLocalFile(); } );

    for ( const auto& localFile : localFiles ) {
        loadFile( localFile.toLocalFile() );
    }

    for ( const auto& remoteFile : remoteFiles ) {
        openRemoteFile( remoteFile );
    }
}

void MainWindow::openFolder()
{
    QString defaultDir = ".";
    if ( auto current = currentCrawlerWidget() ) {
        const QString currentFile = session_.getAssociatedPath( current );
        const QFileInfo fileInfo( currentFile );
        if ( fileInfo.exists() ) {
            defaultDir = fileInfo.path();
        }
    }

    const QString folder
        = QFileDialog::getExistingDirectory( this, tr( "Open folder" ), defaultDir );
    if ( folder.isEmpty() ) {
        return;
    }

    openFolderByPath( folder );
}

void MainWindow::openFolderByPath( const QString& folderPath )
{
    const auto filePaths = enumerateFolderFiles( folderPath );
    if ( filePaths.empty() ) {
        QMessageBox::information( this, tr( "Open folder" ),
                                  tr( "No readable files found in %1" ).arg( folderPath ) );
        return;
    }

    auto* view = session_.openFolder(
        folderPath, std::vector<QString>( filePaths.begin(), filePaths.end() ) );
    if ( view == nullptr ) {
        return;
    }

    // Robust to trailing slashes (drag-dropped folders carry one).
    const QString displayName = klogg::displayNameForPath( folderPath );
    const auto index = mainTabWidget_.addCrawler( static_cast<FolderCrawlerWidget*>( view ),
                                                  folderPath, displayName, folderPath );
    mainTabWidget_.setCurrentIndex( index );
    updateOpenedFilesMenu();
    // Record the folder only on success (non-empty + tab added).
    addRecentFolder( folderPath );
}

void MainWindow::openAdbLogcat()
{
    AdbLogcatDialog dialog( this );
    if ( dialog.exec() == QDialog::Accepted ) {
        openAdbLogcatSource( dialog.sessionData(), true );
    }
}

void MainWindow::openIosLogStream()
{
#ifdef Q_OS_MAC
    IosLogDialog dialog( this );
    if ( dialog.exec() == QDialog::Accepted ) {
        openAdbLogcatSource( dialog.sessionData(), true );
    }
#else
    QMessageBox::information( this, tr( "Open iOS Log Stream" ),
                              tr( "iOS log streaming is supported only on macOS." ) );
#endif
}

void MainWindow::openRemoteFile( const QUrl& url )
{
    Downloader downloader;

    QProgressDialog progressDialog;
    progressDialog.setLabelText( tr( "Downloading %1" ).arg( url.toString() ) );

    connect( &downloader, &Downloader::downloadProgress,
             [ &progressDialog ]( qint64 bytesReceived, qint64 bytesTotal ) {
                 const auto progress = calculateProgress( bytesReceived, bytesTotal );
                 progressDialog.setRange( 0, 100 );
                 progressDialog.setValue( progress );
             } );

    connect( &downloader, &Downloader::finished,
             [ &progressDialog ]( bool isOk ) { progressDialog.done( isOk ? 0 : 1 ); } );

    auto tempFile = new QTemporaryFile( tempDir_.filePath( url.fileName() ), this );
    if ( tempFile->open() ) {
        downloader.download( url, tempFile );
        const auto downloadResult = progressDialog.exec();

        // Drain stale Cocoa events while the dialog's NSWindow is still alive.
        // On macOS, a parentless QProgressDialog gets its own NSWindow.  After
        // exec() returns, orphaned Cocoa events may remain in the queue.  If the
        // dialog is destroyed before those events are drained, the main event
        // loop will try to dispatch them to the dead window → SIGABRT.
        QCoreApplication::processEvents();

        if ( !downloadResult ) {
            loadFile( tempFile->fileName() );
        }
        else {
            QMessageBox::critical( this, tr( "Klogg - File download" ), downloader.lastError() );
        }
    }
    else {
        QMessageBox::critical( this, tr( "Klogg - File download" ),
                               tr( "Failed to create temp file" ) );
    }
}

void MainWindow::switchToOpenedFile( QAction* action )
{
    if ( !action ) {
        return;
    }

    const auto documentId = action->data().toString();
    for ( int index = 0; index < mainTabWidget_.count(); ++index ) {
        const auto* crawler = qobject_cast<CrawlerWidget*>( mainTabWidget_.widget( index ) );
        if ( crawler && session_.getDocumentId( crawler ) == documentId ) {
            mainTabWidget_.setCurrentIndex( index );
            activateWindow();
            return;
        }
    }
}

void MainWindow::openFileFromRecent( QAction* action )
{
    if ( !action ) {
        return;
    }

    const auto filename = action->data().toString();
    if ( QFileInfo{ filename }.isReadable() ) {
        loadFile( filename );
    }
    else {
        const auto userAction = QMessageBox::question(
            this, tr( "klogg - remove from recent" ),
            tr( "Could not read file %1. Remove it from recent files?" ).arg( filename ),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

        if ( userAction == QMessageBox::Yes ) {
            removeFromRecent( filename );
        }
    }
}

void MainWindow::openFolderFromRecent( QAction* action )
{
    if ( !action ) {
        return;
    }

    const auto folderPath = action->data().toString();
    if ( QFileInfo{ folderPath }.isDir() ) {
        openFolderByPath( folderPath );
    }
    else {
        const auto userAction = QMessageBox::question(
            this, tr( "klogg - remove from recent folders" ),
            tr( "Folder %1 does not exist. Remove it from recent folders?" ).arg( folderPath ),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

        if ( userAction == QMessageBox::Yes ) {
            auto& recentFolders = RecentFolders::get();
            recentFolders.removeRecent( folderPath );
            recentFolders.save();
            updateRecentFolderActions();
        }
    }
}

void MainWindow::openFileFromFavorites( QAction* action )
{
    if ( !action ) {
        return;
    }

    const auto filename = action->data().toString();
    if ( QFileInfo{ filename }.isReadable() ) {
        loadFile( filename );
    }
    else {
        const auto userAction = QMessageBox::question(
            this, tr( "klogg - remove from favorites" ),
            tr( "Could not read file %1. Remove it from favorites?" ).arg( filename ),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

        if ( userAction == QMessageBox::Yes ) {
            removeFromFavorites( filename );
        }
    }
}

// Close current tab
void MainWindow::closeTab( ActionInitiator initiator )
{
    int currentIndex = mainTabWidget_.currentIndex();

    if ( currentIndex >= 0 ) {
        closeTab( currentIndex, initiator );
    }
    else {
        this->close();
    }
}

// Close all tabs
void MainWindow::closeAll( ActionInitiator initiator )
{
    while ( mainTabWidget_.count() ) {
        closeTab( 0, initiator );
    }
}

// Select all the text in the currently selected view
void MainWindow::selectAll()
{
    if ( infoLine->hasFocus() ) {
        infoLine->setSelection( 0, klogg::isize( infoLine->text() ) );
    }
    else if ( auto current = currentDocument(); current != nullptr ) {
        current->selectAll();
    }
}

// Copy the currently selected line into the clipboard
void MainWindow::copy()
{
    try {
        if ( infoLine->hasFocus() && infoLine->hasSelectedText() ) {
            sendTextToClipboard( infoLine->selectedText() );
            return;
        }

        if ( auto current = currentDocument(); current != nullptr ) {
            auto text = current->getSelectedText();
            text.replace( QChar::Null, QChar::Space );

            sendTextToClipboard( text, true );
        }
    } catch ( std::exception& err ) {
        LOG_ERROR << "failed to copy data to clipboard " << err.what();
    }
}

// Display the QuickFind bar
void MainWindow::find()
{
    displayQuickFindBar( QuickFindMux::Forward );
}

void MainWindow::clearLog()
{
    auto* crawler = currentCrawlerWidget();
    if ( !crawler ) {
        return;
    }

    if ( session_.getDocumentKind( crawler ) == DocumentKind::AdbLogcat ) {
        auto* adbSource = session_.getAdbLogcatSource( crawler );
        if ( !adbSource ) {
            return;
        }

        const auto displayName = session_.getDisplayName( crawler );
        const auto isIosLogStream
            = adbSource->sessionData().sourceType == LiveLogSourceType::IosLogStream;
        const auto userAction = QMessageBox::question(
            this,
            isIosLogStream ? tr( "klogg - clear iOS log stream" )
                           : tr( "klogg - clear logcat buffer" ),
            isIosLogStream
                ? tr( "Clear local iOS log stream capture for %1? The live stream will be "
                      "restarted if the source was connected." )
                      .arg( displayName )
                : tr( "Clear device log buffer for %1? Local cached log will also be removed." )
                      .arg( displayName ),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

        if ( userAction == QMessageBox::Yes ) {
            if ( !adbSource->clearAndRestart() ) {
                QMessageBox::critical(
                    this,
                    isIosLogStream ? tr( "klogg - clear iOS log stream" )
                                   : tr( "klogg - clear logcat buffer" ),
                    adbSource->lastError().isEmpty()
                        ? ( isIosLogStream ? tr( "Failed to clear iOS log stream" )
                                           : tr( "Failed to clear logcat buffer" ) )
                        : adbSource->lastError() );
            }
        }
        return;
    }

    const auto currentFile = session_.getFilename( crawler );
    const auto userAction = QMessageBox::question(
        this, tr( "klogg - clear file" ),
        tr( "Clear file %1? File content will be removed from disk, this is irreversible" )
            .arg( currentFile ),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

    if ( userAction == QMessageBox::Yes ) {
        QFile::resize( currentFile, 0 );
    }
}

void MainWindow::saveCurrentLiveLog( LiveLogSaveAnsiMode ansiMode )
{
    auto* crawler = currentCrawlerWidget();
    if ( !crawler || session_.getDocumentKind( crawler ) != DocumentKind::AdbLogcat ) {
        return;
    }

    auto* adbSource = session_.getAdbLogcatSource( crawler );
    if ( !adbSource ) {
        return;
    }

    auto suggestedPath = adbSource->sessionData().boundOutputFile;
    if ( suggestedPath.isEmpty() ) {
        suggestedPath
            = QDir::home().filePath( session_.getDisplayName( crawler ) + QStringLiteral( ".log" ) );
    }

    QString outputPath;
    {
        ScopedMainWindowShortcutSuspender shortcutSuspender( this );
        outputPath = QFileDialog::getSaveFileName(
            this, tr( "Save live log" ), suggestedPath,
            tr( "Log files (*.log *.txt);;All files (*)" ) );
    }
    if ( outputPath.isEmpty() ) {
        return;
    }

    if ( !adbSource->bindOutputFile( outputPath, ansiMode ) ) {
        QMessageBox::critical( this, tr( "Save live log" ),
                               tr( "Failed to bind live capture to %1" ).arg( outputPath ) );
        return;
    }

    updateLiveTabAppearance( crawler );
    updateMenuBarFromDocument( crawler );
    updateOpenedFilesMenu();
    updateInfoLine();
    scheduleSessionPersistence();
}

void MainWindow::disconnectCurrentSource()
{
    stopReconnectCountdown();

    auto* crawler = currentCrawlerWidget();
    if ( !crawler || session_.getDocumentKind( crawler ) != DocumentKind::AdbLogcat ) {
        return;
    }

    if ( auto* adbSource = session_.getAdbLogcatSource( crawler ) ) {
        adbSource->disconnectSource();
        updateMenuBarFromDocument( crawler );
    }
}

void MainWindow::reconnectCurrentSource()
{
    auto* crawler = currentCrawlerWidget();
    if ( !crawler || session_.getDocumentKind( crawler ) != DocumentKind::AdbLogcat ) {
        return;
    }

    if ( auto* adbSource = session_.getAdbLogcatSource( crawler ) ) {
        if ( !adbSource->reconnectSource() && !adbSource->lastError().isEmpty() ) {
            QMessageBox::warning( this, tr( "Reconnect source" ), adbSource->lastError() );
        }
        updateMenuBarFromDocument( crawler );
    }
}

void MainWindow::copyFullPath()
{
    // Route via currentView() (not currentCrawlerWidget()) so folder tabs work
    // too -- a folder tab's associated path is the folder path. Session
    // accessors key on ViewInterface*, which both tab kinds implement.
    auto* view = currentView();
    if ( view == nullptr ) {
        return;
    }

    // Agree with the info line: on a folder tab with a file open in the main
    // view, the info line shows THAT file, so Copy Path copies it too (the
    // folder path is the fallback when nothing is open).
    if ( auto* document = currentDocument() ) {
        if ( const auto info = document->currentMainViewInfo();
             info.has_value() && !info->path.isEmpty() ) {
            sendTextToClipboard( QDir::toNativeSeparators( info->path ) );
            return;
        }
    }

    const auto associatedPath = session_.getAssociatedPath( view );
    const auto text = associatedPath.isEmpty() ? session_.getDocumentId( view ) : associatedPath;
    sendTextToClipboard( QDir::toNativeSeparators( text ) );
}

void MainWindow::openContainingFolder()
{
    // Route via currentView() so folder tabs work too (reveals the folder).
    auto* view = currentView();
    if ( view == nullptr ) {
        return;
    }

    const auto associatedPath = session_.getAssociatedPath( view );
    if ( !associatedPath.isEmpty() ) {
        showPathInFileExplorer( associatedPath );
    }
}

void MainWindow::openInEditor()
{
    auto* crawler = currentCrawlerWidget();
    if ( !crawler ) {
        return;
    }

    const auto associatedPath = session_.getAssociatedPath( crawler );
    if ( !associatedPath.isEmpty() ) {
        openFileInDefaultApplication( associatedPath );
    }
}

void MainWindow::tryOpenClipboard( int tryTimes )
{
    auto clipboard = QGuiApplication::clipboard();
    auto text = clipboard->text();

    if ( text.isEmpty() && tryTimes > 0 ) {
        QTimer::singleShot( 50, [ tryTimes, this ]() { tryOpenClipboard( tryTimes - 1 ); } );
    }
    else {
        auto tempFile = new QTemporaryFile( tempDir_.filePath( "klogg_clipboard" ), this );
        if ( tempFile->open() ) {
            tempFile->write( text.toUtf8() );
            tempFile->flush();

            loadFile( tempFile->fileName() );
        }
    }
}

void MainWindow::openClipboard()
{
    tryOpenClipboard( ClipboardMaxTry );
}

void MainWindow::openUrl()
{
    bool ok;
    const auto urlInClipboard = QUrl::fromUserInput( QApplication::clipboard()->text() );
    const auto selectedUrl = urlInClipboard.isValid() ? urlInClipboard.toString() : QString{};

    QString url
        = QInputDialog::getText( this, tr( "Open URL as log file" ), tr( "URL to download:" ),
                                 QLineEdit::Normal, selectedUrl, &ok );
    if ( ok && !url.isEmpty() ) {
        openRemoteFile( url );
    }
}

// Opens the 'Highlighters' dialog box
void MainWindow::editHighlighters()
{
    HighlightersDialog dialog( this );
    signalMux_.connect( &dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );

    connect( &dialog, &HighlightersDialog::optionsChanged,
             [ this ]() { updateHighlightersMenu(); } );

    dialog.exec();
    signalMux_.disconnect( &dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );
}

// Opens dialog to configure predefined filters
void MainWindow::editPredefinedFilters( const QString& newFilter )
{
    PredefinedFiltersDialog dialog( newFilter, this );

    signalMux_.connect( &dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );

    dialog.exec();
    signalMux_.disconnect( &dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );
}

void MainWindow::importFilterFavorites()
{
    const auto file
        = QFileDialog::getOpenFileName( this, tr( "Select file to import" ), "",
                                        tr( "Filter favorites (*.conf);;All files (*)" ) );

    if ( file.isEmpty() ) {
        return;
    }

    const auto filters = PredefinedFiltersCollection::loadFromFile( file );
    PredefinedFiltersCollection::getSynced().saveToStorage( filters );
    Q_EMIT optionsChanged();
}

void MainWindow::exportFilterFavorites()
{
    auto file = QFileDialog::getSaveFileName( this, tr( "Export filter favorites" ), "",
                                              tr( "Filter favorites (*.conf)" ) );

    if ( file.isEmpty() ) {
        return;
    }

    if ( !file.endsWith( ".conf" ) ) {
        file += ".conf";
    }

    PredefinedFiltersCollection::saveToFile( file,
                                             PredefinedFiltersCollection::getSynced().getFilters() );
}

// Opens the 'Options' modal dialog box
void MainWindow::options()
{
    OptionsDialog dialog( this );
    signalMux_.connect( &dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );

    connect( &dialog, &OptionsDialog::optionsChanged, [ this ]() {
        const auto& config = Configuration::get();
        logging::enableFileLogging( config.enableLogging(),
                                    static_cast<logging::LogLevel>( config.loggingLevel() ) );

        newWindowAction->setVisible( config.allowMultipleWindows() );
        followAction->setEnabled( config.anyFileWatchEnabled() );

        updateShortcuts();
        updateRecentFileActions();
        updateRecentFolderActions();
    } );
    dialog.exec();

    signalMux_.disconnect( &dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );
}

void MainWindow::about()
{
    QMessageBox::about(
        this, tr( "About klogg" ),
        tr( "<h2>klogg %1</h2>"
            "<p>A fast, advanced log explorer.</p>"
            "<p>Built %2 from %3</p>"
            "<p><a href=\"https://github.com/ZEACENT/klogg\">https://github.com/ZEACENT/klogg</a></p>"
            "<p>This is fork of glogg</p>"
            "<p><a href=\"http://glogg.bonnefon.org/\">http://glogg.bonnefon.org/</a></p>"
            "<p>Using icons from <a href=\"https://icons8.com\">icons8.com</a> project</p>"
            "<p>Copyright &copy; 2020-%4 ZEACENT, Nicolas Bonnefon, Anton Filimonov and other contributors</p>"
            "<p>You may modify and redistribute the program under the terms of the GPL (version 3 "
            "or later).</p>" )
            .arg( kloggVersion(), kloggBuildDate(), kloggCommit(), kloggBuildYear() ) );
}

void MainWindow::aboutQt()
{
    QMessageBox::aboutQt( this, tr( "About Qt" ) );
}

void MainWindow::documentation()
{
    QFile doc( ":/documentation.html" );
    if ( doc.open( QIODevice::ReadOnly | QIODevice::Text ) ) {
        const auto text = QString::fromUtf8( doc.readAll() );
        QTextBrowser* tb = new QTextBrowser();
        tb->setOpenExternalLinks( true );
        tb->setHtml( text );
        tb->setWindowFlags( Qt::Window );
        tb->setAttribute( Qt::WA_DeleteOnClose );
        tb->setWindowTitle( tr( "klogg documentation" ) );
        tb->resize( this->width() / 2, this->height() );
        tb->show();
    }
    else {
        LOG_ERROR << "Can't open documentation resource";
    }
}

void MainWindow::checkForNewVersion()
{
    Q_EMIT checkForNewVersionRequested();
}

void MainWindow::showScratchPad()
{
    auto state = scratchPad_.windowState();
    state.setFlag( Qt::WindowMinimized, false );
    scratchPad_.setWindowState( state );

    scratchPad_.show();
    scratchPad_.activateWindow();
}

void MainWindow::sendToScratchpad( QString newData )
{
    scratchPad_.addData( newData );
    showScratchPad();
}

void MainWindow::replaceDataInScratchpad( QString newData )
{
    scratchPad_.replaceData( newData );
    showScratchPad();
}

void MainWindow::encodingChanged( QAction* action )
{
    const auto mibData = action->data();
    std::optional<int> mib;
    if ( mibData.isValid() ) {
        mib = mibData.toInt();
    }

    LOG_DEBUG << "encodingChanged, encoding " << mib.value_or( 0 );
    if ( auto doc = currentDocument() ) {
        doc->setEncoding( mib );
        updateInfoLine();
    }
}

void MainWindow::toggleOverviewVisibility( bool isVisible )
{
    auto& config = Configuration::get();
    config.setOverviewVisible( isVisible );
    config.save();
    Q_EMIT optionsChanged();
}

void MainWindow::toggleMainLineNumbersVisibility( bool isVisible )
{
    auto& config = Configuration::get();

    config.setMainLineNumbersVisible( isVisible );
    config.save();
    Q_EMIT optionsChanged();
}

void MainWindow::toggleFilteredLineNumbersVisibility( bool isVisible )
{
    auto& config = Configuration::get();

    config.setFilteredLineNumbersVisible( isVisible );
    config.save();
    Q_EMIT optionsChanged();
}

void MainWindow::changeFollowMode( bool follow )
{
    auto& config = Configuration::get();
    const auto currentCrawler = currentCrawlerWidget();
    const auto isLiveSource
        = currentCrawler && session_.getDocumentKind( currentCrawler ) == DocumentKind::AdbLogcat;
    if ( follow && !isLiveSource && !( config.nativeFileWatchEnabled() || config.pollingEnabled() ) ) {
        LOG_WARNING << "File watch disabled in settings";
    }

    followAction->setChecked( follow );
}

void MainWindow::lineNumberHandler( LineNumber startLine, LinesCount nLines, LineColumn startCol,
                                    LineLength nSymbols )
{
    lastStartLine_ = startLine;
    lastNLines_ = nLines;
    lastStartCol_ = startCol;
    lastNSymbols_ = nSymbols;

    // The line number received is the internal (starts at 0)
    uint64_t fileSize{};
    uint64_t fileNbLine{};
    QDateTime lastModified;

    // Guarded: this slot is signalMux-routed so it only fires for a file
    // current doc, but getFileInfo asserts on a null ViewInterface*
    // (session.cpp:358). Skip the Session call for a folder/no-tab current;
    // fileNbLine stays 0 and the line-number text is cleared below.
    if ( auto* cw = currentCrawlerWidget() ) {
        session_.getFileInfo( cw, &fileSize, &fileNbLine, &lastModified );
    }
    else if ( auto* doc = currentDocument() ) {
        // Folder tab: line count of the file currently in the folder main view
        // (0 when none is open -> lineNbField cleared below).
        if ( const auto info = doc->currentMainViewInfo() ) {
            fileNbLine = info->nbLines;
        }
    }

    if ( fileNbLine != 0 ) {
        QString lineText;
        if ( nSymbols.get() == 0 ) {
            lineText = tr( "Ln:%1/%2" ).arg( startLine.get() + 1 ).arg( fileNbLine );
        }
        else {
            if ( nLines.get() == 1 ) {
                lineText = tr( "Ln:%1/%2 Col:%3 Sel:%4|%5" )
                               .arg( startLine.get() + 1 )
                               .arg( fileNbLine )
                               .arg( startCol.get() )
                               .arg( nSymbols.get() )
                               .arg( nLines.get() );
            }
            else {
                lineText = tr( "Ln:%1/%2 Sel:%4|%5" )
                               .arg( startLine.get() + 1 )
                               .arg( fileNbLine )
                               .arg( nSymbols.get() )
                               .arg( nLines.get() );
            }
        }

        const auto* crawler = currentCrawlerWidget();
        if ( crawler ) {
            const auto pending = crawler->searchPendingLines();
            if ( pending > 0 ) {
                lineText += tr( " (+%1 pending)" ).arg( pending );
            }
        }

        lineNbField->setText( lineText );
    }
    else {
        lineNbField->clear();
    }
}

void MainWindow::refreshLineNumberField()
{
    lineNumberHandler( lastStartLine_, lastNLines_, lastStartCol_, lastNSymbols_ );
}

void MainWindow::onFolderMainViewNewSelection( LineNumber startLine, LinesCount nLines,
                                               LineColumn startCol, LineLength nSymbols )
{
    // The folder main view's newSelection is wired per-folder-widget in
    // currentTabChanged with Qt::UniqueConnection. Only act when a folder tab is
    // current so a backgrounded folder's selection never overwrites the status
    // bar of the active (e.g. single-file) tab. File tabs get the same broadcast
    // via signalMux; the folder is intentionally not a mux document.
    if ( currentCrawlerWidget() == nullptr ) {
        lineNumberHandler( startLine, nLines, startCol, nSymbols );
    }
}

void MainWindow::updateLoadingProgress( int progress )
{
    LOG_DEBUG << "Loading progress: " << progress;

    // Guarded: this slot is signalMux-routed (loadingProgressed) so it only
    // fires for a file current doc, but getDisplayName asserts on a null
    // ViewInterface* (session.cpp:358). Early-return for a folder/no-tab
    // current -- the folder info line is owned by currentTabChanged.
    if ( currentCrawlerWidget() == nullptr ) {
        return;
    }

    const auto currentFile
        = QDir::toNativeSeparators( session_.getDisplayName( currentCrawlerWidget() ) );

    // We ignore 0% and 100% to avoid a flash when the file (or update)
    // is very short.
    if ( progress > 0 && progress < 100 ) {
        infoLine->setText( currentFile + tr( " - Indexing lines... (%1 %)" ).arg( progress ) );
        infoLine->displayGauge( progress );

        showInfoLabels( false );

        stopAction->setEnabled( true );
        reloadAction->setEnabled( false );
    }
}

void MainWindow::handleLoadingFinished( LoadingStatus status )
{
    LOG_DEBUG << "handleLoadingFinished success=" << ( status == LoadingStatus::Successful );

    // No file is loading
    loadingFileName.clear();

    if ( status == LoadingStatus::Successful ) {
        updateInfoLine();

        infoLine->hideGauge();
        showInfoLabels( true );
        stopAction->setEnabled( false );
        reloadAction->setEnabled( true );

        // Only reset the line number for the initial file load.
        // For incremental updates (follow mode / live sources), preserve the
        // current line number so the display doesn't flip back to Ln:1/y.
        if ( auto* crawler = currentCrawlerWidget();
             crawler == nullptr || !crawler->isFirstLoadDone() ) {
            lineNumberHandler( 0_lnum, LinesCount( 0 ), LineColumn( 0 ), LineLength( 0 ) );
        }
        else {
            refreshLineNumberField();
        }

        // Now everything is ready, we can finally show the file!
        // Guarded: this slot is signalMux-routed (loadingFinished) so it only
        // fires when a CrawlerWidget is the current document, but the deref was
        // unguarded -- harden it so a folder/no-tab current can't crash.
        if ( auto* cw = currentCrawlerWidget() ) {
            cw->show();
        }
    }
    else {
        if ( status == LoadingStatus::NoMemory ) {
            QMessageBox alertBox;
            alertBox.setText( tr( "Not enough memory." ) );
            alertBox.setInformativeText(
                tr( "The system does not have enough memory to hold the index for this file. The "
                    "file will now be closed." ) );
            alertBox.setIcon( QMessageBox::Critical );
            alertBox.exec();
        }

        closeTab( mainTabWidget_.currentIndex(), ActionInitiator::App );
    }

    // mainTabWidget_.setEnabled( true );
}

void MainWindow::handleFilteredViewChanged()
{
    int currentIndex = mainTabWidget_.currentIndex();
    if ( currentIndex >= 0 ) {
        // qobject_cast (NOT static_cast): a folder tab is a FolderCrawlerWidget,
        // not a CrawlerWidget. registerSelector(nullptr) is the safe folder
        // behavior (quickfindmux.cpp:51 early-returns); the folder's own
        // SearchToolbar is unaffected.
        auto* crawler_widget = qobject_cast<CrawlerWidget*>( mainTabWidget_.widget( currentIndex ) );
        if ( crawler_widget != nullptr ) {
            quickFindMux_.registerSelector( crawler_widget );
        }
    }
}

void MainWindow::closeTab( int index, ActionInitiator initiator )
{
    // Folder tab close path. FolderCrawlerWidget is NOT a CrawlerWidget (it has
    // no stopLoading / ADB source / file-index semantics), so it must be
    // handled BEFORE the CrawlerWidget assert below, which would otherwise
    // abort in debug and null-deref in release.
    auto* folder_widget = qobject_cast<FolderCrawlerWidget*>( mainTabWidget_.widget( index ) );
    if ( folder_widget != nullptr ) {
        const auto documentId = session_.getDocumentId( folder_widget );
        const auto displayName = session_.getDisplayName( folder_widget );

        if ( initiator == ActionInitiator::User ) {
            auto& config = Configuration::get();
            if ( config.confirmTabClose() ) {
                QMessageBox msgBox( this );
                msgBox.setWindowTitle( tr( "Confirm Close" ) );
                msgBox.setText( tr( "Close \"%1\"?" ).arg( displayName ) );
                msgBox.setStandardButtons( QMessageBox::Yes | QMessageBox::No );
                msgBox.setDefaultButton( QMessageBox::Yes );

                QCheckBox* dontAskCheckBox = new QCheckBox( tr( "Don't ask again" ) );
                msgBox.setCheckBox( dontAskCheckBox );

                if ( msgBox.exec() != QMessageBox::Yes ) {
                    return;
                }

                if ( dontAskCheckBox->isChecked() ) {
                    config.setConfirmTabClose( false );
                    config.save();
                }
            }
        }

        // removeCrawler fires currentTabChanged for the NEW current tab
        // (synchronously). That queries Session for the new tab, not this
        // folder, so it is safe to run before session_.close below.
        mainTabWidget_.removeCrawler( index );

        if ( !shutdownInProgress_ ) {
            auto& groupManager = TabGroupManager::get();
            groupManager.removeTabFromGroup( documentId );
            groupManager.save();
        }

        // session_.close removes the folder from openFiles_ (keyed on its
        // ViewInterface*). Must run before deleteLater() resolves, otherwise a
        // stale ViewInterface* key would linger in the Session map.
        session_.close( folder_widget );

        updateOpenedFilesMenu();
        if ( !shutdownInProgress_ ) {
            scheduleSessionPersistence();
        }

        folder_widget->deleteLater();
        return;
    }

    auto widget = qobject_cast<CrawlerWidget*>( mainTabWidget_.widget( index ) );

    assert( widget );

    // Stop reconnect countdown if the closing tab is the countdown target
    if ( widget == reconnectCountdownCrawler_ ) {
        stopReconnectCountdown();
    }

    const auto documentId = session_.getDocumentId( widget );
    const auto displayName = session_.getDisplayName( widget );
    const auto associatedPath = session_.getAssociatedPath( widget );
    const auto documentKind = session_.getDocumentKind( widget );

    // Show confirmation dialog for user-initiated closes if enabled
    if ( initiator == ActionInitiator::User ) {
        auto& config = Configuration::get();
        if ( config.confirmTabClose() ) {
            QMessageBox msgBox( this );
            msgBox.setWindowTitle( tr( "Confirm Close" ) );
            msgBox.setText( tr( "Close \"%1\"?" ).arg( displayName ) );
            msgBox.setStandardButtons( QMessageBox::Yes | QMessageBox::No );
            msgBox.setDefaultButton( QMessageBox::Yes );

            QCheckBox* dontAskCheckBox = new QCheckBox( tr( "Don't ask again" ) );
            msgBox.setCheckBox( dontAskCheckBox );

            if ( msgBox.exec() != QMessageBox::Yes ) {
                return;
            }

            if ( dontAskCheckBox->isChecked() ) {
                config.setConfirmTabClose( false );
                config.save();
            }
        }
    }

    widget->stopLoading();
    mainTabWidget_.removeCrawler( index );

    if ( initiator == ActionInitiator::User && documentKind == DocumentKind::File ) {
        addRecentFile( associatedPath );
    }

    if ( !shutdownInProgress_ ) {
        auto& groupManager = TabGroupManager::get();
        groupManager.removeTabFromGroup( documentId );
        groupManager.save();
    }

    if ( documentKind == DocumentKind::AdbLogcat ) {
        if ( auto* adbSource = session_.getAdbLogcatSource( widget ) ) {
            adbSource->disconnectSource();
            // Preserve captures during app shutdown so restored ADB tabs keep their history.
            if ( initiator == ActionInitiator::User && !session_.exitRequested() ) {
                adbSource->deleteCaptureFiles();
            }
        }
    }

    session_.close( widget );

    updateOpenedFilesMenu();
    if ( !shutdownInProgress_ ) {
        scheduleSessionPersistence();
    }

    widget->deleteLater();
}

void MainWindow::currentTabChanged( int index )
{
    LOG_DEBUG << "currentTabChanged";

    if ( index >= 0 ) {
        auto* widget = mainTabWidget_.widget( index );
        // qobject_cast (NOT static_cast): a folder tab is a FolderCrawlerWidget,
        // not a CrawlerWidget. static_cast would produce a garbage pointer whose
        // dereference crashes (the original EXC_BAD_ACCESS at this site).
        auto* crawler_widget = qobject_cast<CrawlerWidget*>( widget );

        if ( crawler_widget != nullptr ) {
            // --- File / live-source tab ---
            signalMux_.setCurrentDocument( crawler_widget );
            quickFindMux_.registerSelector( crawler_widget );

            // New tab is set up with fonts etc...
            Q_EMIT optionsChanged();

            updateMenuBarFromDocument( crawler_widget );
            updateTitleBar( session_.getDisplayName( crawler_widget ) );
            updateFavoritesMenu();

            // Update infoLine when switching to a tab that isn't the countdown target
            if ( reconnectCountdownTimer_->isActive()
                 && crawler_widget != reconnectCountdownCrawler_ ) {
                updateInfoLine();
            }

            editMenu->setEnabled( true );
        }
        else {
            // --- Folder tab (FolderCrawlerWidget) ---
            // The folder is NOT registered as the signalMux document: the mux
            // routes file/live-source slots (reload/stopLoading) the folder
            // does not implement, and registering it would emit "No such slot"
            // warnings. Document-level actions (goToLine, go-to-top, follow,
            // wrap, focus-search, quickfind lifecycle) reach the folder via
            // currentDocument() virtual dispatch instead; config/view option
            // changes (line numbers, font, overview) are delivered directly
            // to applyConfiguration via the connection below.
            signalMux_.setCurrentDocument( nullptr );

            auto* folder_widget = qobject_cast<FolderCrawlerWidget*>( widget );
            // The folder implements QuickFindMuxSelectorInterface and its views
            // were rebound to the session QuickFindPattern (doSetQuickFindPattern
            // -> AbstractLogView::setQuickFindPattern), so the mux's pattern now
            // drives them -- Ctrl+F QuickFind works on folder tabs.
            quickFindMux_.registerSelector( folder_widget );

            if ( folder_widget != nullptr ) {
                // UniqueConnection: survives repeated tab switches without
                // duplicating; redundant fires while a non-folder tab is current
                // are harmless (applyConfiguration is idempotent and the folder
                // views are hidden).
                connect( this, &MainWindow::optionsChanged, folder_widget,
                         &FolderCrawlerWidget::applyConfiguration, Qt::UniqueConnection );
                // Refresh the info line (path/size/date/encoding) when the file
                // shown in the folder main view changes.
                connect( folder_widget, &FolderCrawlerWidget::mainViewFileChanged, this,
                         &MainWindow::updateInfoLine, Qt::UniqueConnection );
                // The encoding override resets on a main-view file switch, so
                // re-check the encoding menu alongside the info line.
                connect( folder_widget, &FolderCrawlerWidget::mainViewFileChanged, this,
                         &MainWindow::syncEncodingMenuFromDocument, Qt::UniqueConnection );
                // Re-register the QuickFind selector when the folder's pane set
                // changes (create/switch/close) so the mux never drives a
                // stale or freed pane view.
                connect( folder_widget, &FolderCrawlerWidget::searchablesChanged, this,
                         &MainWindow::onFolderSearchablesChanged, Qt::UniqueConnection );
                // Forward the folder main view's Ln:col selection to the status
                // bar (file tabs get this via signalMux). A real slot (not a
                // lambda) so Qt::UniqueConnection can dedupe across tab switches
                // (lambda+UniqueConnection warns and accumulates duplicates).
                connect( folder_widget->mainView(), &AbstractLogView::newSelection, this,
                         &MainWindow::onFolderMainViewNewSelection, Qt::UniqueConnection );
                // Scratchpad forwarding: single-file tabs reach these slots via
                // SignalMux (string slots); the folder is not a mux document,
                // so its equivalent widget-level signals are direct-connected.
                connect( folder_widget, &FolderCrawlerWidget::sendToScratchpad, this,
                         &MainWindow::sendToScratchpad, Qt::UniqueConnection );
                connect( folder_widget, &FolderCrawlerWidget::replaceDataInScratchpad, this,
                         &MainWindow::replaceDataInScratchpad, Qt::UniqueConnection );
                // Follow-state uplink (single-file parity with the mux-routed
                // CrawlerWidget::followModeChanged): the folder main view's
                // follow changes (elastic-hook disengage on scroll-up, ...) keep
                // the Follow action's checked state in sync.
                connect( folder_widget, &FolderCrawlerWidget::followModeChanged, this,
                         &MainWindow::changeFollowMode, Qt::UniqueConnection );
            }

            // Routes to the folder via the connection above.
            Q_EMIT optionsChanged();

            // Session accessors assert on a null ViewInterface*, so the folder
            // display name must be fetched via the ViewInterface cross-cast
            // (never nullptr for a real tab). dynamic_cast succeeds for both
            // CrawlerWidget and FolderCrawlerWidget.
            const auto* view = dynamic_cast<const ViewInterface*>( widget );
            updateTitleBar( view != nullptr ? session_.getDisplayName( view ) : QString() );

            {
                // Block the forced uncheck inside disableFileSpecificActions:
                // followAction::toggled is now dispatched to the CURRENT tab,
                // which is already this folder widget, so an unguarded
                // setChecked(false) would kill the folder main view's follow
                // mode before its state can be synced back below.
                const QSignalBlocker followActionBlocker( followAction );
                disableFileSpecificActions();
            }
            // A folder tab has a valid filesystem path (the folder), so Copy
            // Path and Open Containing Folder are meaningful (they operate on
            // the folder path via currentView()). Re-enable them; the other
            // actions disabled above (live-log save, disconnect/reconnect,
            // open-in-editor) remain folder-inapplicable.
            copyPathToClipboardAction->setEnabled( true );
            openContainingFolderAction->setEnabled( true );

            // Follow applies to the file shown in the folder main view: sync
            // the checked state from it, mirroring updateMenuBarFromDocument's
            // followAction->setChecked( crawler->isFollowEnabled() ) for file
            // tabs.
            followAction->setChecked( folder_widget != nullptr
                                      && folder_widget->isFollowEnabled() );

            infoLine->hideGauge();
            // updateInfoLine now owns the folder info line: it shows the file
            // currently in the main view (path/size/date/encoding), or the
            // folder path when no file is open.
            updateInfoLine();
            // Restore "Ln: x/y" for the file already in the folder main view.
            // Single-file tabs get this from signalMux's state broadcast; the
            // folder is intentionally not a mux document, so re-derive the
            // field from the widget's last announced line. Clear when no file.
            if ( folder_widget != nullptr && folder_widget->currentMainViewInfo().has_value() ) {
                lineNumberHandler( folder_widget->currentMainViewLine(), LinesCount( 1 ),
                                   LineColumn( 0 ), LineLength( 0 ) );
            }
            else {
                lineNbField->clear();
            }

            // Folder view supports select/copy.
            editMenu->setEnabled( true );

            // Document-level menu state (parity with updateMenuBarFromDocument
            // for file tabs): the wrap toggle reflects the folder views, and
            // the encoding menu checks the folder's current override.
            if ( folder_widget != nullptr ) {
                textWrapAction->setEnabled( true );
                textWrapAction->setChecked( folder_widget->isTextWrapEnabled() );
                syncEncodingMenuCheck( folder_widget->encodingMib() );
            }
        }
    }
    else {
        // No tab left
        signalMux_.setCurrentDocument( nullptr );
        quickFindMux_.registerSelector( nullptr );

        infoLine->hideGauge();
        infoLine->clear();
        showInfoLabels( false );

        updateTitleBar( QString() );

        editMenu->setEnabled( false );
        disableFileSpecificActions();
    }

    scheduleSessionPersistence();
}

void MainWindow::changeQFPattern( const QString& newPattern, bool ignoreCase, bool isRegex, bool isWholeWord )
{
    quickFindWidget_.changeDisplayedPattern( newPattern, ignoreCase, isRegex, isWholeWord );
}

void MainWindow::loadFileNonInteractive( const QString& file_name )
{
    LOG_DEBUG << "loadFileNonInteractive( " << file_name.toStdString() << " )";

    loadFile( file_name );

    // Try to get the window to the front
    // This is a bit of a hack but has been tested on:
    // Qt 5.3 / Gnome / Linux
    // Qt 5.11 / Win10
#ifdef Q_OS_WIN
    const auto isMaximized = isMaximized_;

    if ( isMaximized ) {
        showMaximized();
    }
    else {
        showNormal();
    }

    activateWindow();
    raise();
#else
    Qt::WindowFlags window_flags = windowFlags();
    window_flags |= Qt::WindowStaysOnTopHint;
    setWindowFlags( window_flags );

    raise();
    activateWindow();

    window_flags = windowFlags();
    window_flags &= ~Qt::WindowStaysOnTopHint;
    setWindowFlags( window_flags );
    show();
#endif

    if ( auto currentCrawler = currentCrawlerWidget() ) {
        currentCrawler->setFocus();
    }
}

//
// Events
//

// Closes the application
void MainWindow::closeEvent( QCloseEvent* event )
{
    if ( !isCloseFromTray_ && this->isVisible() && Configuration::get().minimizeToTray() ) {
        event->ignore();
        trayIcon_->show();
        this->hide();
    }
    else {
        session_.close();

        shutdownInProgress_ = true;
        suspendSessionPersistence_ = true;
        writeSettings();
        TabGroupManager::get().save();

        closeAll( ActionInitiator::App );
        trayIcon_->hide();
        Q_EMIT windowClosed();

        event->accept();
    }
}

// Minimize handling the application
void MainWindow::changeEvent( QEvent* event )
{
    if ( event->type() == QEvent::WindowStateChange ) {
        isMaximized_ = windowState().testFlag( Qt::WindowMaximized );

        if ( this->windowState() & Qt::WindowMinimized ) {
            if ( Configuration::get().minimizeToTray() ) {
                dispatchToMainThread( [ this ] {
                    trayIcon_->show();
                    this->hide();
                } );
            }
        }
    }
    else if ( event->type() == QEvent::StyleChange ) {
        dispatchToMainThread( [ this ] {
            loadIcons();
            updateOpenedFilesMenu();
            updateFavoritesMenu();
            updateHighlightersMenu();
        } );
    }
    else if ( event->type() == QEvent::LanguageChange ) {
        reTranslateUI();
    }

    QMainWindow::changeEvent( event );
}

// Accepts the drag event if it looks like a filename
void MainWindow::dragEnterEvent( QDragEnterEvent* event )
{
    if ( event->mimeData()->hasFormat( "text/uri-list" ) )
        event->acceptProposedAction();
}

// Tries and loads the file if the URL dropped is local
void MainWindow::dropEvent( QDropEvent* event )
{
    const QList<QUrl> urls = event->mimeData()->urls();

    QStringList localPaths;
    for ( const auto& url : urls ) {
        auto fileName = url.toLocalFile();
        if ( !fileName.isEmpty() ) {
            localPaths.append( fileName );
        }
    }

    const auto classified = classifyLocalPaths( localPaths );

    // Show the merge question over the dropped files BEFORE opening anything,
    // so Cancel aborts the whole drop (no half-applied state with some tabs
    // already opened). The prompt is only about files; directories are opened
    // unconditionally as folder tabs.
    bool mergeRequested = false;
    if ( classified.files.size() > 1 ) {
        const auto userAction = QMessageBox::question(
            this, tr( "Multiple Files" ),
            tr( "You dropped %1 files. Do you want to merge them into a single view?" )
                .arg( classified.files.size() ),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes );

        if ( userAction == QMessageBox::Cancel ) {
            return;
        }
        mergeRequested = ( userAction == QMessageBox::Yes );
    }

    // Open each dropped directory as its own folder crawler tab. This is the
    // fix: previously a dropped directory fell through to loadFile and failed
    // in Decompressor::action.
    for ( const auto& dir : classified.dirs ) {
        openFolderByPath( dir );
    }

    // Handle the dropped files via the existing merge / loadFile flow.
    if ( mergeRequested ) {
        auto filesToMerge = showMergeFilesDialog( classified.files );
        if ( !filesToMerge.empty() ) {
            executeMerge( filesToMerge );
            return;
        }
        // User cancelled merge dialog -- fall through to open files separately
    }

    for ( const auto& fileName : classified.files ) {
        loadFile( fileName );
    }
}

bool MainWindow::event( QEvent* event )
{
    if ( event->type() == QEvent::WindowActivate ) {
        Q_EMIT windowActivated();
    }
    else if ( event->type() == QEvent::Show ) {
        if ( this->windowHandle() ) {
            std::call_once( screenChangesConnect_, [ this ]() {
                logScreenInfo( this->windowHandle()->screen() );
                connect( this->windowHandle(), &QWindow::screenChanged,
                         [ this ]( QScreen* screen ) { logScreenInfo( screen ); } );
            } );
        }
    }

    return QMainWindow::event( event );
}

//
// Private functions
//

bool MainWindow::extractAndLoadFile( const QString& fileName )
{
    const auto& config = Configuration::get();

    if ( !config.extractArchives() ) {
        return false;
    }

    if ( !config.extractArchivesAlways() ) {
        const auto userChoice
            = QMessageBox::question( this, tr( "klogg" ), tr( "Extract archive to temp folder?" ) );
        if ( userChoice == QMessageBox::No ) {
            return false;
        }
    }

    const auto decompressAction = Decompressor::action( fileName );

    Decompressor decompressor;
    AtomicFlag decompressInterrupt;

    QProgressDialog progressDialog;
    progressDialog.setLabelText( tr( "Extracting %1" ).arg( fileName ) );
    progressDialog.setRange( 0, 0 );

    // Note: this progressDialog is parentless.  After each exec() call below,
    // QCoreApplication::processEvents() must be called to drain stale Cocoa
    // events while the NSWindow is still alive.  See openRemoteFile() for the
    // full explanation of the macOS Cocoa lifecycle issue.

    connect( &decompressor, &Decompressor::finished,
             [ &progressDialog ]( bool isOk ) { progressDialog.done( isOk ? 0 : 1 ); } );
    connect( &progressDialog, &QProgressDialog::canceled,
             [ &decompressInterrupt, &decompressor ]() {
                 decompressInterrupt.set();
                 decompressor.waitForResult();
             } );

    if ( decompressAction == DecompressAction::Decompress ) {

        auto tempFile = new QTemporaryFile(
            this->tempDir_.filePath( QFileInfo( fileName ).fileName() ), this );

        const bool decompressOk = tempFile->open()
                                  && decompressor.decompress( fileName, tempFile,
                                                              decompressInterrupt );
        const bool decompressAccepted = decompressOk && !progressDialog.exec();

        // Drain stale Cocoa events while the dialog's NSWindow is still alive.
        QCoreApplication::processEvents();

        if ( decompressAccepted ) {

            if ( decompressInterrupt ) {
                return false;
            }

            return this->loadFile( tempFile->fileName() );
        }
        else {
            QMessageBox::warning(
                this, tr( "klogg" ),
                tr( "Failed to decompress %1" ).arg( QDir::toNativeSeparators( fileName ) ) );
        }
    }
    else if ( decompressAction == DecompressAction::Extract ) {
        QTemporaryDir archiveDir{ this->tempDir_.filePath( QFileInfo( fileName ).fileName() ) };
        archiveDir.setAutoRemove( false );

        const bool extractOk
            = decompressor.extract( fileName, archiveDir.path(), decompressInterrupt );
        const bool extractAccepted = extractOk && !progressDialog.exec();

        // Drain stale Cocoa events while the dialog's NSWindow is still alive.
        QCoreApplication::processEvents();

        if ( extractAccepted ) {

            if ( decompressInterrupt ) {
                return false;
            }

            const auto selectedFiles = QFileDialog::getOpenFileNames(
                this, tr( "Open file from archive" ), archiveDir.path(), tr( "All files (*)" ) );

            for ( const auto& extractedFile : selectedFiles ) {
                this->loadFile( extractedFile );
            }

            return true;
        }
        else {
            QMessageBox::warning(
                this, tr( "klogg" ),
                tr( "Failed to extract %1" ).arg( QDir::toNativeSeparators( fileName ) ) );
        }
    }

    return false;
}

// Create a CrawlerWidget for the passed file, start its loading
// and update the title bar.
// The loading is done asynchronously.
bool MainWindow::loadFile( const QString& fileName, bool followFile )
{
    LOG_DEBUG << "loadFile ( " << fileName.toStdString() << " )";

    // Directory guard: any caller that hands loadFile a directory is routed to
    // the folder-open flow instead of falling into Decompressor::action /
    // session_.open (which fail on a directory). This single guard covers every
    // caller (loadInitialFile/CLI, loadFileNonInteractive/IPC+FileOpenEvent,
    // recent, favorites, open, openRemoteFile, extractAndLoadFile). Returns
    // true to signal 'handled' -- matching loadFile's success contract; the
    // boolean is ignored by loadInitialFile/loadFileNonInteractive.
    if ( isDirectoryPath( fileName ) ) {
        openFolderByPath( fileName );
        return true;
    }

    // First check if the file is already open... (dynamic_cast, not static_cast:
    // getViewIfOpen returns ViewInterface* and can match a folder entry whose
    // fileName == folderPath; the isDirectoryPath guard above routes directories
    // away, but dynamic_cast returns nullptr for a folder instead of a wrong-type
    // pointer -- the same anti-pattern that crashed folder tabs.)
    auto* existing_crawler = dynamic_cast<CrawlerWidget*>( session_.getViewIfOpen( fileName ) );

    if ( existing_crawler ) {
        auto* crawlerWindow = qobject_cast<MainWindow*>( existing_crawler->window() );
        crawlerWindow->mainTabWidget_.setCurrentWidget( existing_crawler );
        crawlerWindow->activateWindow();
        return true;
    }

    const auto decompressAction = Decompressor::action( fileName );

    if ( decompressAction == DecompressAction::None || !Configuration::get().extractArchives() ) {
        // Load the file
        loadingFileName = fileName;

        try {
            const auto previousViewContext = [ &fileName ]() {
                const auto& session = SessionInfo::getSynced();
                const auto& windows = session.windows();
                for ( const auto& windowId : windows ) {
                    const auto openedFiles = session.openFiles( windowId );
                    const auto existingContext
                        = std::find_if( openedFiles.begin(), openedFiles.end(),
                                        [ &fileName ]( const auto& context ) {
                                            return context.fileName == fileName;
                                        } );
                    if ( existingContext != openedFiles.end() ) {
                        return existingContext->viewContext;
                    }
                }
                return QString{};
            }();

            CrawlerWidget* crawler_widget = static_cast<CrawlerWidget*>(
                session_.open( fileName, []() { return new CrawlerWidget(); } ) );

            if ( !crawler_widget ) {
                LOG_ERROR << "Can't create crawler for " << fileName.toStdString();
                return false;
            }

            // We won't show the widget until the file is fully loaded
            crawler_widget->hide();

            if ( !previousViewContext.isEmpty() ) {
                LOG_INFO << "Found existing context";
                crawler_widget->setViewContext( previousViewContext );
            }

            // We disable the tab widget to avoid having someone switch
            // tab during loading. (maybe FIXME)
            // mainTabWidget_.setEnabled( false );

            int index = mainTabWidget_.addCrawler( crawler_widget, fileName, {}, fileName );
            // Setting the new tab, the user will see a blank page for the duration
            // of the loading, with no way to switch to another tab
            mainTabWidget_.setCurrentIndex( index );

            addRecentFile( fileName );
            updateOpenedFilesMenu();

            const auto& config = Configuration::get();
            if ( config.anyFileWatchEnabled() && ( followFile || config.followFileOnLoad() ) ) {
                signalCrawlerToFollowFile( crawler_widget );
                followAction->setChecked( true );
            }
        } catch ( ... ) {
            LOG_ERROR << "Can't open file " << fileName.toStdString();
            return false;
        }

        LOG_DEBUG << "Success loading file " << fileName.toStdString();
        scheduleSessionPersistence();
        return true;
    }
    else {
        return extractAndLoadFile( fileName );
    }
}

bool MainWindow::openAdbLogcatSource( const AdbLogcatSessionData& sessionData, bool startConnected )
{
    CrawlerWidget* crawlerWidget = static_cast<CrawlerWidget*>(
        session_.openAdbLogcat( sessionData, []() { return new CrawlerWidget(); }, startConnected ) );

    if ( !crawlerWidget ) {
        return false;
    }

    crawlerWidget->hide();

    const auto toolTip = session_.getAssociatedPath( crawlerWidget ).isEmpty()
                             ? session_.getDisplayName( crawlerWidget )
                             : session_.getAssociatedPath( crawlerWidget );
    const auto index = mainTabWidget_.addCrawler(
        crawlerWidget, session_.getDocumentId( crawlerWidget ), session_.getDisplayName( crawlerWidget ),
        toolTip );
    mainTabWidget_.setCurrentIndex( index );

    registerAdbLogcatSource( crawlerWidget );
    signalCrawlerToFollowFile( crawlerWidget );
    followAction->setChecked( true );

    updateOpenedFilesMenu();
    scheduleSessionPersistence();

    if ( auto* adbSource = session_.getAdbLogcatSource( crawlerWidget );
         adbSource != nullptr && adbSource->state() == AdbLogcatSource::State::Error
         && !adbSource->lastError().isEmpty() ) {
        QMessageBox::warning( this, tr( "Open ADB Logcat" ), adbSource->lastError() );
    }

    return true;
}

// Strips the passed filename from its directory part.
QString MainWindow::strippedName( const QString& fullFileName ) const
{
    return klogg::displayNameForPath( fullFileName );
}

// Return the currently active CrawlerWidget, or NULL if none
CrawlerWidget* MainWindow::currentCrawlerWidget() const
{
    auto current = qobject_cast<CrawlerWidget*>( mainTabWidget_.currentWidget() );

    return current;
}

// The AbstractCrawlerWidget* of the current tab (CrawlerWidget or
// FolderCrawlerWidget) -- the polymorphic dispatch target. nullptr only when
// there is no current tab.
AbstractCrawlerWidget* MainWindow::currentDocument() const
{
    return dynamic_cast<AbstractCrawlerWidget*>( mainTabWidget_.currentWidget() );
}

// True if the tab at `index` is a FolderCrawlerWidget (folder mode), not a
// CrawlerWidget. qobject_cast returns nullptr for non-matching types, so this
// is the safe type test (static_cast would lie here -- the original crash).
bool MainWindow::isFolderTab( int index ) const
{
    return qobject_cast<const FolderCrawlerWidget*>( mainTabWidget_.widget( index ) ) != nullptr;
}

// The ViewInterface cross-cast of the current tab widget. Succeeds for BOTH
// CrawlerWidget and FolderCrawlerWidget (both multiply-inherit ViewInterface);
// returns nullptr only when there is no current tab. Session accessors key on
// this pointer and assert on nullptr, so folder display names must be fetched
// via this cross-cast (mirrors the already-fixed writeSettings at L3165).
const ViewInterface* MainWindow::currentView() const
{
    return dynamic_cast<const ViewInterface*>( mainTabWidget_.currentWidget() );
}

// Disable every file-specific menu action. Used for the folder-tab and no-tab
// states so a user can't trigger a slot that would deref a null CrawlerWidget.
void MainWindow::disableFileSpecificActions()
{
    followAction->setChecked( false );
    followAction->setEnabled( Configuration::get().anyFileWatchEnabled() );
    addToFavoritesAction->setEnabled( false );
    addToFavoritesMenuAction->setEnabled( false );
    saveCurrentLiveLogMenu->setEnabled( false );
    disconnectSourceAction->setEnabled( false );
    reconnectSourceAction->setEnabled( false );
    openContainingFolderAction->setEnabled( false );
    openInEditorAction->setEnabled( false );
    copyPathToClipboardAction->setEnabled( false );
}

// Update the title bar.
void MainWindow::updateTitleBar( const QString& file_name )
{
    QString shownName = tr( "Untitled" );
    if ( !file_name.isEmpty() ) {
        shownName = strippedName( file_name );
        if ( shownName.isEmpty() ) {
            shownName = file_name;
        }
    }

    QString indexPart = "";
    if ( session_.windowIndex() > 0 ) {
        indexPart = QString( " #%1" ).arg( session_.windowIndex() + 1 );
    }

    setWindowTitle( tr( "%1 - %2%3" ).arg( shownName, tr( "klogg" ), indexPart ) + tr( " (build " )
                    + kloggVersion() + ")" );
}

void MainWindow::addRecentFile( const QString& fileName )
{
    auto& recentFiles = RecentFiles::getSynced();
    recentFiles.addRecent( fileName );
    recentFiles.save();
    updateRecentFileActions();
}

void MainWindow::addRecentFolder( const QString& folderPath )
{
    auto& recentFolders = RecentFolders::getSynced();
    recentFolders.addRecent( folderPath );
    recentFolders.save();
    updateRecentFolderActions();
}

void MainWindow::updateLiveTabAppearance( CrawlerWidget* crawler )
{
    const auto tabIndex = mainTabWidget_.indexOf( crawler );
    if ( tabIndex < 0 ) {
        return;
    }

    auto* source = session_.getAdbLogcatSource( crawler );
    const auto displayName = session_.getDisplayName( crawler );
    const auto baseTip = session_.getAssociatedPath( crawler ).isEmpty()
                             ? displayName
                             : session_.getAssociatedPath( crawler );

    QString toolTip = baseTip;
    if ( source ) {
        const auto state = source->state();
        LiveTabStatus liveStatus = LiveTabStatus::Connected;
        if ( state == AdbLogcatSource::State::Error ) {
            if ( source->isAutoReconnectActive() ) {
                liveStatus = LiveTabStatus::Reconnecting;
                toolTip = tr( "%1\nReconnecting... (attempt %2)" )
                              .arg( baseTip )
                              .arg( source->reconnectAttempt() + 1 );
            }
            else {
                liveStatus = LiveTabStatus::Error;
                if ( !source->lastError().isEmpty() ) {
                    toolTip = tr( "%1\nError: %2" ).arg( baseTip, source->lastError() );
                }
            }
        }
        else if ( state == AdbLogcatSource::State::Disconnected ) {
            liveStatus = LiveTabStatus::Disconnected;
        }
        mainTabWidget_.setLiveTabStatus( tabIndex, liveStatus );
    }

    mainTabWidget_.updateCrawler( tabIndex, displayName, toolTip );
}

void MainWindow::registerAdbLogcatSource( CrawlerWidget* crawler )
{
    if ( !crawler || session_.getDocumentKind( crawler ) != DocumentKind::AdbLogcat ) {
        return;
    }

    auto* adbSource = session_.getAdbLogcatSource( crawler );
    if ( !adbSource ) {
        return;
    }

    // Apply auto-reconnect and capture limit configuration from session data
    // (populated by the open dialog or session restore).
    const auto& sessionData = adbSource->sessionData();
    adbSource->setAutoReconnectEnabled( sessionData.autoReconnectEnabled );
    adbSource->setAutoReconnectMaxAttempts( sessionData.maxReconnectAttempts );
    adbSource->setCaptureLimits( sessionData.captureMaxFileSize,
                                 sessionData.captureBackupCount );

    connect( adbSource, &AdbLogcatSource::stateChanged, this,
             [ this, crawler ]( AdbLogcatSource::State state ) {
                 if ( currentCrawlerWidget() == crawler ) {
                     updateMenuBarFromDocument( crawler );
                     if ( state == AdbLogcatSource::State::Connected ) {
                         stopReconnectCountdown();
                     }
                     updateInfoLine();
                 }
                 updateOpenedFilesMenu();
                 updateLiveTabAppearance( crawler );
             } );
    connect( adbSource, &AdbLogcatSource::errorOccurred, this,
             [ this, crawler ]( const QString& ) {
                 if ( currentCrawlerWidget() == crawler ) {
                     updateInfoLine();
                 }
                 updateLiveTabAppearance( crawler );
             } );
    connect( adbSource, &AdbLogcatSource::reconnectAttemptStarted, this,
             [ this, crawler ]( int ) {
                 updateLiveTabAppearance( crawler );
                 const auto* source = session_.getAdbLogcatSource( crawler );
                 if ( source ) {
                     startReconnectCountdown( crawler, source->reconnectRemainingMs() );
                 }
             } );

    // Sync tab appearance immediately in case the source is already
    // in Error or Disconnected state (e.g. during session restore).
    updateLiveTabAppearance( crawler );
}

void MainWindow::startReconnectCountdown( CrawlerWidget* crawler, int delayMs )
{
    reconnectCountdownCrawler_ = crawler;
    reconnectCountdownTotalMs_ = delayMs;
    reconnectCountdownEndMs_ = QDateTime::currentMSecsSinceEpoch() + delayMs;
    reconnectCountdownTimer_->start();
    updateReconnectCountdown(); // immediate first update
}

void MainWindow::stopReconnectCountdown()
{
    if ( !reconnectCountdownTimer_->isActive() ) {
        return;
    }
    reconnectCountdownTimer_->stop();
    reconnectCountdownCrawler_ = nullptr;
    reconnectCountdownEndMs_ = 0;
    reconnectCountdownTotalMs_ = 0;
    infoLine->hideGauge();
    // Restore normal infoLine if the current tab matches
    if ( currentCrawlerWidget() ) {
        updateInfoLine();
    }
}

void MainWindow::updateReconnectCountdown()
{
    if ( !reconnectCountdownCrawler_
         || currentCrawlerWidget() != reconnectCountdownCrawler_ ) {
        return;
    }

    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto remainingMs = static_cast<int>( reconnectCountdownEndMs_ - now );
    const auto displayName = session_.getDisplayName( reconnectCountdownCrawler_ );

    if ( remainingMs <= 0 ) {
        // Timer expired — the reconnect attempt is about to fire.
        // Keep showing "Reconnecting..." until state changes.
        infoLine->setText( QDir::toNativeSeparators( displayName )
                           + tr( " - Reconnecting..." ) );
        infoLine->displayGauge( 100 );
        return;
    }

    const auto remainingSec = ( remainingMs + 999 ) / 1000; // round up
    infoLine->setText( QDir::toNativeSeparators( displayName )
                       + tr( " - Reconnect in %1 seconds..." ).arg( remainingSec ) );

    // Gauge: percentage of delay elapsed (fills left-to-right as time passes)
    const auto elapsedMs = reconnectCountdownTotalMs_ - remainingMs;
    const auto pct = static_cast<int>( elapsedMs * 100 / reconnectCountdownTotalMs_ );
    infoLine->displayGauge( pct );
}

// Updates the actions for the recent files.
// Must be called after having added a new name to the list.
void MainWindow::updateRecentFileActions()
{
    auto& recentFiles = RecentFiles::get();
    QStringList recent_files = recentFiles.recentFiles();
    int recent_files_max_items = recentFiles.getNumberItemsToShow();

    if ( recentFiles.recentFiles().count() > 0 ) {
        recentFilesMenu->setEnabled( true );
        for ( auto j = 0; j < MAX_RECENT_FILES; ++j ) {
            const auto actionIndex = static_cast<size_t>( j );
            if ( j < recent_files_max_items ) {
                int key = j + ( ( j < 9 ) ? 0x31 : ( 0x61 - 9 ) ); // shortcuts: 1..9 next a,b...
                QString text
                    = tr( "&%1 %2" ).arg( QChar( key ) ).arg( strippedName( recent_files[ j ] ) );
                recentFileActions[ actionIndex ]->setText( text );
                recentFileActions[ actionIndex ]->setToolTip( recent_files[ j ] );
                recentFileActions[ actionIndex ]->setData( recent_files[ j ] );
                recentFileActions[ actionIndex ]->setVisible( true );
            }
            else {
                recentFileActions[ actionIndex ]->setVisible( false );
            }
        }
    }
    else {
        recentFilesMenu->setEnabled( false );
    }

    // separatorAction->setVisible(!recentFiles.isEmpty());
}

// Clear the list of the recent files
void MainWindow::clearRecentFileActions()
{
    auto& recentFiles = RecentFiles::getSynced();
    recentFiles.removeAll();
    recentFiles.save();
    updateRecentFileActions();
}

// Updates the actions for the recent folders.
// Must be called after having added a new folder to the list.
void MainWindow::updateRecentFolderActions()
{
    auto& recentFolders = RecentFolders::get();
    QStringList recent_folders = recentFolders.recentFolders();
    int recent_folders_max_items = recentFolders.getNumberItemsToShow();

    if ( recentFolders.recentFolders().count() > 0 ) {
        recentFoldersMenu->setEnabled( true );
        for ( auto j = 0; j < MAX_RECENT_FILES; ++j ) {
            const auto actionIndex = static_cast<size_t>( j );
            if ( j < recent_folders_max_items ) {
                int key = j + ( ( j < 9 ) ? 0x31 : ( 0x61 - 9 ) ); // shortcuts: 1..9 next a,b...
                QString text = tr( "&%1 %2" )
                                   .arg( QChar( key ) )
                                   .arg( strippedName( recent_folders[ j ] ) );
                recentFolderActions[ actionIndex ]->setText( text );
                recentFolderActions[ actionIndex ]->setToolTip( recent_folders[ j ] );
                recentFolderActions[ actionIndex ]->setData( recent_folders[ j ] );
                recentFolderActions[ actionIndex ]->setVisible( true );
            }
            else {
                recentFolderActions[ actionIndex ]->setVisible( false );
            }
        }
    }
    else {
        recentFoldersMenu->setEnabled( false );
    }
}

// Clear the list of the recent folders
void MainWindow::clearRecentFolderActions()
{
    auto& recentFolders = RecentFolders::getSynced();
    recentFolders.removeAll();
    recentFolders.save();
    updateRecentFolderActions();
}
// Update our menu bar to match the settings of the crawler
// (used when the tab is changed)
void MainWindow::updateMenuBarFromDocument( const CrawlerWidget* crawler )
{
    // Defensive: a folder tab or no-tab state must never reach the body (it
    // derefs crawler->encodingMib() and queries Session). The folder/no-tab
    // branches in currentTabChanged handle their own menu state.
    if ( crawler == nullptr ) {
        return;
    }

    const auto encodingMib = crawler->encodingMib();
    const auto documentKind = session_.getDocumentKind( crawler );
    const auto associatedPath = session_.getAssociatedPath( crawler );
    const auto hasFilesystemPath = !associatedPath.isEmpty();
    const auto isFileDocument = documentKind == DocumentKind::File;
    const auto isLiveDocument = documentKind == DocumentKind::AdbLogcat;

    syncEncodingMenuCheck( encodingMib );

    followAction->setChecked( crawler->isFollowEnabled() );
    textWrapAction->setChecked( crawler->isTextWrapEnabled() );
    followAction->setEnabled( isLiveDocument || Configuration::get().anyFileWatchEnabled() );
    copyPathToClipboardAction->setEnabled( true );
    openContainingFolderAction->setEnabled( hasFilesystemPath );
    openInEditorAction->setEnabled( hasFilesystemPath );
    addToFavoritesAction->setEnabled( isFileDocument );
    addToFavoritesMenuAction->setEnabled( isFileDocument );
    saveCurrentLiveLogMenu->setEnabled( isLiveDocument );

    auto* adbSource = isLiveDocument ? session_.getAdbLogcatSource( crawler ) : nullptr;
    const auto sourceState
        = adbSource ? adbSource->state() : AdbLogcatSource::State::Disconnected;
    disconnectSourceAction->setEnabled( isLiveDocument
                                        && sourceState != AdbLogcatSource::State::Disconnected );
    reconnectSourceAction->setEnabled( isLiveDocument
                                       && sourceState != AdbLogcatSource::State::Connected );
}

void MainWindow::syncEncodingMenuCheck( const std::optional<int>& encodingMib )
{
    // The auto-detect action is the only one with an invalid QVariant data
    // (encodings.h); every specific encoding action carries its mib.
    auto encodingActions = encodingGroup->actions();
    const auto encodingItem = std::find_if(
        encodingActions.begin(), encodingActions.end(),
        [ &encodingMib ]( const auto& encodingAction ) {
            return ( !encodingMib && !encodingAction->data().isValid() )
                   || ( encodingMib && encodingAction->data().isValid()
                        && *encodingMib == encodingAction->data().toInt() );
        } );

    if ( encodingItem != encodingActions.end() ) {
        ( *encodingItem )->setChecked( true );
    }
}

void MainWindow::syncEncodingMenuFromDocument()
{
    const auto* document = currentDocument();
    syncEncodingMenuCheck( document != nullptr ? document->encodingMib() : std::nullopt );
}

void MainWindow::onFolderSearchablesChanged()
{
    auto* folderWidget = qobject_cast<FolderCrawlerWidget*>( sender() );
    // Only the CURRENT tab owns the mux: a background folder's pane change
    // must not steal it from whatever document is showing.
    if ( folderWidget == nullptr
         || dynamic_cast<AbstractCrawlerWidget*>( folderWidget ) != currentDocument() ) {
        return;
    }
    // Rebuild the mux's searchable registry from the folder's live panes.
    quickFindMux_.registerSelector( folderWidget );
}

// Update the top info line from the session
void MainWindow::updateInfoLine()
{
    // Don't overwrite the reconnect countdown display when on the countdown tab
    if ( reconnectCountdownTimer_ && reconnectCountdownTimer_->isActive()
         && currentCrawlerWidget() == reconnectCountdownCrawler_ ) {
        return;
    }

    QLocale defaultLocale;

    auto* crawler = currentCrawlerWidget();
    if ( crawler == nullptr ) {
        // Folder tab (or no tab): the info line reflects the file currently in
        // the folder main view, falling back to the folder path when none.
        auto* doc = currentDocument();
        if ( doc == nullptr ) {
            return;
        }
        const auto info = doc->currentMainViewInfo();
        if ( info.has_value() ) {
            const auto currentFile = QDir::toNativeSeparators( info->path );
            infoLine->setText( currentFile );
            infoLine->setPath( currentFile );
            sizeField->setText( readableSize( info->size ) );
            encodingField->setText( info->encodingText );
            if ( info->lastModified.isValid() ) {
                dateField->setText( tr( "modified on %1" )
                                        .arg( defaultLocale.toString( info->lastModified, QLocale::NarrowFormat ) ) );
                dateField->show();
            }
            else {
                dateField->hide();
            }
        }
        else {
            // No file in the main view: show the folder path.
            const auto folderPath = QDir::toNativeSeparators( session_.getAssociatedPath( doc ) );
            const auto display = session_.getDisplayName( doc );
            infoLine->setText( folderPath.isEmpty() ? display : folderPath );
            infoLine->setPath( folderPath );
            sizeField->clear();
            encodingField->clear();
            dateField->hide();
        }
        return;
    }

    const auto associatedPath = session_.getAssociatedPath( crawler );
    const auto currentFile = QDir::toNativeSeparators(
        associatedPath.isEmpty() ? session_.getDisplayName( crawler ) : associatedPath );

    uint64_t fileSize;
    uint64_t fileNbLine;
    QDateTime lastModified;

    session_.getFileInfo( crawler, &fileSize, &fileNbLine, &lastModified );

    infoLine->setText( currentFile );
    infoLine->setPath( currentFile );
    sizeField->setText( readableSize( fileSize ) );
    encodingField->setText( crawler->encodingText() );

    if ( lastModified.isValid() ) {
        const QString date = defaultLocale.toString( lastModified, QLocale::NarrowFormat );
        dateField->setText( tr( "modified on %1" ).arg( date ) );
        dateField->show();
    }
    else {
        dateField->hide();
    }
}

void MainWindow::updateOpenedFilesMenu()
{
    openedFilesMenu->clear();

    const auto files = session_.openedDocuments();

    openedFilesMenu->setEnabled( !files.empty() );

    openedFilesMenu->addAction( selectOpenFileAction );
    openedFilesMenu->addSeparator();

    for ( const auto& file : files ) {
        auto action = openedFilesMenu->addAction( file.displayName );

        action->setActionGroup( openedFilesGroup );
        action->setToolTip( file.toolTip );
        action->setData( file.documentId );
    }

    selectOpenFileAction->setDisabled( files.empty() );
}

void MainWindow::updateHighlightersMenu()
{
    highlightersMenu->clearHighlightersMenu();
    highlightersMenu->createHighlightersMenu();
    highlightersMenu->addAction( editHighlightersAction, true );
    highlightersMenu->populateHighlightersMenu();
}

void MainWindow::updateFavoritesMenu()
{
    favoritesMenu->clear();

    favoritesMenu->addAction( addToFavoritesMenuAction );
    favoritesMenu->addAction( removeFromFavoritesAction );

    addToFavoritesMenuAction->setIcon( iconLoader_.load( "icons8-star" ) );

    using namespace klogg::mainwindow;

    addToFavoritesAction->setText(
        QApplication::translate( "klogg::mainwindow::action", action::addToFavoritesText ) );
    addToFavoritesAction->setIcon( iconLoader_.load( "icons8-star" ) );
    addToFavoritesAction->setData( true );

    const auto& favorites = FavoriteFiles::getSynced().favorites();
    auto crawler = currentCrawlerWidget();
    const auto isFileDocument
        = crawler && session_.getDocumentKind( crawler ) == DocumentKind::File;

    addToFavoritesAction->setEnabled( isFileDocument );
    addToFavoritesMenuAction->setEnabled( isFileDocument );
    removeFromFavoritesAction->setEnabled( !favorites.empty() );

    if ( isFileDocument ) {
        const auto path = session_.getAssociatedPath( crawler );
        if ( std::any_of( favorites.begin(), favorites.end(), FullPathComparator( path ) ) ) {

            addToFavoritesAction->setText( QApplication::translate(
                "klogg::mainwindow::action", action::removeFromFavoritesText ) );
            addToFavoritesAction->setIcon( iconLoader_.load( "icons8-star-filled" ) );
            addToFavoritesAction->setData( false );

            addToFavoritesMenuAction->setEnabled( false );
            addToFavoritesMenuAction->setIcon( iconLoader_.load( "icons8-star-filled" ) );
        }
    }

    favoritesMenu->addSeparator();

    for ( const auto& file : favorites ) {
        auto action = favoritesMenu->addAction( file.displayName() );

        action->setActionGroup( favoritesGroup );
        action->setToolTip( file.nativeFullPath() );
        action->setData( file.fullPath() );
    }
}

void MainWindow::addToFavorites()
{
    if ( const auto crawler = currentCrawlerWidget();
         crawler != nullptr && session_.getDocumentKind( crawler ) == DocumentKind::File ) {
        auto& favorites = FavoriteFiles::get();
        const auto path = session_.getAssociatedPath( crawler );

        if ( addToFavoritesAction->data().toBool() ) {
            favorites.add( path );
        }
        else {
            favorites.remove( path );
        }

        favorites.save();

        updateFavoritesMenu();
    }
}

void MainWindow::removeFromFavorites()
{
    const auto& favoriteFiles = FavoriteFiles::get();
    const auto& favorites = favoriteFiles.favorites();
    QStringList files;
    std::transform( favorites.cbegin(), favorites.cend(), std::back_inserter( files ),
                    []( const auto& f ) { return f.nativeFullPath(); } );

    auto currentIndex = 0;

    if ( const auto crawler = currentCrawlerWidget();
         crawler != nullptr && session_.getDocumentKind( crawler ) == DocumentKind::File ) {
        const auto currentPath = session_.getAssociatedPath( crawler );
        const auto currentItem
            = std::find_if( favorites.begin(), favorites.end(), FullPathComparator( currentPath ) );
        if ( currentItem != favorites.end() ) {
            currentIndex = static_cast<int>( std::distance( favorites.begin(), currentItem ) );
        }
    }

    bool ok = false;
    const auto pathToRemove = QInputDialog::getItem( this, tr( "Remove from favorites" ),
                                                     tr( "Select item to remove from favorites" ),
                                                     files, currentIndex, false, &ok );
    if ( ok ) {
        removeFromFavorites( pathToRemove );
    }
}

void MainWindow::removeFromFavorites( const QString& pathToRemove )
{
    auto& favoriteFiles = FavoriteFiles::get();
    const auto& favorites = favoriteFiles.favorites();
    const auto selectedFile = std::find_if( favorites.begin(), favorites.end(),
                                            [ pathToRemove ]( const DisplayFilePath& f ) {
                                                return f.nativeFullPath() == pathToRemove;
                                            } );

    if ( selectedFile != favorites.end() ) {
        favoriteFiles.remove( selectedFile->fullPath() );
        favoriteFiles.save();
        updateFavoritesMenu();
    }
}

void MainWindow::removeFromRecent( const QString& pathToRemove )
{
    auto& recentFiles = RecentFiles::get();
    recentFiles.removeRecent( pathToRemove );
    recentFiles.save();
    updateRecentFileActions();
}

void MainWindow::selectOpenedFile()
{
    const auto openedDocuments = session_.openedDocuments();
    QStringList filesToShow;
    std::transform(
        openedDocuments.cbegin(), openedDocuments.cend(), std::back_inserter( filesToShow ),
        []( const auto& info ) {
            if ( info.toolTip.isEmpty() || info.toolTip == info.displayName ) {
                return info.displayName;
            }
            return QStringLiteral( "%1 (%2)" ).arg( info.displayName, info.toolTip );
        } );

    auto selectFileDialog = std::make_unique<QDialog>( this );
    selectFileDialog->setWindowTitle( tr( "klogg -- switch to file" ) );
    selectFileDialog->setMinimumWidth( 800 );
    selectFileDialog->setMinimumHeight( 600 );

    auto filesModel = std::make_unique<QStringListModel>( filesToShow, selectFileDialog.get() );
    auto filteredModel = std::make_unique<QSortFilterProxyModel>( selectFileDialog.get() );
    filteredModel->setSourceModel( filesModel.get() );

    auto filesView = std::make_unique<QListView>();
    filesView->setModel( filteredModel.get() );
    filesView->setEditTriggers( QAbstractItemView::NoEditTriggers );
    filesView->setSelectionMode( QAbstractItemView::SingleSelection );

    auto filterEdit = std::make_unique<QLineEdit>();
    auto buttonBox
        = std::make_unique<QDialogButtonBox>( QDialogButtonBox::Ok | QDialogButtonBox::Cancel );

    connect( buttonBox.get(), &QDialogButtonBox::accepted, selectFileDialog.get(),
             &QDialog::accept );
    connect( buttonBox.get(), &QDialogButtonBox::rejected, selectFileDialog.get(),
             &QDialog::reject );

    connect( filterEdit.get(), &QLineEdit::textEdited,
             [ model = filteredModel.get(), view = filesView.get() ]( const QString& filter ) {
                 model->setFilterWildcard( filter );
                 model->invalidate();
                 view->selectionModel()->select( model->index( 0, 0 ),
                                                 QItemSelectionModel::SelectCurrent );
             } );

    dispatchToMainThread( [ edit = filterEdit.get() ]() { edit->setFocus(); } );

    connect( selectFileDialog.get(), &QDialog::finished,
             [ this, openedDocuments, dialog = selectFileDialog.get(), model = filteredModel.get(),
               view = filesView.get() ]( auto result ) {
                 dialog->deleteLater();
                 if ( result != QDialog::Accepted || !view->selectionModel()->hasSelection() ) {
                     return;
                 }
                 const auto sourceIndex
                     = model->mapToSource( view->selectionModel()->selectedIndexes().front() );
                 if ( sourceIndex.isValid() && sourceIndex.row() >= 0
                      && sourceIndex.row() < klogg::isize( openedDocuments ) ) {
                     const auto& selectedDocument
                         = openedDocuments[ static_cast<size_t>( sourceIndex.row() ) ];
                     for ( int index = 0; index < mainTabWidget_.count(); ++index ) {
                         const auto* crawler
                             = qobject_cast<CrawlerWidget*>( mainTabWidget_.widget( index ) );
                         if ( crawler
                              && session_.getDocumentId( crawler ) == selectedDocument.documentId ) {
                             mainTabWidget_.setCurrentIndex( index );
                             activateWindow();
                             break;
                         }
                     }
                 }
             } );

    auto layout = std::make_unique<QVBoxLayout>();
    layout->addWidget( filesView.release() );
    layout->addWidget( filterEdit.release() );
    layout->addWidget( buttonBox.release() );

    selectFileDialog->setLayout( layout.release() );
    selectFileDialog->setModal( true );
    selectFileDialog->open();

    filesModel.release();
    filteredModel.release();
    selectFileDialog.release();
}

void MainWindow::showInfoLabels( bool show )
{
    for ( auto separator : infoToolbarSeparators ) {
        separator->setVisible( show );
    }
    if ( !show ) {
        sizeField->clear();
        dateField->clear();
        encodingField->clear();
        lineNbField->clear();
    }
}

// Write settings to permanent storage
void MainWindow::writeSettings()
{
    // Save the session
    // Generate the ordered list of widgets and their topLine
    std::vector<
        std::tuple<const ViewInterface*, uint64_t, std::shared_ptr<const ViewContextInterface>>>
        widget_list;
    for ( int i = 0; i < mainTabWidget_.count(); ++i ) {
        // dynamic_cast (not qobject_cast<CrawlerWidget*>) so that BOTH
        // CrawlerWidget and FolderCrawlerWidget tabs are captured: they
        // multiply-inherit ViewInterface, so the cross-cast succeeds for
        // both. qobject_cast<CrawlerWidget*> returned nullptr for folder
        // tabs and view->context() then dereferenced null -> crash on
        // persistSessionState (called from tab switch/close/periodic).
        const auto view = dynamic_cast<const ViewInterface*>( mainTabWidget_.widget( i ) );
        if ( view == nullptr ) {
            continue;
        }
        widget_list.emplace_back( view, 0UL, view->context() );
    }
    session_.save( widget_list, saveGeometry(), mainTabWidget_.currentIndex() );
}

void MainWindow::persistSessionState()
{
    sessionPersistenceTimer_.stop();
    if ( suspendSessionPersistence_ || shutdownInProgress_ ) {
        return;
    }

    writeSettings();
    TabGroupManager::get().save();
}

void MainWindow::scheduleSessionPersistence()
{
    if ( suspendSessionPersistence_ || shutdownInProgress_ ) {
        return;
    }
    // (Re)start the debounce: bursts of triggers (e.g. rapid tab switches)
    // coalesce into a single persistSessionState once activity settles.
    sessionPersistenceTimer_.start();
}

// Read settings from permanent storage
void MainWindow::readSettings()
{
    // Get and restore the session
    // auto& session = SessionInfo::getSynced();
    /*
     * FIXME: should be in the session
    crawlerWidget->restoreState( session.crawlerState() );
    */

    // History of recent files
    RecentFiles::getSynced();
    updateRecentFileActions();

    // History of recent folders
    RecentFolders::getSynced();
    updateRecentFolderActions();

    FavoriteFiles::getSynced();
    updateFavoritesMenu();

    HighlighterSetCollection::getSynced();
    updateHighlightersMenu();
}

void MainWindow::displayQuickFindBar( QuickFindMux::QFDirection direction )
{
    LOG_DEBUG << "MainWindow::displayQuickFindBar";

    // Warn the current document so it can save the position of the focus in
    // order to do incremental search in the right view (polymorphic: folder
    // tabs save/restore their view focus through the same virtuals).
    if ( auto* document = currentDocument() ) {
        document->enteringQuickFind();
    }

    const auto* document = currentDocument();
    if ( document != nullptr && document->isPartialSelection() ) {
        const auto selection = document->getSelectedText();
        if ( !selection.isEmpty() ) {
            quickFindWidget_.changeDisplayedPattern( selection, Configuration::get().qfIgnoreCase(), false, false );
        }
    }

    quickFindMux_.setDirection( direction );
    quickFindWidget_.userActivate();
}

void MainWindow::logScreenInfo( QScreen* screen )
{
    LOG_INFO << "screen changed for " << session_.windowIndex();
    if ( screen == nullptr ) {
        return;
    }

    LOG_INFO << "screen name " << screen->name();
    LOG_INFO << "screen size " << screen->size().width() << "x" << screen->size().height();
    LOG_INFO << "screen ratio " << screen->devicePixelRatio();
    LOG_INFO << "screen logical dpi " << screen->logicalDotsPerInch();
    LOG_INFO << "screen physical dpi " << screen->physicalDotsPerInch();
}

void MainWindow::generateDump()
{
    const auto userAction = QMessageBox::warning(
        this, tr( "klogg - generate crash dump" ),
        tr( "This will shutdown klogg and generate diagnostic crash dump. Continue?" ),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

    if ( userAction == QMessageBox::Yes ) {
        throw std::logic_error( "test dump" );
    }
}

std::vector<QString> MainWindow::showMergeFilesDialog( const QStringList& filePaths )
{
    // Show files in dictionary order (case-insensitive natural sort); see
    // mergefileorder.h. Display order is independent of the merge order, which
    // the user sets by checking boxes below.
    const std::vector<QString> sortedPaths = sortedMergeFilePaths( filePaths );

    QDialog dialog( this );
    dialog.setWindowTitle( tr( "Merge Files" ) );
    dialog.setMinimumWidth( 400 );

    auto* layout = new QVBoxLayout( &dialog );
    layout->addWidget( new QLabel( tr( "Check files in desired merge order:" ) ) );

    auto* listWidget = new QListWidget( &dialog );
    // Qt::UserRole = file path, Qt::UserRole+1 = original display name, Qt::UserRole+2 = check order
    int checkCounter = 0;

    for ( const auto& filePath : sortedPaths ) {
        const auto displayName = klogg::displayNameForPath( filePath );
        auto* item = new QListWidgetItem( displayName );
        item->setData( Qt::UserRole, filePath );
        item->setData( Qt::UserRole + 1, displayName );
        item->setData( Qt::UserRole + 2, 0 );
        item->setCheckState( Qt::Unchecked );
        item->setFlags( item->flags() | Qt::ItemIsUserCheckable );
        listWidget->addItem( item );
    }
    layout->addWidget( listWidget );

    // Lambda to update sequence numbers based on check order
    auto updateSequenceNumbers = [ listWidget ]() {
        // Collect checked items with their check order
        std::vector<std::pair<int, int>> checkedItems; // (check order, list index)
        for ( int i = 0; i < listWidget->count(); ++i ) {
            auto* item = listWidget->item( i );
            if ( item->checkState() == Qt::Checked ) {
                checkedItems.emplace_back( item->data( Qt::UserRole + 2 ).toInt(), i );
            }
        }
        std::sort( checkedItems.begin(), checkedItems.end() );

        listWidget->blockSignals( true );
        // Reset all to original names first
        for ( int i = 0; i < listWidget->count(); ++i ) {
            auto* item = listWidget->item( i );
            const auto originalName = item->data( Qt::UserRole + 1 ).toString();
            if ( item->checkState() != Qt::Checked ) {
                item->setText( originalName );
            }
        }
        // Number checked items by check order
        int seq = 1;
        for ( const auto& [ order, idx ] : checkedItems ) {
            auto* item = listWidget->item( idx );
            const auto originalName = item->data( Qt::UserRole + 1 ).toString();
            item->setText( QString( "%1. %2" ).arg( seq++ ).arg( originalName ) );
        }
        listWidget->blockSignals( false );
    };

    // checkCounter lives on the stack of showMergeFilesDialog; safe because
    // dialog.exec() blocks until the dialog closes, keeping it alive.
    connect( listWidget, &QListWidget::itemChanged,
             [ &checkCounter, listWidget, updateSequenceNumbers ]( QListWidgetItem* item ) {
                 // Block signals while mutating item data to prevent recursive
                 // itemChanged from corrupting checkCounter.
                 {
                     const QSignalBlocker blocker( listWidget );
                     if ( item->checkState() == Qt::Checked ) {
                         item->setData( Qt::UserRole + 2, ++checkCounter );
                     }
                     else {
                         // Uncheck: compact remaining order values so re-check gets the next slot
                         const int removedOrder = item->data( Qt::UserRole + 2 ).toInt();
                         item->setData( Qt::UserRole + 2, 0 );
                         for ( int i = 0; i < listWidget->count(); ++i ) {
                             auto* other = listWidget->item( i );
                             const int otherOrder = other->data( Qt::UserRole + 2 ).toInt();
                             if ( otherOrder > removedOrder ) {
                                 other->setData( Qt::UserRole + 2, otherOrder - 1 );
                             }
                         }
                         --checkCounter;
                     }
                 }
                 updateSequenceNumbers();
             } );

    auto* dialogButtons
        = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog );
    connect( dialogButtons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
    connect( dialogButtons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
    layout->addWidget( dialogButtons );

    if ( dialog.exec() != QDialog::Accepted ) {
        return {};
    }

    // Collect checked files sorted by check order
    std::vector<std::pair<int, QString>> checkedFiles;
    for ( int i = 0; i < listWidget->count(); ++i ) {
        auto* item = listWidget->item( i );
        if ( item->checkState() == Qt::Checked ) {
            checkedFiles.emplace_back( item->data( Qt::UserRole + 2 ).toInt(),
                                       item->data( Qt::UserRole ).toString() );
        }
    }
    std::sort( checkedFiles.begin(), checkedFiles.end() );

    std::vector<QString> result;
    result.reserve( checkedFiles.size() );
    for ( auto& [ order, path ] : checkedFiles ) {
        result.push_back( std::move( path ) );
    }

    if ( result.size() < 2 ) {
        QMessageBox::information( this, tr( "Merge Files" ),
                                  tr( "Please select at least 2 files to merge." ) );
        return {};
    }

    return result;
}

void MainWindow::mergeTabs()
{
    const int tabCount = mainTabWidget_.count();
    QStringList tabFiles;
    for ( int i = 0; i < tabCount; ++i ) {
        auto* crawler = dynamic_cast<CrawlerWidget*>( mainTabWidget_.widget( i ) );
        if ( !crawler || session_.getDocumentKind( crawler ) != DocumentKind::File ) {
            continue;
        }
        tabFiles.append( session_.getAssociatedPath( crawler ) );
    }

    if ( tabFiles.size() < 2 ) {
        QMessageBox::information( this, tr( "Merge Tabs" ),
                                  tr( "At least 2 file-backed tabs are required to merge." ) );
        return;
    }

    auto filesToMerge = showMergeFilesDialog( tabFiles );
    if ( !filesToMerge.empty() ) {
        executeMerge( filesToMerge );
    }
}

bool MainWindow::executeMerge( const std::vector<QString>& filesToMerge )
{
    try {
        auto* crawler_widget = static_cast<CrawlerWidget*>( session_.openMerged(
            filesToMerge, []() { return new CrawlerWidget(); }, tempDir_.path() ) );

        if ( !crawler_widget ) {
            QMessageBox::warning( this, tr( "Merge Files" ),
                                  tr( "Failed to create merged view." ) );
            return false;
        }

        crawler_widget->hide();

        const auto mergedFilePath = session_.getFilename( crawler_widget );
        int index = mainTabWidget_.addCrawler( crawler_widget, mergedFilePath, {}, mergedFilePath );

        QStringList shortNames;
        for ( const auto& fn : filesToMerge ) {
            shortNames << klogg::displayNameForPath( fn );
        }
        mainTabWidget_.setTabText( index,
                                   QString( "[Merged] %1" ).arg( shortNames.join( " + " ) ) );

        mainTabWidget_.setCurrentIndex( index );
        updateOpenedFilesMenu();
        return true;
    } catch ( const std::exception& e ) {
        LOG_ERROR << "Failed to merge files: " << e.what();
        QMessageBox::warning( this, tr( "Merge Files" ),
                              tr( "Failed to merge files: %1" ).arg( e.what() ) );
        return false;
    }
}
