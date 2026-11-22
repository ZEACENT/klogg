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

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QList>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QTimer>

#include <QTranslator>
#include <QUuid>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "configuration.h"
#include "crawlerwidget.h"
#include "downloader.h"
#include "iconloader.h"
#include "livelogclosetransaction.h"
#include "pathline.h"
#include "quickfindmux.h"
#include "quickfindwidget.h"
#include "session.h"
#include "signalmux.h"
#include "tabbedcrawlerwidget.h"
#include "tabbedscratchpad.h"

class QAction;
class QActionGroup;
class AdbLiveServices;
namespace klogg::livecapture::ios {
class IosLiveServices;
}
class Session;
class RecentFiles;
class HighlightersMenu;
enum class LiveLogSaveAnsiMode;

// Main window of the application, creates menus, toolbar and
// the CrawlerWidget
class MainWindow : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow( WindowSession session );
    MainWindow( WindowSession session, AdbLiveServices& adbLiveServices );
    MainWindow( WindowSession session, AdbLiveServices& adbLiveServices,
                klogg::livecapture::ios::IosLiveServices& iosLiveServices );

    const klogg::livecapture::ios::IosLiveServices* iosLiveServices() const noexcept
    {
        return iosLiveServices_;
    }

    struct TabInventoryEntry {
        QString tabId;
        DocumentKind kind = DocumentKind::File;
        QString documentId;
        QString displayName;
        QString associatedPath;
        QString tabTitle;
        QPointer<QWidget> widget;

        bool operator==( const TabInventoryEntry& other ) const;
    };

    struct InventorySnapshot {
        QString windowId;
        uint64_t revision = 0;
        QString currentTabId; // Empty when no document tab is current.
        std::vector<TabInventoryEntry> tabs;
    };

    QString runtimeWindowId() const { return runtimeWindowId_; }
    // GUI-owner-thread observation in actual widget order, including collapsed
    // group members. displayName is document metadata; tabTitle is the rendered
    // label (including custom names/group labels). Weak widgets are not leases:
    // callers must revalidate current membership before later operations.
    // Revision covers membership/order/current tab and the exposed metadata,
    // not pane/source/result state. Legacy compound actions may advance it more
    // than once; this does not claim workspace atomicity. Reads have no effects.
    InventorySnapshot inventorySnapshot() const;

    // Native membership publication, not rollback or a complete transaction.
    // Without yielding: install/extract all owners, finalize every inventory,
    // release all barriers, then notify. Callers own policy, global groups,
    // history, source activation and retirement. Destruction only releases the
    // facade barrier; it does not finalize, notify or undo installed membership.
    class Publication {
      public:
        ~Publication();
        Publication( const Publication& ) = delete;
        Publication& operator=( const Publication& ) = delete;
        bool install( std::unique_ptr<Session::PreparedDocument>& document );
        std::unique_ptr<Session::PreparedDocument> extract( AbstractCrawlerWidget* document );
        void finalize();
        void release();
        void notify();
      private:
        friend class MainWindow;
        Publication( MainWindow& owner, std::unique_ptr<TabbedCrawlerWidget::Publication> tabs );
        struct State;
        std::unique_ptr<State> state_;
    };
    std::unique_ptr<Publication> beginPublication();
    std::shared_ptr<Session> sessionOwner() const { return session_.owner(); }
    std::unique_ptr<Session::PreparedDocument> prepareFile(
        std::unique_ptr<PreparedFileSource>& source, bool followFile = false );
    std::unique_ptr<Session::PreparedDocument> prepareFolder(
        const QString& path, const std::vector<QString>& files );
    void recordPublishedDocument( const QString& path, DocumentKind kind, bool opened );

    // Re-install the geometry stored in config file
    // (should be done before 'Widget::show()')
    void reloadGeometry();
    // Re-load the files from the previous session
    void reloadSession();
    // Loads the initial file (parameter passed or from config file)
    void loadInitialFile( QString fileName, bool followFile );

    // Opens a folder as a crawler tab (the shared body of openFolder() and the
    // directory-drop path in dropEvent). Safe to call on any path:
    // enumerateFolderFiles returns {} for non-dir / empty input.
    // Must be called on the GUI thread (shows a QMessageBox on empty folders
    // and interacts with the tab widget).
    void openFolderByPath( const QString& folderPath );

    void reTranslateUI();

    static int installLanguage( QString lang );

  public Q_SLOTS:
    // Load a file in a new tab (non-interactive)
    // (for use from e.g. IPC)
    void loadFileNonInteractive( const QString& file_name );

  protected:
    void closeEvent( QCloseEvent* event ) override;
    void changeEvent( QEvent* event ) override;

    // Drag and drop support
    void dragEnterEvent( QDragEnterEvent* event ) override;
    void dropEvent( QDropEvent* event ) override;

    bool event( QEvent* event ) override;

  private:
    friend struct MainWindowLiveSaveTestAccess;
    static QString previousFileContext( const QString& fileName );

    enum class ActionInitiator : std::uint8_t { User, WindowDiscard, App };
    enum class CloseRequestScope : std::uint8_t { Single, Batch };
    enum class DiscardCommit : std::uint8_t { PerTab, WindowShutdown };

  private Q_SLOTS:
    void open();
    void openFolder();
    void openAdbLogcat();
    void openIosLogStream();
    void openFileFromRecent( QAction* action );
    void openFolderFromRecent( QAction* action );
    void openFileFromFavorites( QAction* action );
    void switchToOpenedFile( QAction* action );
    void closeTab( ActionInitiator initiator );
    void closeAll( ActionInitiator initiator );
    void selectAll();
    void selectAllMatches( bool copy = false );
    void copy();
    void find();
    void clearLog();
    void disconnectCurrentSource();
    void reconnectCurrentSource();
    void copyFullPath();
    void openContainingFolder();
    void openInEditor();
    void openClipboard();
    void openUrl();
    void editHighlighters();
    void editPredefinedFilters( const QString& newFilter = {} );
    void importFilterFavorites();
    void importFilterFavoritesFromFile( const QString& file );
    void exportFilterFavorites();
    void exportFilterFavoritesToFile( QString file );
    void mergeTabs();
    std::vector<QString> showMergeFilesDialog( const QStringList& filePaths );
    bool executeMerge( const std::vector<QString>& filesToMerge );
    void options();
    void about();
    void aboutQt();
    void documentation();
    void showScratchPad();
    void sendToScratchpad( QString );
    void replaceDataInScratchpad( QString );
    void encodingChanged( QAction* action );
    void addToFavorites();
    void removeFromFavorites();
    void selectOpenedFile();
    void generateDump();
    void checkForNewVersion();

    // Change the view settings
    void toggleOverviewVisibility( bool isVisible );
    void toggleMainLineNumbersVisibility( bool isVisible );
    void toggleFilteredLineNumbersVisibility( bool isVisible );

    // Change the follow mode checkbox (the views report follow-state changes
    // up; the action's toggled signal carries followSet down to the document)
    void changeFollowMode( bool follow );

    // Update the selection information displayed in the status bar.
    // Must be passed as the internal (starts at 0) line number.
    void lineNumberHandler( LineNumber startLine, LinesCount nLines, LineColumn startCol,
                            LineLength nSymbols );
    void refreshLineNumberField();
    // Routes a folder main view's newSelection to lineNumberHandler, but only
    // when a folder tab is current (a backgrounded folder's selection must not
    // overwrite the current tab's status bar). A real slot (not a lambda) so the
    // Qt::UniqueConnection in currentTabChanged can dedupe across tab switches.
    void onFolderMainViewNewSelection( LineNumber startLine, LinesCount nLines,
                                       LineColumn startCol, LineLength nSymbols );

    // Instructs the widget to update the loading progress gauge
    void updateLoadingProgress( int progress );
    // Instructs the widget to display the 'normal' status bar,
    // without the progress gauge and with file info
    // or an error recovery when loading is finished
    void handleLoadingFinished( LoadingStatus status );

    // Update quick find searchable
    void handleFilteredViewChanged();

    // Close the tab with the passed index
    void closeTab( int index, ActionInitiator initiator,
                   CloseRequestScope scope = CloseRequestScope::Single );
    // Setup the tab with current index for view
    void currentTabChanged( int index );

    // Instructs the widget to change the pattern in the QuickFind widget
    // and confirm it.
    void changeQFPattern( const QString& newPattern, bool ignoreCase, bool isRegex, bool isWholeWord );

  Q_SIGNALS:
    // Is emitted when new settings must be used
    void optionsChanged();

    void newWindow();
    void windowActivated();
    void windowClosed();
    void exitRequested();
    void checkForNewVersionRequested();

  private:
    MainWindow( WindowSession session, AdbLiveServices* adbLiveServices,
                klogg::livecapture::ios::IosLiveServices* iosLiveServices = nullptr );
    void createActions();
    void loadIcons();
    void createMenus();
    void createToolBars();
    void createTrayIcon();
    void readSettings();
    void writeSettings();
    bool loadFile( const QString& fileName, bool followFile = false );
    bool openAdbLogcatSource( const AdbLogcatSessionData& sessionData,
                              bool startConnected = true );
    bool extractAndLoadFile( const QString& fileName );
    void openRemoteFile( const QUrl& url );
    void updateTitleBar( const QString& fileName );
    void syncCrawlerPresentationActivity();
    void addRecentFile( const QString& fileName );
    void addRecentFolder( const QString& folderPath );
    void updateRecentFileActions();
    void clearRecentFileActions();
    void updateRecentFolderActions();
    void clearRecentFolderActions();
    void updateFavoritesMenu();
    void updateOpenedFilesMenu();
    void updateHighlightersMenu();
    QString strippedName( const QString& fullFileName ) const;
    CrawlerWidget* currentCrawlerWidget() const;
    // The AbstractCrawlerWidget* cross-cast of the current tab -- the polymorphic
    // dispatch target for copy/selectAll (succeeds for both CrawlerWidget and
    // FolderCrawlerWidget). Replaces currentCrawlerWidget()-only dispatch that
    // was a silent no-op on folder tabs.
    AbstractCrawlerWidget* currentDocument() const;
    // True if the tab at `index` is a FolderCrawlerWidget (not a CrawlerWidget).
    // Used at explicit folder branch points (currentTabChanged, closeTab).
    bool isFolderTab( int index ) const;
    // The ViewInterface cross-cast of the current tab widget. Succeeds for BOTH
    // CrawlerWidget and FolderCrawlerWidget (both implement ViewInterface);
    // returns nullptr only when there is no current tab. Required because
    // Session accessors (getDisplayName/getDocumentKind/...) key on a
    // ViewInterface* and assert on nullptr -- a folder tab's display name must
    // be fetched via this cross-cast, never via a nullptr CrawlerWidget*.
    const ViewInterface* currentView() const;
    void displayQuickFindBar( QuickFindMux::QFDirection direction );
    void updateMenuBarFromDocument( const CrawlerWidget* crawler );
    void updateInfoLine();
    // Checks the encoding-menu action matching the given override (nullopt =
    // auto-detect, the action with invalid QVariant data). Shared by the file
    // and folder menu-state sync paths.
    void syncEncodingMenuCheck( const std::optional<int>& encodingMib );
    // Re-checks the encoding menu from the current document's override. Called
    // when the folder main-view file changes (the override resets on switch).
    void syncEncodingMenuFromDocument();
    // Re-registers the folder tab's QuickFind selector with the mux when the
    // folder's searchable set changes (pane create/switch/close).
    void onFolderSearchablesChanged();
    // Currency-guarded uplink for a folder tab's followModeChanged: only the
    // CURRENT tab may drive the shared followAction (a hidden folder still
    // emits when its async main-view open completes in the background).
    void onFolderFollowModeChanged( bool follow );
    // Disable every file-specific menu action (used for folder / no-tab states).
    void disableFileSpecificActions();
    void showInfoLabels( bool show );
    void logScreenInfo( QScreen* screen );
    void removeFromFavorites( const QString& pathToRemove );
    void removeFromRecent( const QString& pathToRemove );
    void tryOpenClipboard( int tryTimes );
    void updateShortcuts();
    void persistSessionState();
    // Coalescing variant: frequent triggers (tab switches, tab reorder, group
    // changes) restart a short debounce timer instead of synchronously
    // rewriting the session + syncing QSettings on the spot (each sync is a
    // CFPreferencesSynchronize XPC on macOS and can stall the UI for hundreds
    // of ms under daemon contention). closeEvent still flushes synchronously.
    void scheduleSessionPersistence();
    void registerAdbLogcatSource( CrawlerWidget* crawler );
    void updateLiveTabAppearance( CrawlerWidget* crawler );
    void saveCurrentLiveLog( LiveLogSaveAnsiMode ansiMode );
    QString selectLiveLogOutputPath( CrawlerWidget* crawler );
    void startLiveLogExport( CrawlerWidget* crawler, const QString& outputPath,
                             LiveLogSaveAnsiMode ansiMode );
    void startLiveCloseTransaction( CrawlerWidget* crawler,
                                    klogg::livelog::LiveLogCloseTransaction::Mode mode,
                                    DiscardCommit discardCommit,
                                    std::function<void( bool )> completion,
                                    bool appSingleRequest = false );
    void showLiveCloseFailureDialog(
        const klogg::livelog::LiveLogCloseTransaction::Failure& failure );
    void finalizeCrawlerClose( CrawlerWidget* widget, ActionInitiator initiator );
    void closeTabs( const QList<QWidget*>& tabs );
    void continueTabCloseBatch();
    void cancelTabCloseBatch();
    void resumeDeferredWindowClose();
    void retainAppTabClose( QWidget* tab );
    void schedulePendingAppTabCloses();
    void beginWindowShutdown();
    void advanceWindowShutdown();
    void abortWindowShutdown();
    void finalizeWindowShutdown();

    InventorySnapshot readInventory() const;
    void refreshInventory();

    const QString runtimeWindowId_ = QUuid::createUuid().toString( QUuid::WithoutBraces );
    // Observation cache only: widgets and per-view Session metadata remain authoritative.
    InventorySnapshot inventory_{ runtimeWindowId_, 0, {}, {} };
    WindowSession session_;
    AdbLiveServices* adbLiveServices_{ nullptr };
    klogg::livecapture::ios::IosLiveServices* iosLiveServices_{ nullptr };
    QString loadingFileName;

    std::array<QAction*, MAX_RECENT_FILES> recentFileActions;
    QActionGroup* recentFilesGroup;

    std::array<QAction*, MAX_RECENT_FILES> recentFolderActions;
    QActionGroup* recentFoldersGroup;

    QMenu* fileMenu;
    QMenu* saveCurrentLiveLogMenu;
    QMenu* recentFilesMenu;
    QMenu* recentFoldersMenu;
    QMenu* editMenu;
    QMenu* viewMenu;
    QMenu* toolsMenu;
    QMenu* favoritesMenu;
    HighlightersMenu* highlightersMenu;
    QMenu* openedFilesMenu;
    QMenu* helpMenu;

    PathLine* infoLine;
    QLabel* lineNbField;
    QLabel* sizeField;
    QLabel* dateField;

    // Last known selection for refreshing line number field
    LineNumber lastStartLine_{ 0 };
    LinesCount lastNLines_{ 0 };
    LineColumn lastStartCol_{ 0 };
    LineLength lastNSymbols_{ 0 };
    QLabel* encodingField;
    std::vector<QAction*> infoToolbarSeparators;

    QToolBar* toolBar;

    QAction* newWindowAction;
    QAction* openAction;
    QAction* openFolderAction;
    QAction* openAdbLogcatAction;
    QAction* openIosLogStreamAction;
    QAction* closeAction;
    QAction* closeAllAction;
    QAction* exitAction;
    QAction* copyAction;
    QAction* selectAllAction;
    QAction* selectAllMatchesAction;
    QAction* copyMatchesAction;
    QAction* goToLineAction;
    QAction* findAction;
    QAction* clearLogAction;
    QAction* saveCurrentLiveLogStripAnsiAction;
    QAction* saveCurrentLiveLogPreserveAnsiAction;
    QAction* disconnectSourceAction;
    QAction* reconnectSourceAction;
    QAction* copyPathToClipboardAction;
    QAction* openContainingFolderAction;
    QAction* openInEditorAction;
    QAction* openClipboardAction;
    QAction* openUrlAction;
    QAction* overviewVisibleAction;
    QAction* lineNumbersVisibleInMainAction;
    QAction* lineNumbersVisibleInFilteredAction;
    QAction* followAction;
    QAction* goToTopAction;
    QAction* textWrapAction;
    QAction* reloadAction;
    QAction* stopAction;
    QAction* editHighlightersAction;
    QAction* optionsAction;
    QAction* showScratchPadAction;
    QAction* showDocumentationAction;
    QAction* aboutAction;
    QAction* aboutQtAction;
    QAction* predefinedFiltersDialogAction;
    QAction* importFilterFavoritesAction;
    QAction* exportFilterFavoritesAction;
    QAction* reportIssueAction;
    QAction* generateDumpAction;
    QAction* checkForNewVersionAction;
    QAction* mergeTabsAction;
    QActionGroup* encodingGroup;
    QAction* addToFavoritesAction;
    QAction* addToFavoritesMenuAction;
    QAction* removeFromFavoritesAction;
    QAction* selectOpenFileAction;
    QAction* recentFilesCleanup;
    QAction* recentFoldersCleanup;
    QActionGroup* favoritesGroup;
    QActionGroup* openedFilesGroup;
    QActionGroup* highlightersActionGroup = nullptr;

    std::map<QString, QShortcut*> shortcuts_;

    QSystemTrayIcon* trayIcon_;
    QAction* trayOpenWindowAction_;
    QAction* trayQuitAction_;

    QIcon mainIcon_;

    IconLoader iconLoader_;

    // Multiplex signals to any of the CrawlerWidgets
    SignalMux signalMux_;

    static QTranslator mTranslator;
    static QTranslator mQtTranslator;

    // QuickFind widget
    QuickFindWidget quickFindWidget_;

    // Multiplex signals to/from the QuickFindWidget
    QuickFindMux quickFindMux_;

    // The main widget
    TabbedCrawlerWidget mainTabWidget_;

    TabbedScratchPad scratchPad_;

    QTemporaryDir tempDir_;

    bool isMaximized_ = false;
    bool isCloseFromTray_ = false;
    bool suspendSessionPersistence_ = false;
    bool shutdownInProgress_ = false;
    bool shutdownReadyToAccept_ = false;
    bool shutdownPreserveWindowSession_ = false;
    std::optional<WindowSession::CloseDisposition> shutdownCloseDisposition_;
    bool tabCloseBatchInProgress_ = false;
    ActionInitiator tabCloseBatchInitiator_{ ActionInitiator::User };
    std::optional<QList<QPointer<QWidget>>> pendingTabCloseTargets_;
    QList<QPointer<QWidget>> pendingAppTabCloseTargets_;
    bool deferredWindowClose_ = false;
    std::unique_ptr<klogg::livelog::LiveLogCloseTransaction> liveCloseTransaction_;
    QPointer<QWidget> activeAppTabCloseTarget_;
    std::vector<CrawlerWidget*> shutdownLiveTabs_;
    std::vector<CrawlerWidget*> shutdownResumeTabs_;
    std::size_t shutdownLiveTabIndex_ = 0;

    // Debounce timer for session persistence (see scheduleSessionPersistence).
    QTimer sessionPersistenceTimer_;

    std::once_flag screenChangesConnect_;
};

#endif
