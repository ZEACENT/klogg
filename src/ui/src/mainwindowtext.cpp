//
// Created by marvin on 23-4-26.
//

#include "mainwindowtext.h"

#include <QtGlobal>

using namespace klogg::mainwindow;

// menu
const char* menu::fileTitle = QT_TR_NOOP( "&File" );
const char* menu::recentFilesTitle = QT_TR_NOOP( "Open &Recent" );
const char* menu::recentFoldersTitle = QT_TR_NOOP( "Open Recent Fol&der" );
const char* menu::editTitle = QT_TR_NOOP( "&Edit" );
const char* menu::saveCurrentLiveLogTitle = QT_TR_NOOP( "Save Live Log As" );
const char* menu::viewTitle = QT_TR_NOOP( "&View" );
const char* menu::openedFilesTitle = QT_TR_NOOP( "Opened Files" );
const char* menu::toolsTitle = QT_TR_NOOP( "&Tools" );
const char* menu::highlightersTitle = QT_TR_NOOP( "Highlighters" );
const char* menu::favoritesTitle = QT_TR_NOOP( "F&avorite Files" );
const char* menu::helpTitle = QT_TR_NOOP( "&Help" );
const char* menu::encodingTitle = QT_TR_NOOP( "E&ncoding" );

// toolbar
const char* toolbar::toolbarTitle = QT_TR_NOOP( "&Toolbar" );

// trayicon
const char* trayicon::trayiconTip = QT_TR_NOOP( "klogg log viewer" );
const char* trayicon::openWindowText = QT_TR_NOOP( "Open Window" );
const char* trayicon::quitText = QT_TR_NOOP( "Quit" );

// action
const char* action::newWindowText = QT_TR_NOOP( "&New Window" );
const char* action::newWindowStatusTip = QT_TR_NOOP( "Create new klogg window" );
const char* action::openText = QT_TR_NOOP( "&Open..." );
const char* action::openStatusTip = QT_TR_NOOP( "Open a file" );
const char* action::openFolderText = QT_TR_NOOP( "Open Folder..." );
const char* action::openFolderStatusTip
    = QT_TR_NOOP( "Search every file in a folder (like grep -EIrn)" );
const char* action::openAdbLogcatText = QT_TR_NOOP( "Open ADB Logcat..." );
const char* action::openAdbLogcatStatusTip
    = QT_TR_NOOP( "Open Android logcat as a live source" );
const char* action::openIosLogStreamText = QT_TR_NOOP( "Open iOS Log Stream..." );
const char* action::openIosLogStreamStatusTip
    = QT_TR_NOOP( "Open iOS device logs as a live source" );
const char* action::recentFilesCleanupText = QT_TR_NOOP( "Clear List" );
const char* action::recentFoldersCleanupText = QT_TR_NOOP( "Clear List" );
const char* action::closeText = QT_TR_NOOP( "&Close" );
const char* action::closeStatusTip = QT_TR_NOOP( "Close document" );
const char* action::closeAllText = QT_TR_NOOP( "Close &All" );
const char* action::closeAllStatusTip = QT_TR_NOOP( "Close all documents" );
const char* action::exitText = QT_TR_NOOP( "E&xit" );
const char* action::exitStatusTip = QT_TR_NOOP( "Exit the application" );
const char* action::copyText = QT_TR_NOOP( "&Copy" );
const char* action::copyStatusTip = QT_TR_NOOP( "Copy the selection" );
const char* action::selectAllText = QT_TR_NOOP( "Select &All" );
const char* action::selectAllStatusTip = QT_TR_NOOP( "Select all the text" );
const char* action::goToLineText = QT_TR_NOOP( "Go to Line..." );
const char* action::goToLineStatusTip
    = QT_TR_NOOP( "Scrolls selected main view to specified line" );
const char* action::findText = QT_TR_NOOP( "&Find..." );
const char* action::findStatusTip = QT_TR_NOOP( "Find the text" );
const char* action::clearLogText = QT_TR_NOOP( "Clear File..." );
const char* action::clearLogStatusTip = QT_TR_NOOP( "Clear current file" );
const char* action::saveCurrentLiveLogStripAnsiText
    = QT_TR_NOOP( "Without ANSI Sequences..." );
const char* action::saveCurrentLiveLogStripAnsiStatusTip
    = QT_TR_NOOP( "Persist the current live capture to a file after removing ANSI sequences" );
const char* action::saveCurrentLiveLogPreserveAnsiText
    = QT_TR_NOOP( "With ANSI Sequences..." );
const char* action::saveCurrentLiveLogPreserveAnsiStatusTip
    = QT_TR_NOOP( "Persist the current live capture to a file without modifying ANSI sequences" );
const char* action::disconnectSourceText = QT_TR_NOOP( "Disconnect Source" );
const char* action::disconnectSourceStatusTip
    = QT_TR_NOOP( "Stop streaming from the current live source" );
