/*
 * Copyright (C) 2009, 2010, 2011, 2013, 2015 Nicolas Bonnefon and other
 * contributors
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

#ifndef KLOGG_CONFIGURATION_H
#define KLOGG_CONFIGURATION_H

#include <QColor>
#include <QFont>
#include <QSettings>
#include <qcolor.h>
#include <string>
#include <string_view>

#include "persistable.h"

// Type of regexp to use for searches
enum class SearchRegexpType {
    ExtendedRegexp,
    Wildcard,
    FixedString,
};

enum class RegexpEngine { Vectorscan, QRegularExpression };

enum class ThemeMode { Light, Dark, Auto };
static constexpr int MAX_RECENT_FILES = 25;

// Configuration class containing everything in the "Settings" dialog
class Configuration final : public Persistable<Configuration> {
  public:
    static constexpr int MinLineSpacingPercent = 100;
    static constexpr int DefaultLineSpacingPercent = 120;
    static constexpr int MaxLineSpacingPercent = 200;

    static const char* persistableName()
    {
        return "Configuration";
    }
    Configuration();

    // Accesses the main font used for display
    QFont mainFont() const;
    void setMainFont( QFont newFont );
    int lineSpacingPercent() const
    {
        return lineSpacingPercent_;
    }
    void setLineSpacingPercent( int percent );

    QString language() const
    {
        return language_;
    }

    void setLanguage( QString lang )
    {
        language_ = lang;
    }

    // Accesses the regexp types
    SearchRegexpType mainRegexpType() const
    {
        return mainRegexpType_;
    }
    SearchRegexpType quickfindRegexpType() const
    {
        return quickfindRegexpType_;
    }
    bool isQuickfindIncremental() const
    {
        return quickfindIncremental_;
    }
    void setMainRegexpType( SearchRegexpType type )
    {
        mainRegexpType_ = type;
    }
    void setQuickfindRegexpType( SearchRegexpType type )
    {
        quickfindRegexpType_ = type;
    }
    void setQuickfindIncremental( bool isIncremental )
    {
        quickfindIncremental_ = isIncremental;
    }

    // "Advanced" settings
    bool anyFileWatchEnabled() const
    {
        return nativeFileWatchEnabled() || pollingEnabled();
    }

    bool nativeFileWatchEnabled() const
    {
        return nativeFileWatchEnabled_;
    }
    void setNativeFileWatchEnabled( bool enabled )
    {
        nativeFileWatchEnabled_ = enabled;
    }
    bool pollingEnabled() const
    {
        return pollingEnabled_;
    }
    void setPollingEnabled( bool enabled )
    {
        pollingEnabled_ = enabled;
    }
    int pollIntervalMs() const
    {
        return pollIntervalMs_;
    }
    void setPollIntervalMs( int interval )
    {
        pollIntervalMs_ = interval;
    }

    bool fastModificationDetection() const
    {
        return fastModificationDetection_;
    }

    void setFastModificationDetection( bool fastDetection )
    {
        fastModificationDetection_ = fastDetection;
    }

    bool loadLastSession() const
    {
        return loadLastSession_;
    }
    void setLoadLastSession( bool enabled )
    {
        loadLastSession_ = enabled;
    }
    bool followFileOnLoad() const
    {
        return followFileOnLoad_;
    }
    void setFollowFileOnLoad( bool enabled )
    {
        followFileOnLoad_ = enabled;
    }
    bool allowMultipleWindows() const
    {
        return allowMultipleWindows_;
    }
    void setAllowMultipleWindows( bool enabled )
    {
        allowMultipleWindows_ = enabled;
    }

    // perf settings
    bool useParallelSearch() const
    {
        return useParallelSearch_;
    }
    void setUseParallelSearch( bool enabled )
    {
        useParallelSearch_ = enabled;
    }
    bool useBlockScan() const
    {
        return useBlockScan_;
    }
    void setUseBlockScan( bool enabled )
    {
        useBlockScan_ = enabled;
    }
    bool useSearchResultsCache() const
    {
        return useSearchResultsCache_;
    }
    void setUseSearchResultsCache( bool enabled )
    {
        useSearchResultsCache_ = enabled;
    }
    unsigned searchResultsCacheLines() const
    {
        return searchResultsCacheLines_;
    }
    void setSearchResultsCacheLines( unsigned lines )
    {
        searchResultsCacheLines_ = lines;
    }
    int indexReadBufferSizeMb() const
    {
        return indexReadBufferSizeMb_;
    }
    void setIndexReadBufferSizeMb( int bufferSizeMb )
    {
        indexReadBufferSizeMb_ = bufferSizeMb;
    }
    int searchReadBufferSizeLines() const
    {
        return searchReadBufferSizeLines_;
    }
    void setSearchReadBufferSizeLines( int lines )
    {
        searchReadBufferSizeLines_ = lines;
    }
    int searchThreadPoolSize() const
    {
        return searchThreadPoolSize_;
    }
    void setSearchThreadPoolSize( int threads )
    {
        searchThreadPoolSize_ = threads;
    }
    bool keepFileClosed() const
    {
        return keepFileClosed_;
    }
    void setKeepFileClosed( bool shouldKeepClosed )
    {
        keepFileClosed_ = shouldKeepClosed;
    }
    bool useCompressedIndex() const
    {
        return useCompressedIndex_;
    }
    void setUseCompressedIndex( bool useCompressedIndex )
    {
        useCompressedIndex_ = useCompressedIndex;
    }

    RegexpEngine regexpEngine() const
    {
        return regexpEngine_;
    }

    void setRegexpEnging( RegexpEngine engine )
    {
        regexpEngine_ = engine;
    }

    // Accessors
    bool versionCheckingEnabled() const
    {
        return enableVersionChecking_;
    }
    void setVersionCheckingEnabled( bool enabled )
    {
        enableVersionChecking_ = enabled;
    }

    // View settings
    bool isOverviewVisible() const
    {
        return overviewVisible_;
    }
    void setOverviewVisible( bool isVisible )
    {
        overviewVisible_ = isVisible;
    }
    bool mainLineNumbersVisible() const
    {
        return lineNumbersVisibleInMain_;
    }
    bool filteredLineNumbersVisible() const
    {
        return lineNumbersVisibleInFiltered_;
    }
    bool minimizeToTray() const
    {
        return minimizeToTray_;
    }
    QString style() const
    {
        return style_;
    }
    ThemeMode themeMode() const
    {
        return themeMode_;
    }
    void setThemeMode( ThemeMode mode )
    {
        themeMode_ = mode;
    }
    void setMainLineNumbersVisible( bool lineNumbersVisible )
    {
        lineNumbersVisibleInMain_ = lineNumbersVisible;
    }
    void setFilteredLineNumbersVisible( bool lineNumbersVisible )
    {
        lineNumbersVisibleInFiltered_ = lineNumbersVisible;
    }
    void setMinimizeToTray( bool minimizeToTray )
    {
        minimizeToTray_ = minimizeToTray;
    }
    void setStyle( const QString& style )
    {
        style_ = style;
    }

    bool enableLogging() const
    {
        return enableLogging_;
    }
    int loggingLevel() const
    {
        return loggingLevel_;
    }

    void setEnableLogging( bool enableLogging )
    {
        enableLogging_ = enableLogging;
    }
    void setLoggingLevel( int level )
    {
        loggingLevel_ = level;
    }

    // Default settings for new views
    bool isSearchAutoRefreshDefault() const
    {
        return searchAutoRefresh_;
    }
    void setSearchAutoRefreshDefault( bool autoRefresh )
    {
        searchAutoRefresh_ = autoRefresh;
    }
    bool isSearchIgnoreCaseDefault() const
    {
        return searchIgnoreCase_;
    }
    void setSearchIgnoreCaseDefault( bool ignoreCase )
    {
        searchIgnoreCase_ = ignoreCase;
    }
    bool isSearchLogicalCombiningDefault() const
    {
        return searchLogicalCombining_;
    }
    void setSearchLogicalCombiningDefault( bool logicalCombining )
    {
        searchLogicalCombining_ = logicalCombining;
    }
    QList<int> splitterSizes() const
    {
        return splitterSizes_;
    }
    void setSplitterSizes( QList<int> sizes )
    {
        splitterSizes_ = std::move( sizes );
    }

    bool extractArchives() const
    {
        return extractArchives_;
    }
    void setExtractArchives( bool extract )
    {
        extractArchives_ = extract;
    }

    bool extractArchivesAlways() const
    {
        return extractArchivesAlways_;
    }
    void setExtractArchivesAlways( bool extract )
    {
        extractArchivesAlways_ = extract;
    }

    bool verifySslPeers() const
    {
        return verifySslPeers_;
    }
    void setVerifySslPeers( bool verify )
    {
        verifySslPeers_ = verify;
    }

    QString adbExecutable() const
    {
        return adbExecutable_;
    }
    void setAdbExecutable( QString adbExecutable )
    {
        adbExecutable_ = std::move( adbExecutable );
    }

    QString adbLogcatExtraArgs() const
    {
        return adbLogcatExtraArgs_;
    }
    void setAdbLogcatExtraArgs( QString adbLogcatExtraArgs )
    {
        adbLogcatExtraArgs_ = std::move( adbLogcatExtraArgs );
    }

    bool adbLogcatAnsiOutputEnabled() const
    {
        return adbLogcatAnsiOutputEnabled_;
    }
    void setAdbLogcatAnsiOutputEnabled( bool enabled )
    {
        adbLogcatAnsiOutputEnabled_ = enabled;
    }

    QString iosLogExecutable() const
    {
        return iosLogExecutable_;
    }
    void setIosLogExecutable( QString iosLogExecutable )
    {
        iosLogExecutable_ = std::move( iosLogExecutable );
    }

    QString iosLogExtraArgs() const
    {
        return iosLogExtraArgs_;
    }
    void setIosLogExtraArgs( QString iosLogExtraArgs )
    {
        iosLogExtraArgs_ = std::move( iosLogExtraArgs );
    }

    bool iosLogAnsiOutputEnabled() const
    {
        return iosLogAnsiOutputEnabled_;
    }
    void setIosLogAnsiOutputEnabled( bool enabled )
    {
        iosLogAnsiOutputEnabled_ = enabled;
    }

    bool forceFontAntialiasing() const
    {
        return forceFontAntialiasing_;
    }
    void setForceFontAntialiasing( bool force )
    {
        forceFontAntialiasing_ = force;
    }

    bool useBoldFont() const
    {
        return useBoldFont_;
    }
    void setUseBoldFont( bool bold )
    {
        useBoldFont_ = bold;
    }

    bool enableQtHighDpi() const
    {
        return enableQtHighDpi_;
    }
    void setEnableQtHighDpi( bool enable )
    {
        enableQtHighDpi_ = enable;
    }

    int scaleFactorRounding() const
    {
        return scaleFactorRounding_;
    }
    void setScaleFactorRounding( int rounding )
    {
        scaleFactorRounding_ = rounding;
    }

    bool mainSearchHighlight() const
    {
        return enableMainSearchHighlight_;
    }
    void setEnableMainSearchHighlight( bool enable )
    {
        enableMainSearchHighlight_ = enable;
    }

    bool variateMainSearchHighlight() const
    {
        return enableMainSearchHighlightVariance_;
    }
    void setVariateMainSearchHighlight( bool enable )
    {
        enableMainSearchHighlightVariance_ = enable;
    }

    QColor mainSearchBackColor() const
    {
        return mainSearchBackColor_;
    }
    void setMainSearchBackColor( QColor color )
    {
        mainSearchBackColor_ = color;
    }

    QColor qfBackColor() const
    {
        return qfBackColor_;
    }
    void setQfBackColor( QColor color )
    {
        qfBackColor_ = color;
    }

    bool qfIgnoreCase() const
    {
        return qfIgnoreCase_;
    }
    void setQfIgnoreCase( bool ignore )
    {
        qfIgnoreCase_ = ignore;
    }

    std::map<std::string, QStringList> shortcuts() const
    {
        return shortcuts_;
    }
    void setShortcuts( const std::map<std::string, QStringList>& shortcuts )
    {
        shortcuts_ = shortcuts;
    }

    bool allowFollowOnScroll() const
    {
        return allowFollowOnScroll_;
    }
    void setAllowFollowOnScroll( bool enable )
    {
        allowFollowOnScroll_ = enable;
    }

    bool useTextWrap() const
    {
        return useTextWrap_;
    }
    void setUseTextWrap( bool enable )
    {
        useTextWrap_ = enable;
    }

    bool confirmTabClose() const
    {
        return confirmTabClose_;
    }
    void setConfirmTabClose( bool confirm )
    {
        confirmTabClose_ = confirm;
    }

    bool autoRunSearchOnPatternChange() const
    {
        return autoRunSearchOnPatternChange_;
    }
    void setAutoRunSearchOnPatternChange( bool enable )
    {
        autoRunSearchOnPatternChange_ = enable;
    }
    bool showAllInFilteredViewWhenSearchEmpty() const
    {
        return showAllInFilteredViewWhenSearchEmpty_;
    }
    void setShowAllInFilteredViewWhenSearchEmpty( bool enable )
    {
        showAllInFilteredViewWhenSearchEmpty_ = enable;
    }

    bool optimizeForNotLatinEncodings() const
    {
        return optimizeForNotLatinEncodings_;
    }
    void setOptimizeForNotLatinEncodings( bool enable )
    {
        optimizeForNotLatinEncodings_ = enable;
    }

    bool hideAnsiColorSequences() const
    {
        return hideAnsiColorSequences_;
    }
    void setHideAnsiColorSequences( bool hide )
    {
        hideAnsiColorSequences_ = hide;
    }
    bool renderAnsiColorSequences() const
    {
        return renderAnsiColorSequences_;
    }
    void setRenderAnsiColorSequences( bool render )
    {
        renderAnsiColorSequences_ = render;
    }

    int defaultEncodingMib() const
    {
        return defaultEncodingMib_;
    }
    void setDefaultEncodingMib( int mib )
    {
        defaultEncodingMib_ = mib;
    }

    std::map<QString, QString> darkPalette() const {
        return darkPalette_;
    }

    // Reads/writes the current config in the QSettings object passed
    void saveToStorage( QSettings& settings ) const;
    void retrieveFromStorage( QSettings& settings );

  private:
    // Configuration settings
    mutable QFont mainFont_ = { "DejaVu Sans Mono", 12 };
    SearchRegexpType mainRegexpType_ = SearchRegexpType::ExtendedRegexp;
    SearchRegexpType quickfindRegexpType_ = SearchRegexpType::ExtendedRegexp;
    bool quickfindIncremental_ = true;

    QString language_{ "en" };

    bool nativeFileWatchEnabled_ = true;
#ifdef Q_OS_WIN
    bool pollingEnabled_ = true;
#else
    bool pollingEnabled_ = false;
#endif

    int pollIntervalMs_ = 2000;

    bool fastModificationDetection_ = false;

    bool loadLastSession_ = true;
    bool followFileOnLoad_ = false;
    bool allowMultipleWindows_ = false;

    // View settings
    bool overviewVisible_ = true;
    bool lineNumbersVisibleInMain_ = false;
    bool lineNumbersVisibleInFiltered_ = true;
    bool minimizeToTray_ = false;
    QString style_;
    ThemeMode themeMode_ = ThemeMode::Auto;

    // Default settings for new views
    bool searchAutoRefresh_ = true;
    bool searchIgnoreCase_ = true;
    bool searchLogicalCombining_ = false;
    QList<int> splitterSizes_;

    // Performance settings
    bool useSearchResultsCache_ = true;
    unsigned searchResultsCacheLines_ = 1000000;
    bool useParallelSearch_ = true;
    bool useBlockScan_ = false;
    int indexReadBufferSizeMb_ = 16;
    int searchReadBufferSizeLines_ = 10000;
    int searchThreadPoolSize_ = 0;
    bool keepFileClosed_ = false;
    bool useCompressedIndex_ = true;

    bool enableLogging_ = false;
    int loggingLevel_ = 4;

    bool enableVersionChecking_ = true;

    bool extractArchives_ = true;
    bool extractArchivesAlways_ = false;

    bool verifySslPeers_ = true;
    QString adbExecutable_;
    QString adbLogcatExtraArgs_;
    bool adbLogcatAnsiOutputEnabled_ = false;
    QString iosLogExecutable_;
    QString iosLogExtraArgs_;
    bool iosLogAnsiOutputEnabled_ = false;

    bool forceFontAntialiasing_ = false;
    bool enableQtHighDpi_ = true;
    bool useBoldFont_ = false;

    int scaleFactorRounding_ = 1;

    RegexpEngine regexpEngine_ = RegexpEngine::Vectorscan;

    QColor qfBackColor_ = Qt::yellow;
    QColor mainSearchBackColor_ = Qt::lightGray;
    bool enableMainSearchHighlight_ = true;
    bool enableMainSearchHighlightVariance_ = false;

    bool allowFollowOnScroll_ = true;
    bool autoRunSearchOnPatternChange_ = false;
    bool showAllInFilteredViewWhenSearchEmpty_ = true;

    bool optimizeForNotLatinEncodings_ = false;

    bool hideAnsiColorSequences_ = false;
    bool renderAnsiColorSequences_ = false;

    int defaultEncodingMib_ = 106; // UTF-8

    bool qfIgnoreCase_ = true;

    bool useTextWrap_ = false;
    int lineSpacingPercent_ = DefaultLineSpacingPercent;

    bool confirmTabClose_ = true;

    std::map<std::string, QStringList> shortcuts_;

    // Dark theme palette aligned with the Modern theme tokens.
    // FilteredView uses Base color, keep it close to Window for consistency.
    std::map<QString, QString> darkPalette_ = {
        {"Window", "#0F1115"},            // Main window background
        {"WindowText", "#E6E9EF"},        // Primary text
        {"Base", "#151821"},              // Text input / view background
        {"AlternateBase", "#1B1F2A"},     // Alternate rows
        {"ToolTipBase", "#1A1F2B"},       // Tooltip background
        {"ToolTipText", "#E6E9EF"},       // Tooltip text
        {"Text", "#E6E9EF"},              // Main text color
        {"Button", "#1A1F2B"},            // Button background
        {"ButtonText", "#E6E9EF"},        // Button text
        {"Link", "#5B8CFF"},              // Link color
        {"Highlight", "#23314D"},         // Selection background
        {"HighlightedText", "#EAF2FF"},   // Selection text
        {"ActiveButton", "#222939"},      // Active button background
        {"DisabledButtonText", "#7C8596"}, // Disabled button text
        {"DisabledWindowText", "#7C8596"}, // Disabled window text
        {"DisabledText", "#7C8596"},       // Disabled text
        {"DisabledLight", "#1A1F2B"},      // Disabled light
    };
};

#endif