const char* action::reconnectSourceText = QT_TR_NOOP( "Reconnect Source" );
const char* action::reconnectSourceStatusTip = QT_TR_NOOP( "Reconnect the current live source" );
const char* action::openContainingFolderText = QT_TR_NOOP( "Open Containing Folder" );
const char* action::openContainingFolderStatusTip
    = QT_TR_NOOP( "Open folder containing current file" );
const char* action::openInEditorText = QT_TR_NOOP( "Open in Editor" );
const char* action::openInEditorStatusTip = QT_TR_NOOP( "Open current file in default editor" );
const char* action::copyPathToClipboardText = QT_TR_NOOP( "Copy Full Path" );
const char* action::copyPathToClipboardStatusTip
    = QT_TR_NOOP( "Copy full path for file to clipboard" );
const char* action::openClipboardText = QT_TR_NOOP( "Open from Clipboard" );
const char* action::openClipboardStatusTip = QT_TR_NOOP( "Open clipboard as log file" );
const char* action::openUrlText = QT_TR_NOOP( "Open from URL..." );
const char* action::openUrlStatusTip = QT_TR_NOOP( "Open URL as log file" );
const char* action::overviewVisibleText = QT_TR_NOOP( "Matches &Overview" );
const char* action::lineNumbersVisibleInMainText = QT_TR_NOOP( "Line &Numbers in Main View" );
const char* action::lineNumbersVisibleInFilteredText
    = QT_TR_NOOP( "Line &Numbers in Filtered View" );
const char* action::followText = QT_TR_NOOP( "&Follow File" );
const char* action::goToTopText = QT_TR_NOOP( "Go to &Top" );
const char* action::wrapText = QT_TR_NOOP( "&Wrap Text" );
const char* action::reloadText = QT_TR_NOOP( "&Reload" );
const char* action::stopText = QT_TR_NOOP( "&Stop" );
const char* action::optionsText = QT_TR_NOOP( "&Preferences..." );
const char* action::optionsStatusTip = QT_TR_NOOP( "Show application settings dialog" );
const char* action::editHighlightersText = QT_TR_NOOP( "Configure &Highlighters..." );
const char* action::editHighlightersStatusTip = QT_TR_NOOP( "Show highlighters configuration" );
const char* action::showDocumentationText = QT_TR_NOOP( "&Documentation" );
const char* action::showDocumentationStatusTip = QT_TR_NOOP( "Show documentation" );
const char* action::aboutText = QT_TR_NOOP( "&About" );
const char* action::aboutStatusTip = QT_TR_NOOP( "Show the About box" );
const char* action::aboutQtText = QT_TR_NOOP( "About &Qt" );
const char* action::aboutQtStatusTip = QT_TR_NOOP( "Show the Qt library's About box" );
const char* action::reportIssueText = QT_TR_NOOP( "Report Issue" );
const char* action::reportIssueStatusTip = QT_TR_NOOP( "Report an issue on GitHub" );
const char* action::generateDumpText = QT_TR_NOOP( "Generate Crash Dump" );
const char* action::generateDumpStatusTip = QT_TR_NOOP( "Generate diagnostic crash dump" );
const char* action::checkForNewVersionText = QT_TR_NOOP( "Check for New Version" );
const char* action::checkForNewVersionStatusTip = QT_TR_NOOP( "Check for new version of klogg" );
const char* action::showScratchPadText = QT_TR_NOOP( "Scratchpad" );
const char* action::showScratchPadStatusTip = QT_TR_NOOP( "Show the scratchpad" );
const char* action::addToFavoritesText = QT_TR_NOOP( "Add Current File to Favorites" );
const char* action::removeFromFavoritesText
    = QT_TR_NOOP( "Remove Current File from Favorites" );
const char* action::addCurrentFileText = QT_TR_NOOP( "Add Current File" );
const char* action::removeFavoriteFileText = QT_TR_NOOP( "Remove Favorite File..." );
const char* action::selectOpenFileText = QT_TR_NOOP( "Switch to Opened File..." );
const char* action::predefinedFiltersDialogText = QT_TR_NOOP( "Filter Favorites..." );
const char* action::predefinedFiltersDialogStatusTip
    = QT_TR_NOOP( "Show dialog to manage filter favorites" );
const char* action::importFilterFavoritesText = QT_TR_NOOP( "Import Filter Favorites..." );
const char* action::importFilterFavoritesStatusTip
    = QT_TR_NOOP( "Import filter favorites from a file" );
const char* action::exportFilterFavoritesText = QT_TR_NOOP( "Export Filter Favorites..." );
const char* action::exportFilterFavoritesStatusTip
    = QT_TR_NOOP( "Export filter favorites to a file" );
const char* action::mergeTabsText = QT_TR_NOOP( "Merge Tabs..." );
const char* action::mergeTabsStatusTip = QT_TR_NOOP( "Merge selected tabs into a single view" );
const char* action::autoEncodingText = QT_TR_NOOP( "Auto" );
const char* action::autoEncodingStatusTip
    = QT_TR_NOOP( "Automatically detect the file's encoding" );
