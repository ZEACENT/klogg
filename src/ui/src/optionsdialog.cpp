/*
 * Copyright (C) 2009, 2010, 2011, 2013 Nicolas Bonnefon and other contributors
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

#include <QColorDialog>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QMessageBox>
#include <QToolButton>
#include <QtGui>

#include <algorithm>

#include "adbprocesstransport.h"
#include "encodings.h"
#include "fontutils.h"
#include "highlighteredit.h"
#include "ioslogprocesstransport.h"
#include "log.h"
#include "logger.h"
#include "mainwindow.h"
#include "openfilehelper.h"
#include "recentfiles.h"
#include "savedsearches.h"
#include "shortcuts.h"
#include "styles.h"

#include "optionsdialog.h"

static constexpr int PollIntervalMin = 10;
static constexpr int PollIntervalMax = 3600000;

// Constructor
OptionsDialog::OptionsDialog( QWidget* parent )
    : QDialog( parent )
{
    setupUi( this );
    setSizeGripEnabled( true );
    setMinimumSize( QSize( 640, 520 ) );

    setupTabs();
    setupFontList();
    setupRegexp();
    setupStyles();
    setupEncodings();
    setupLanguageList();
    setupIosLogSettings();
    setupLiveSourceSettings();
    setupPanelResetButtons();
    standardizeLayoutSpacing();

    // Validators
    QValidator* pollingIntervalValidator = new QIntValidator( PollIntervalMin, PollIntervalMax );
    pollIntervalLineEdit->setValidator( pollingIntervalValidator );

    connect( buttonBox, &QDialogButtonBox::clicked, this, &OptionsDialog::onButtonBoxClicked );
    connect( fontFamilyBox, &QComboBox::currentTextChanged, this, &OptionsDialog::updateFontSize );
    connect( pollingCheckBox, &QCheckBox::toggled, [ this ]( auto ) { this->setupPolling(); } );
    connect( searchResultsCacheCheckBox, &QCheckBox::toggled,
             [ this ]( auto ) { this->setupSearchResultsCache(); } );
    connect( loggingCheckBox, &QCheckBox::toggled, [ this ]( auto ) { this->setupLogging(); } );
    connect( openLogFileButton, &QPushButton::clicked, this, &OptionsDialog::openLogFile );

    connect( extractArchivesCheckBox, &QCheckBox::toggled,
             [ this ]( auto ) { this->setupArchives(); } );
    connect( hideAnsiColorsCheckBox, &QCheckBox::toggled, this, [ this ]( bool checked ) {
        if ( checked ) {
            renderAnsiColorsCheckBox->setChecked( false );
        }
    } );
    connect( renderAnsiColorsCheckBox, &QCheckBox::toggled, this, [ this ]( bool checked ) {
        if ( checked ) {
            hideAnsiColorsCheckBox->setChecked( false );
        }
    } );

    connect( mainSearchColorButton, &QPushButton::clicked, this, &OptionsDialog::changeMainColor );
    connect( quickFindColorButton, &QPushButton::clicked, this, &OptionsDialog::changeQfColor );

    connect( adbDetectButton, &QPushButton::clicked, this, [ this ] {
        const auto resolved = AdbProcessTransport::detectAdbExecutable();
        if ( resolved.isEmpty() ) {
            QMessageBox::information(
                this, tr( "Detect ADB executable" ),
                tr( "No adb found at well-known install locations. Set the path manually." ) );
            return;
        }
        const auto current = adbExecutableLineEdit->text().trimmed();
        if ( !current.isEmpty() && current != resolved ) {
            const auto answer = QMessageBox::question(
                this, tr( "Detect ADB executable" ),
                tr( "Replace the configured path\n\n    %1\n\nwith the auto-detected path?\n\n    %2" )
                    .arg( current, resolved ),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel );
            if ( answer != QMessageBox::Yes ) {
                return;
            }
        }
        adbExecutableLineEdit->setText( resolved );
    } );

    restoreShortcutsDefaults->setText( tr( "Reset to defaults" ) );
    connect( restoreShortcutsDefaults, &QPushButton::clicked, this,
             &OptionsDialog::resetShortcutsDefaults );

    updateDialogFromConfig();

    setupPolling();
    setupSearchResultsCache();
    setupLogging();
    setupArchives();
}

//
// Private functions
//

// Setups the tabs depending on the configuration
void OptionsDialog::setupTabs()
{
#ifndef Q_OS_WIN
    keepFileClosedCheckBox->setVisible( false );
#endif

#ifdef Q_OS_MAC
    minimizeToTrayCheckBox->setVisible( false );
#endif

#ifndef KLOGG_HAS_VECTORSCAN
    regexpEngineLabel->setVisible( false );
    regexpEngineComboBox->setVisible( false );
#endif
}

// Populates the 'family' ComboBox
void OptionsDialog::setupFontList()
{
    const auto families = FontUtils::availableFonts();
    for ( const QString& str : families ) {
        fontFamilyBox->addItem( str );
    }
}

// Populate the regexp ComboBoxes
void OptionsDialog::setupRegexp()
{
    QStringList regexpTypes;
    regexpTypes << tr( "Extended Regexp" ) << tr( "Fixed Strings" );

    mainSearchBox->addItems( regexpTypes );
    quickFindSearchBox->addItems( regexpTypes );

    QStringList regexpEngines;
    regexpEngines << tr( "Vectorscan" ) << tr( "Qt" );

    regexpEngineComboBox->addItems( regexpEngines );
}

void OptionsDialog::setupStyles()
{
    styleComboBox->clear();
    styleComboBox->addItem( tr( "Modern" ), StyleManager::ModernKey );
    styleComboBox->addItem( tr( "System" ), StyleManager::SystemKey );
    styleComboBox->addItem( tr( "Classic Dark" ), StyleManager::DarkStyleKey );

    // Setup theme mode combo box
    themeModeComboBox->addItem( tr( "Light" ), static_cast<int>( ThemeMode::Light ) );
    themeModeComboBox->addItem( tr( "Dark" ), static_cast<int>( ThemeMode::Dark ) );
    themeModeComboBox->addItem( tr( "Auto (Follow System)" ), static_cast<int>( ThemeMode::Auto ) );
}

void OptionsDialog::setupEncodings()
{
    const auto availableEncodings = EncodingMenu::supportedEncodings();
    encodingComboBox->addItem( "Auto", -1 );

    std::map<QString, int> allMibs;

    for ( const auto& group : availableEncodings ) {
        for ( const auto& mib : group.second ) {
            auto codec = QTextCodec::codecForMib( mib );
            if ( codec ) {
                allMibs.emplace( codec->name(), mib );
            }
        }
    }

    for ( const auto& codec : allMibs ) {
        encodingComboBox->addItem( codec.first, codec.second );
    }
}

void OptionsDialog::setupLanguageList()
{
    QResource resource( ":/i18n/Languages.xml" );
    QByteArray bytes( reinterpret_cast<const char*>( resource.data() ), (int)resource.size() );
    QXmlStreamReader xml( bytes );

    while ( !xml.atEnd() ) {
        QXmlStreamReader::TokenType token = xml.readNext();
        if ( xml.hasError() ) {
            LOG_ERROR << "load language error";
            return;
        }

        if ( xml.name() == QString( "language" ) && token == QXmlStreamReader::StartElement ) {
            QXmlStreamAttributes attributes = xml.attributes();
            languageComboBox->addItem( attributes.value( "name" ).toString(),
                                       attributes.value( "ietfCode" ).toString() );
        }
    }
}

void OptionsDialog::setupIosLogSettings()
{
    adbAnsiOutputCheckBox_ = new QCheckBox( tr( "Enable ANSI color output" ), adbGroupBox );
    adbAnsiOutputCheckBox_->setObjectName( QStringLiteral( "adbAnsiOutputCheckBox" ) );
    if ( auto* adbLayout = qobject_cast<QVBoxLayout*>( adbGroupBox->layout() ) ) {
        adbLayout->insertWidget( std::max( 0, adbLayout->count() - 1 ), adbAnsiOutputCheckBox_ );
    }

    iosLogGroupBox_ = new QGroupBox( tr( "iOS Log Stream" ), liveSourceTab );
    iosLogGroupBox_->setObjectName( QStringLiteral( "iosLogGroupBox" ) );

    auto* layout = new QVBoxLayout( iosLogGroupBox_ );
    layout->setSpacing( 6 );

    auto* executableRow = new QHBoxLayout();
    auto* executableLabel = new QLabel( tr( "pymobiledevice3 executable" ), iosLogGroupBox_ );
    iosLogExecutableLineEdit_ = new QLineEdit( iosLogGroupBox_ );
    iosLogExecutableLineEdit_->setObjectName( QStringLiteral( "iosLogExecutableLineEdit" ) );
    iosLogExecutableLineEdit_->setPlaceholderText( QStringLiteral( "pymobiledevice3" ) );
    auto* detectButton = new QPushButton( tr( "Detect" ), iosLogGroupBox_ );
    detectButton->setObjectName( QStringLiteral( "iosLogDetectButton" ) );
    detectButton->setToolTip(
        tr( "Probe well-known Homebrew install locations and fill the path automatically." ) );
    executableRow->addWidget( executableLabel );
    executableRow->addWidget( iosLogExecutableLineEdit_ );
    executableRow->addWidget( detectButton );

    auto* argsRow = new QHBoxLayout();
    auto* argsLabel = new QLabel( tr( "Extra iOS log args" ), iosLogGroupBox_ );
    iosLogArgsLineEdit_ = new QLineEdit( iosLogGroupBox_ );
    iosLogArgsLineEdit_->setObjectName( QStringLiteral( "iosLogArgsLineEdit" ) );
    argsRow->addWidget( argsLabel );
    argsRow->addWidget( iosLogArgsLineEdit_ );

    iosLogAnsiOutputCheckBox_ = new QCheckBox( tr( "Enable ANSI color output" ), iosLogGroupBox_ );
    iosLogAnsiOutputCheckBox_->setObjectName( QStringLiteral( "iosLogAnsiOutputCheckBox" ) );

    auto* helpLabel = new QLabel( iosLogGroupBox_ );
    helpLabel->setWordWrap( true );
    helpLabel->setText( tr( "Extra arguments are appended after "
                            "'pymobiledevice3 syslog live --udid <udid>'. This feature is "
                            "available only on macOS." ) );

    layout->addLayout( executableRow );
    layout->addLayout( argsRow );
    layout->addWidget( iosLogAnsiOutputCheckBox_ );
    layout->addWidget( helpLabel );

    const auto insertIndex = std::max( 0, verticalLayout_liveSource->count() - 1 );
    verticalLayout_liveSource->insertWidget( insertIndex, iosLogGroupBox_ );

    connect( detectButton, &QPushButton::clicked, this, [ this ] {
        const auto resolved = IosLogProcessTransport::detectIosSyslogExecutable();
        if ( resolved.isEmpty() ) {
            QMessageBox::information(
                this, tr( "Detect iOS log executable" ),
                tr( "No pymobiledevice3 found at well-known install locations. Set the path "
                    "manually or install pymobiledevice3." ) );
            return;
        }

        const auto current = iosLogExecutableLineEdit_->text().trimmed();
        if ( !current.isEmpty() && current != resolved ) {
            const auto answer = QMessageBox::question(
                this, tr( "Detect iOS log executable" ),
                tr( "Replace the configured path\n\n    %1\n\nwith the auto-detected path?\n\n    %2" )
                    .arg( current, resolved ),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel );
            if ( answer != QMessageBox::Yes ) {
                return;
            }
        }
        iosLogExecutableLineEdit_->setText( resolved );
    } );

#ifndef Q_OS_MAC
    iosLogGroupBox_->setVisible( false );
#endif
}

void OptionsDialog::setupLiveSourceSettings()
{
    // Live Source group box with auto-reconnect and capture file rolling settings.
    // These settings control the behavior of live log streams (ADB logcat, iOS log).
    // Inserted on the File tab alongside the ADB and iOS log stream settings.

    liveSourceGroupBox_ = new QGroupBox( tr( "Live Source" ), liveSourceTab );
    liveSourceGroupBox_->setObjectName( QStringLiteral( "liveSourceGroupBox" ) );

    auto* layout = new QVBoxLayout( liveSourceGroupBox_ );
    layout->setSpacing( 6 );

    // Auto-reconnect toggle — enables/disables automatic reconnection when the
    // live source unexpectedly disconnects or encounters an error. Uses
    // exponential backoff starting at 1 second and capping at 30 seconds
    // between attempts.
    liveSourceAutoReconnectCheckBox_ = new QCheckBox(
        tr( "Enable auto-reconnect on connection loss" ), liveSourceGroupBox_ );
    liveSourceAutoReconnectCheckBox_->setObjectName(
        QStringLiteral( "liveSourceAutoReconnectCheckBox" ) );
    liveSourceAutoReconnectCheckBox_->setToolTip(
        tr( "When enabled, klogg automatically attempts to reconnect to the live source "
            "after an unexpected disconnection or error. Uses exponential backoff "
            "starting at 1 second and capping at 30 seconds between attempts." ) );

    // Max reconnect attempts — limits the number of automatic reconnection
    // attempts. Set to 0 for unlimited retries. Each attempt uses increasing
    // delay (1s, 2s, 4s, 8s, ... up to 30s).
    auto* maxAttemptsRow = new QHBoxLayout();
    auto* maxAttemptsLabel = new QLabel( tr( "Max reconnect attempts" ), liveSourceGroupBox_ );
    maxAttemptsLabel->setToolTip(
        tr( "Maximum number of automatic reconnection attempts. "
            "Set to 0 for unlimited retries. Each retry waits longer (exponential backoff)." ) );
    liveSourceMaxAttemptsSpinBox_ = new QSpinBox( liveSourceGroupBox_ );
    liveSourceMaxAttemptsSpinBox_->setObjectName(
        QStringLiteral( "liveSourceMaxAttemptsSpinBox" ) );
    liveSourceMaxAttemptsSpinBox_->setRange( 0, 9999 );
    liveSourceMaxAttemptsSpinBox_->setSpecialValueText( tr( "Unlimited" ) );
    liveSourceMaxAttemptsSpinBox_->setToolTip(
        tr( "Maximum number of automatic reconnection attempts. "
            "Set to 0 for unlimited retries." ) );
    maxAttemptsRow->addWidget( maxAttemptsLabel );
    maxAttemptsRow->addWidget( liveSourceMaxAttemptsSpinBox_ );
    maxAttemptsRow->addStretch();

    // Max capture file size — when the live capture output file exceeds this
    // size, it is rotated (renamed as a numbered backup) and a new file is
    // started. Set to 0 to disable size-based rolling.
    auto* maxFileSizeRow = new QHBoxLayout();
    auto* maxFileSizeLabel = new QLabel( tr( "Max capture file size (MB)" ), liveSourceGroupBox_ );
    maxFileSizeLabel->setToolTip(
        tr( "When the capture file exceeds this size, it is rotated. "
            "Set to 0 for unlimited size (no rotation by size)." ) );
    liveSourceRollingMaxFileSizeSpinBox_ = new QSpinBox( liveSourceGroupBox_ );
    liveSourceRollingMaxFileSizeSpinBox_->setObjectName(
        QStringLiteral( "liveSourceRollingMaxFileSizeSpinBox" ) );
    liveSourceRollingMaxFileSizeSpinBox_->setRange( 0, 1048576 ); // 0 to ~1 TB in MB
    liveSourceRollingMaxFileSizeSpinBox_->setSpecialValueText( tr( "Unlimited" ) );
    liveSourceRollingMaxFileSizeSpinBox_->setSuffix( tr( " MB" ) );
    liveSourceRollingMaxFileSizeSpinBox_->setToolTip(
        tr( "Maximum size of each rolling capture file in megabytes. "
            "When exceeded, a new file is started and oldest files may be deleted "
            "if the backup count is also set. Set to 0 to disable rolling by file size." ) );
    maxFileSizeRow->addWidget( maxFileSizeLabel );
    maxFileSizeRow->addWidget( liveSourceRollingMaxFileSizeSpinBox_ );
    maxFileSizeRow->addStretch();

    // Rolling backup count — number of old capture files to retain during
    // rotation. Files beyond this count are deleted. Set to 0 to disable
    // rolling backup (no backup files retained, only the current file).
    auto* backupCountRow = new QHBoxLayout();
    auto* backupCountLabel = new QLabel( tr( "Rolling backup count" ), liveSourceGroupBox_ );
    backupCountLabel->setToolTip(
        tr( "Number of old capture files to keep when rolling. "
            "Older files beyond this count are automatically deleted. "
            "Set to 0 to disable rolling backup (only the current file is kept)." ) );
    liveSourceRollingBackupCountSpinBox_ = new QSpinBox( liveSourceGroupBox_ );
    liveSourceRollingBackupCountSpinBox_->setObjectName(
        QStringLiteral( "liveSourceRollingBackupCountSpinBox" ) );
    liveSourceRollingBackupCountSpinBox_->setRange( 0, 999 );
    liveSourceRollingBackupCountSpinBox_->setSpecialValueText( tr( "No rolling" ) );
    liveSourceRollingBackupCountSpinBox_->setToolTip(
        tr( "Number of backup capture files to retain during rotation. "
            "Older files beyond this count are deleted. "
            "Set to 0 to disable rolling backup." ) );
    backupCountRow->addWidget( backupCountLabel );
    backupCountRow->addWidget( liveSourceRollingBackupCountSpinBox_ );
    backupCountRow->addStretch();

    layout->addWidget( liveSourceAutoReconnectCheckBox_ );
    layout->addLayout( maxAttemptsRow );
    layout->addLayout( maxFileSizeRow );
    layout->addLayout( backupCountRow );

    // Help text explaining when settings take effect
    auto* helpLabel = new QLabel( liveSourceGroupBox_ );
    helpLabel->setWordWrap( true );
    helpLabel->setText( tr( "These settings control live log stream capture behavior. "
                            "Changes take effect for new live source connections." ) );
    layout->addWidget( helpLabel );

    // Insert before the spacer at the bottom of the File tab layout
    const auto insertIndex = std::max( 0, verticalLayout_liveSource->count() - 1 );
    verticalLayout_liveSource->insertWidget( insertIndex, liveSourceGroupBox_ );
}

void OptionsDialog::setupPanelResetButtons()
{
    auto addResetButton = [ this ]( QWidget* tab, const QString& objectName, auto slot ) {
        auto* button = new QPushButton( tr( "Reset to defaults" ), tab );
        button->setObjectName( objectName );

        auto* row = new QHBoxLayout();
        row->addStretch();
        row->addWidget( button );

        if ( auto* layout = qobject_cast<QVBoxLayout*>( tab->layout() ) ) {
            layout->addLayout( row );
        }

        connect( button, &QPushButton::clicked, this, slot );
    };

    addResetButton( general_tab, QStringLiteral( "resetGeneralDefaultsButton" ),
                    &OptionsDialog::resetGeneralDefaults );
    addResetButton( viewTab, QStringLiteral( "resetViewDefaultsButton" ),
                    &OptionsDialog::resetViewDefaults );
    addResetButton( file_watch_tab, QStringLiteral( "resetFileDefaultsButton" ),
                    &OptionsDialog::resetFileDefaults );
    addResetButton( liveSourceTab, QStringLiteral( "resetLiveSourceDefaultsButton" ),
                    &OptionsDialog::resetLiveSourceDefaults );
    addResetButton( advanced_tab, QStringLiteral( "resetAdvancedDefaultsButton" ),
                    &OptionsDialog::resetAdvancedDefaults );
}

// Apply uniform vertical spacing to every group-box layout in every tab.
// This prevents "line overlap" caused by inconsistent or missing spacing
// between widgets inside group boxes.
void OptionsDialog::standardizeLayoutSpacing()
{
    static constexpr int kGroupBoxLayoutSpacing = 6;
    static constexpr int kTabLayoutSpacing = 6;

    const QList<QWidget*> tabs = { general_tab, viewTab, file_watch_tab,
                                   liveSourceTab, shortcutsTab, advanced_tab };

    for ( auto* tab : tabs ) {
        if ( !tab ) {
            continue;
        }
        // Consistent spacing between group boxes on the tab surface
        if ( auto* tabLayout = tab->layout() ) {
            tabLayout->setSpacing( kTabLayoutSpacing );
        }

        // Consistent spacing inside every group box (covers both .ui-defined
        // and dynamically-created boxes like iOS Log Stream / Live Source)
        const auto groupBoxes = tab->findChildren<QGroupBox*>();
        for ( auto* groupBox : groupBoxes ) {
            if ( auto* groupLayout = groupBox->layout() ) {
                groupLayout->setSpacing( kGroupBoxLayoutSpacing );
            }
        }
    }
}

void OptionsDialog::setupPolling()
{
    pollIntervalLineEdit->setEnabled( pollingCheckBox->isChecked() );
}

void OptionsDialog::setupSearchResultsCache()
{
    searchCacheSpinBox->setEnabled( searchResultsCacheCheckBox->isChecked() );
}

void OptionsDialog::setupLogging()
{
    verbositySpinBox->setEnabled( loggingCheckBox->isChecked() );
    
    // Update log file path display
    if ( loggingCheckBox->isChecked() ) {
        const auto logFilePath = logging::getLogFilePath();
        if ( !logFilePath.isEmpty() ) {
            logFilePathLabel->setText( tr( "Log file: %1" ).arg( logFilePath ) );
            logFilePathLabel->setVisible( true );
            openLogFileButton->setVisible( true );
            openLogFileButton->setEnabled( true );
        }
        else {
            logFilePathLabel->setText( tr( "Log file: Console output only" ) );
            logFilePathLabel->setVisible( true );
            openLogFileButton->setVisible( false );
        }
    }
    else {
        logFilePathLabel->setVisible( false );
        openLogFileButton->setVisible( false );
    }
}

void OptionsDialog::openLogFile()
{
    const auto logFilePath = logging::getLogFilePath();
    if ( !logFilePath.isEmpty() ) {
        showPathInFileExplorer( logFilePath );
    }
}

void OptionsDialog::setupArchives()
{
    extractArchivesAlwaysCheckBox->setEnabled( extractArchivesCheckBox->isChecked() );
}

// Convert a regexp type to its index in the list
int OptionsDialog::getRegexpTypeIndex( SearchRegexpType syntax ) const
{
    int index;

    switch ( syntax ) {
    case SearchRegexpType::FixedString:
        index = 1;
        break;
    default:
        index = 0;
        break;
    }

    return index;
}

// Convert the index of a regexp type to its type
SearchRegexpType OptionsDialog::getRegexpTypeFromIndex( int index ) const
{
    SearchRegexpType type;

    switch ( index ) {
    case 1:
        type = SearchRegexpType::FixedString;
        break;
    default:
        type = SearchRegexpType::ExtendedRegexp;
        break;
    }

    return type;
}

int OptionsDialog::getRegexpEngineIndex( RegexpEngine engine ) const
{
    int index;

    switch ( engine ) {
    case RegexpEngine::QRegularExpression:
        index = 1;
        break;
    default:
        index = 0;
        break;
    }

    return index;
}

RegexpEngine OptionsDialog::getRegexpEngineFromIndex( int index ) const
{
    RegexpEngine type;

    switch ( index ) {
    case 1:
        type = RegexpEngine::QRegularExpression;
        break;
    default:
        type = RegexpEngine::Vectorscan;
        break;
    }

    return type;
}

// Updates the dialog box using values in global Config()
void OptionsDialog::updateDialogFromConfig()
{
    updateDialogFromConfiguration( Configuration::get() );
}

void OptionsDialog::updateDialogFromConfiguration( const Configuration& config )
{
    // Main font
    const auto configuredFont = config.mainFont();
    const auto configuredFamily = configuredFont.family();
    const auto configuredPointSize = configuredFont.pointSize();

    int familyIndex = fontFamilyBox->findText( configuredFamily );
    if ( familyIndex == -1 && !configuredFamily.isEmpty() ) {
        fontFamilyBox->addItem( configuredFamily );
        familyIndex = fontFamilyBox->findText( configuredFamily );
    }
    if ( familyIndex != -1 ) {
        fontFamilyBox->setCurrentIndex( familyIndex );
    }

    updateFontSizePreservingSelection( fontFamilyBox->currentText(), configuredPointSize );

    lineSpacingSpinBox->setValue( config.lineSpacingPercent() );
    fontSmoothCheckBox->setChecked( config.forceFontAntialiasing() );
    boldFontCheckBox->setChecked( config.useBoldFont() );
    wrapTextCheckBox->setChecked( config.useTextWrap() );
    enableQtHiDpiCheckBox->setChecked( config.enableQtHighDpi() );
    scaleRoundingComboBox->setCurrentIndex( config.scaleFactorRounding() - 1 );

    // Language
    auto langIdx = languageComboBox->findData( { config.language() } );
    if ( langIdx == -1 ) {
        langIdx = 0;
    }
    languageComboBox->setCurrentIndex( langIdx );

    const auto style = config.style();
    const int styleIndex = styleComboBox->findData( style );
    styleComboBox->setCurrentIndex( styleIndex == -1 ? 0 : styleIndex );

    // Theme mode
    const auto themeMode = config.themeMode();
    const int themeModeIndex = themeModeComboBox->findData( static_cast<int>( themeMode ) );
    if ( themeModeIndex != -1 ) {
        themeModeComboBox->setCurrentIndex( themeModeIndex );
    }
    else {
        themeModeComboBox->setCurrentIndex( 2 ); // Default to Auto
    }

    hideAnsiColorsCheckBox->setChecked( config.hideAnsiColorSequences() );
    renderAnsiColorsCheckBox->setChecked( !config.hideAnsiColorSequences()
                                          && config.renderAnsiColorSequences() );

    // Regexp types
    mainSearchBox->setCurrentIndex( getRegexpTypeIndex( config.mainRegexpType() ) );
    mainSearchColor_ = config.mainSearchBackColor();
    HighlighterEdit::updateIcon( mainSearchColorButton, mainSearchColor_ );
    quickFindSearchBox->setCurrentIndex( getRegexpTypeIndex( config.quickfindRegexpType() ) );
    qfSearchColor_ = config.qfBackColor();
    HighlighterEdit::updateIcon( quickFindColorButton, qfSearchColor_ );
    regexpEngineComboBox->setCurrentIndex( getRegexpEngineIndex( config.regexpEngine() ) );
    autoRunSearchOnAddCheckBox->setChecked( config.autoRunSearchOnPatternChange() );
    showAllFilteredWhenEmptyCheckBox->setChecked(
        config.showAllInFilteredViewWhenSearchEmpty() );

    highlightMainSearchCheckBox->setChecked( config.mainSearchHighlight() );
    variateHighlightCheckBox->setChecked( config.variateMainSearchHighlight() );
    incrementalCheckBox->setChecked( config.isQuickfindIncremental() );
    caseSensitiveCheckBox->setChecked( !config.isSearchIgnoreCaseDefault() );
    logicalCombiningCheckBox->setChecked( config.isSearchLogicalCombiningDefault() );
    autoRefreshCheckBox->setChecked( config.isSearchAutoRefreshDefault() );

    // Polling
    nativeFileWatchCheckBox->setChecked( config.nativeFileWatchEnabled() );
    fastModificationDetectionCheckBox->setChecked( config.fastModificationDetection() );
    pollingCheckBox->setChecked( config.pollingEnabled() );
    pollIntervalLineEdit->setText( QString::number( config.pollIntervalMs() ) );
    allowFollowOnScrollCheckBox->setChecked( config.allowFollowOnScroll() );

    // Last session
    loadLastSessionCheckBox->setChecked( config.loadLastSession() );
    followFileOnLoadCheckBox->setChecked( config.followFileOnLoad() );
    minimizeToTrayCheckBox->setChecked( config.minimizeToTray() );
    multipleWindowsCheckBox->setChecked( config.allowMultipleWindows() );
    confirmTabCloseCheckBox->setChecked( config.confirmTabClose() );

    loggingCheckBox->setChecked( config.enableLogging() );
    verbositySpinBox->setValue( config.loggingLevel() );
    
    // Apply logging settings to get log file path
    logging::enableFileLogging( config.enableLogging(),
                                static_cast<logging::LogLevel>( config.loggingLevel() ) );
    
    // Update log file path display immediately
    setupLogging();

    extractArchivesCheckBox->setChecked( config.extractArchives() );
    extractArchivesAlwaysCheckBox->setChecked( config.extractArchivesAlways() );

    // Perf
    parallelSearchCheckBox->setChecked( config.useParallelSearch() );
    searchResultsCacheCheckBox->setChecked( config.useSearchResultsCache() );
    searchCacheSpinBox->setValue( static_cast<int>( config.searchResultsCacheLines() ) );
    indexReadBufferSpinBox->setValue( config.indexReadBufferSizeMb() );
    searchReadBufferSpinBox->setValue( config.searchReadBufferSizeLines() );
    keepFileClosedCheckBox->setChecked( config.keepFileClosed() );
    compressedIndexCheckBox->setChecked( config.useCompressedIndex() );
    optimizeForNotLatinEncodingsCheckBox->setChecked( config.optimizeForNotLatinEncodings() );

    // version checking
    checkForNewVersionCheckBox->setChecked( config.versionCheckingEnabled() );

    // downloads
    verifySslCheckBox->setChecked( config.verifySslPeers() );
    adbExecutableLineEdit->setText( config.adbExecutable() );
    adbLogcatArgsLineEdit->setText( config.adbLogcatExtraArgs() );
    if ( adbAnsiOutputCheckBox_ ) {
        adbAnsiOutputCheckBox_->setChecked( config.adbLogcatAnsiOutputEnabled() );
    }
    if ( iosLogExecutableLineEdit_ ) {
        iosLogExecutableLineEdit_->setText( config.iosLogExecutable() );
    }
    if ( iosLogArgsLineEdit_ ) {
        iosLogArgsLineEdit_->setText( config.iosLogExtraArgs() );
    }
    if ( iosLogAnsiOutputCheckBox_ ) {
        iosLogAnsiOutputCheckBox_->setChecked( config.iosLogAnsiOutputEnabled() );
    }
    if ( liveSourceAutoReconnectCheckBox_ ) {
        liveSourceAutoReconnectCheckBox_->setChecked( config.liveAutoReconnectEnabled() );
    }
    if ( liveSourceMaxAttemptsSpinBox_ ) {
        liveSourceMaxAttemptsSpinBox_->setValue( config.liveAutoReconnectMaxAttempts() );
    }
    if ( liveSourceRollingMaxFileSizeSpinBox_ ) {
        // Configuration stores bytes, UI displays MB
        liveSourceRollingMaxFileSizeSpinBox_->setValue(
            static_cast<int>( config.liveCaptureRollingMaxFileSize() / ( 1024 * 1024 ) ) );
    }
    if ( liveSourceRollingBackupCountSpinBox_ ) {
        liveSourceRollingBackupCountSpinBox_->setValue( config.liveCaptureRollingBackupCount() );
    }

    const auto encodingIndex = encodingComboBox->findData( config.defaultEncodingMib() );
    encodingComboBox->setCurrentIndex( encodingIndex < 0 ? 0 : encodingIndex );

    buildShortcutsTable( false );

    const auto& savedSearches = SavedSearches::get();
    searchHistorySpinBox->setValue( savedSearches.historySize() );

    const auto& recentFiles = RecentFiles::get();
    filesHistoryMaxItemsSpinBox->setMinimum( 1 );
    filesHistoryMaxItemsSpinBox->setMaximum( MAX_RECENT_FILES );
    filesHistoryMaxItemsSpinBox->setValue( recentFiles.filesHistoryMaxItems() );
}

//
// Q_SLOTS:
//

void OptionsDialog::updateFontSize( const QString& fontFamily )
{
    bool ok = false;
    const auto oldFontSize = fontSizeBox->currentText().toInt( &ok );
    updateFontSizePreservingSelection( fontFamily, ok ? oldFontSize : -1 );
}

void OptionsDialog::updateFontSizePreservingSelection( const QString& fontFamily,
                                                       int preferredPointSize )
{
    const auto sizes = FontUtils::availableFontSizes( fontFamily );

    fontSizeBox->clear();
    for ( int size : sizes ) {
        fontSizeBox->addItem( QString::number( size ) );
    }

    if ( preferredPointSize > 0
         && fontSizeBox->findText( QString::number( preferredPointSize ) ) == -1 ) {
        auto insertIndex = 0;
        while ( insertIndex < fontSizeBox->count()
                && fontSizeBox->itemText( insertIndex ).toInt() < preferredPointSize ) {
            ++insertIndex;
        }
        fontSizeBox->insertItem( insertIndex, QString::number( preferredPointSize ) );
    }

    // Now restore the size we had before
    int i = fontSizeBox->findText( QString::number( preferredPointSize ) );
    if ( i != -1 ) {
        fontSizeBox->setCurrentIndex( i );
    }
}

void OptionsDialog::changeMainColor()
{
    QColor newColor;
    if ( HighlighterEdit::showColorPicker( mainSearchColor_, newColor ) ) {
        mainSearchColor_ = newColor;
        HighlighterEdit::updateIcon( mainSearchColorButton, mainSearchColor_ );
    }
}

void OptionsDialog::changeQfColor()
{
    QColor newColor;
    if ( HighlighterEdit::showColorPicker( qfSearchColor_, newColor ) ) {
        qfSearchColor_ = newColor;
        HighlighterEdit::updateIcon( quickFindColorButton, qfSearchColor_ );
    }
}

void OptionsDialog::resetGeneralDefaults()
{
    const Configuration defaults;
    const SavedSearches defaultSavedSearches;

    mainSearchBox->setCurrentIndex( getRegexpTypeIndex( defaults.mainRegexpType() ) );
    mainSearchColor_ = defaults.mainSearchBackColor();
    HighlighterEdit::updateIcon( mainSearchColorButton, mainSearchColor_ );
    quickFindSearchBox->setCurrentIndex( getRegexpTypeIndex( defaults.quickfindRegexpType() ) );
    qfSearchColor_ = defaults.qfBackColor();
    HighlighterEdit::updateIcon( quickFindColorButton, qfSearchColor_ );

    highlightMainSearchCheckBox->setChecked( defaults.mainSearchHighlight() );
    variateHighlightCheckBox->setChecked( defaults.variateMainSearchHighlight() );
    incrementalCheckBox->setChecked( defaults.isQuickfindIncremental() );
    caseSensitiveCheckBox->setChecked( !defaults.isSearchIgnoreCaseDefault() );
    logicalCombiningCheckBox->setChecked( defaults.isSearchLogicalCombiningDefault() );
    autoRefreshCheckBox->setChecked( defaults.isSearchAutoRefreshDefault() );
    autoRunSearchOnAddCheckBox->setChecked( defaults.autoRunSearchOnPatternChange() );
    showAllFilteredWhenEmptyCheckBox->setChecked(
        defaults.showAllInFilteredViewWhenSearchEmpty() );
    searchHistorySpinBox->setValue( defaultSavedSearches.historySize() );

    loadLastSessionCheckBox->setChecked( defaults.loadLastSession() );
    followFileOnLoadCheckBox->setChecked( defaults.followFileOnLoad() );
    minimizeToTrayCheckBox->setChecked( defaults.minimizeToTray() );
    multipleWindowsCheckBox->setChecked( defaults.allowMultipleWindows() );
    confirmTabCloseCheckBox->setChecked( defaults.confirmTabClose() );

    checkForNewVersionCheckBox->setChecked( defaults.versionCheckingEnabled() );
}

void OptionsDialog::resetViewDefaults()
{
    const Configuration defaults;

    const auto defaultFont = defaults.mainFont();
    auto familyIndex = fontFamilyBox->findText( defaultFont.family() );
    if ( familyIndex == -1 ) {
        fontFamilyBox->addItem( defaultFont.family() );
        familyIndex = fontFamilyBox->findText( defaultFont.family() );
    }
    if ( familyIndex != -1 ) {
        fontFamilyBox->setCurrentIndex( familyIndex );
    }
    updateFontSizePreservingSelection( fontFamilyBox->currentText(), defaultFont.pointSize() );
    lineSpacingSpinBox->setValue( defaults.lineSpacingPercent() );
    fontSmoothCheckBox->setChecked( defaults.forceFontAntialiasing() );
    boldFontCheckBox->setChecked( defaults.useBoldFont() );
    wrapTextCheckBox->setChecked( defaults.useTextWrap() );

    auto langIdx = languageComboBox->findData( defaults.language() );
    languageComboBox->setCurrentIndex( langIdx == -1 ? 0 : langIdx );

    const auto styleIndex = styleComboBox->findData( defaults.style() );
    styleComboBox->setCurrentIndex( styleIndex == -1 ? 0 : styleIndex );
    const auto themeModeIndex
        = themeModeComboBox->findData( static_cast<int>( defaults.themeMode() ) );
    themeModeComboBox->setCurrentIndex( themeModeIndex == -1 ? 0 : themeModeIndex );

    enableQtHiDpiCheckBox->setChecked( defaults.enableQtHighDpi() );
    scaleRoundingComboBox->setCurrentIndex( defaults.scaleFactorRounding() - 1 );
    hideAnsiColorsCheckBox->setChecked( defaults.hideAnsiColorSequences() );
    renderAnsiColorsCheckBox->setChecked( !defaults.hideAnsiColorSequences()
                                          && defaults.renderAnsiColorSequences() );
}

void OptionsDialog::resetFileDefaults()
{
    const Configuration defaults;
    const RecentFiles defaultRecentFiles;

    nativeFileWatchCheckBox->setChecked( defaults.nativeFileWatchEnabled() );
    fastModificationDetectionCheckBox->setChecked( defaults.fastModificationDetection() );
    pollingCheckBox->setChecked( defaults.pollingEnabled() );
    pollIntervalLineEdit->setText( QString::number( defaults.pollIntervalMs() ) );
    allowFollowOnScrollCheckBox->setChecked( defaults.allowFollowOnScroll() );
    setupPolling();

    const auto encodingIndex = encodingComboBox->findData( defaults.defaultEncodingMib() );
    encodingComboBox->setCurrentIndex( encodingIndex < 0 ? 0 : encodingIndex );
    filesHistoryMaxItemsSpinBox->setValue( defaultRecentFiles.filesHistoryMaxItems() );

    extractArchivesCheckBox->setChecked( defaults.extractArchives() );
    extractArchivesAlwaysCheckBox->setChecked( defaults.extractArchivesAlways() );
    setupArchives();

    verifySslCheckBox->setChecked( defaults.verifySslPeers() );
}

void OptionsDialog::resetLiveSourceDefaults()
{
    const Configuration defaults;

    adbExecutableLineEdit->setText( defaults.adbExecutable() );
    adbLogcatArgsLineEdit->setText( defaults.adbLogcatExtraArgs() );
    if ( adbAnsiOutputCheckBox_ ) {
        adbAnsiOutputCheckBox_->setChecked( defaults.adbLogcatAnsiOutputEnabled() );
    }
    if ( iosLogExecutableLineEdit_ ) {
        iosLogExecutableLineEdit_->setText( defaults.iosLogExecutable() );
    }
    if ( iosLogArgsLineEdit_ ) {
        iosLogArgsLineEdit_->setText( defaults.iosLogExtraArgs() );
    }
    if ( iosLogAnsiOutputCheckBox_ ) {
        iosLogAnsiOutputCheckBox_->setChecked( defaults.iosLogAnsiOutputEnabled() );
    }
    if ( liveSourceAutoReconnectCheckBox_ ) {
        liveSourceAutoReconnectCheckBox_->setChecked( defaults.liveAutoReconnectEnabled() );
    }
    if ( liveSourceMaxAttemptsSpinBox_ ) {
        liveSourceMaxAttemptsSpinBox_->setValue( defaults.liveAutoReconnectMaxAttempts() );
    }
    if ( liveSourceRollingMaxFileSizeSpinBox_ ) {
        liveSourceRollingMaxFileSizeSpinBox_->setValue(
            static_cast<int>( defaults.liveCaptureRollingMaxFileSize() / ( 1024 * 1024 ) ) );
    }
    if ( liveSourceRollingBackupCountSpinBox_ ) {
        liveSourceRollingBackupCountSpinBox_->setValue( defaults.liveCaptureRollingBackupCount() );
    }
}

void OptionsDialog::resetShortcutsDefaults()
{
    buildShortcutsTable( true );
}

void OptionsDialog::resetAdvancedDefaults()
{
    const Configuration defaults;

    regexpEngineComboBox->setCurrentIndex( getRegexpEngineIndex( defaults.regexpEngine() ) );
    parallelSearchCheckBox->setChecked( defaults.useParallelSearch() );
    searchResultsCacheCheckBox->setChecked( defaults.useSearchResultsCache() );
    searchCacheSpinBox->setValue( static_cast<int>( defaults.searchResultsCacheLines() ) );
    indexReadBufferSpinBox->setValue( defaults.indexReadBufferSizeMb() );
    searchReadBufferSpinBox->setValue( defaults.searchReadBufferSizeLines() );
    keepFileClosedCheckBox->setChecked( defaults.keepFileClosed() );
    compressedIndexCheckBox->setChecked( defaults.useCompressedIndex() );
    optimizeForNotLatinEncodingsCheckBox->setChecked( defaults.optimizeForNotLatinEncodings() );
    setupSearchResultsCache();

    loggingCheckBox->setChecked( defaults.enableLogging() );
    verbositySpinBox->setValue( defaults.loggingLevel() );
    setupLogging();
}

void OptionsDialog::checkShortcutsOnDuplicate() const
{
    static constexpr int PRIMARY_COL = 1;
    static constexpr int SECONDARY_COL = 2;

    if ( !shortcutsTable->rowCount() ) {
        return;
    }

    const auto DEFAULT_BACKGROUND = shortcutsTable->item( 0, PRIMARY_COL )->background();

    for ( auto shortcutRow = 0; shortcutRow < shortcutsTable->rowCount(); ++shortcutRow ) {
        shortcutsTable->item( shortcutRow, PRIMARY_COL )->setBackground( DEFAULT_BACKGROUND );
        shortcutsTable->item( shortcutRow, SECONDARY_COL )->setBackground( DEFAULT_BACKGROUND );
    }

    std::unordered_map<std::string, std::pair<int, int>> uniqueShortcuts;
    bool hasDuplicateShortcuts = false;
    for ( auto shortcutRow = 0; shortcutRow < shortcutsTable->rowCount(); ++shortcutRow ) {

        auto hasDuplicates = [ &uniqueShortcuts, shortcutRow, this ]( int ncol ) {
            auto keySequence = static_cast<KeySequencePresenter*>(
                                   shortcutsTable->cellWidget( shortcutRow, ncol ) )
                                   ->keySequence();

            if ( !keySequence.isEmpty() ) {
                if ( auto it = uniqueShortcuts.find( keySequence.toStdString() );
                     it != uniqueShortcuts.end() ) {

                    shortcutsTable->item( it->second.first, it->second.second )
                        ->setBackground( Qt::red );
                    shortcutsTable->item( shortcutRow, ncol )->setBackground( Qt::red );

                    return true;
                }

                uniqueShortcuts.try_emplace( keySequence.toStdString(),
                                             std::make_pair( shortcutRow, ncol ) );
            }

            return false;
        };

        if ( hasDuplicates( PRIMARY_COL ) || hasDuplicates( SECONDARY_COL ) ) {
            hasDuplicateShortcuts = true;
        }
    }

    buttonBox->button( QDialogButtonBox::Ok )->setEnabled( !hasDuplicateShortcuts );
    buttonBox->button( QDialogButtonBox::Apply )->setEnabled( !hasDuplicateShortcuts );
}

int OptionsDialog::updateTranslate()
{
    return MainWindow::installLanguage( languageComboBox->currentData().toString() );
}

void OptionsDialog::updateConfigFromDialog()
{
    bool restartAppMessage = false;
    auto& config = Configuration::get();

    bool fontSizeOk = false;
    auto fontPointSize = fontSizeBox->currentText().toInt( &fontSizeOk );
    if ( !fontSizeOk || fontPointSize <= 0 ) {
        fontPointSize = Configuration{}.mainFont().pointSize();
    }
    QFont font = QFont( fontFamilyBox->currentText(), fontPointSize );
    config.setMainFont( font );
    config.setLineSpacingPercent( lineSpacingSpinBox->value() );
    config.setForceFontAntialiasing( fontSmoothCheckBox->isChecked() );
    config.setUseBoldFont( boldFontCheckBox->isChecked() );
    config.setUseTextWrap( wrapTextCheckBox->isChecked() );
    config.setEnableQtHighDpi( enableQtHiDpiCheckBox->isChecked() );
    config.setScaleFactorRounding( scaleRoundingComboBox->currentIndex() + 1 );

    config.setMainRegexpType( getRegexpTypeFromIndex( mainSearchBox->currentIndex() ) );
    config.setMainSearchBackColor( mainSearchColor_ );
    config.setEnableMainSearchHighlight( highlightMainSearchCheckBox->isChecked() );
    config.setVariateMainSearchHighlight( variateHighlightCheckBox->isChecked() );
    config.setSearchIgnoreCaseDefault( !caseSensitiveCheckBox->isChecked() );
    config.setSearchAutoRefreshDefault( autoRefreshCheckBox->isChecked() );
    config.setSearchLogicalCombiningDefault( logicalCombiningCheckBox->isChecked() );
    config.setQuickfindRegexpType( getRegexpTypeFromIndex( quickFindSearchBox->currentIndex() ) );
    config.setQfBackColor( qfSearchColor_ );
    config.setQuickfindIncremental( incrementalCheckBox->isChecked() );
    config.setRegexpEnging( getRegexpEngineFromIndex( regexpEngineComboBox->currentIndex() ) );
    config.setAutoRunSearchOnPatternChange( autoRunSearchOnAddCheckBox->isChecked() );
    config.setShowAllInFilteredViewWhenSearchEmpty(
        showAllFilteredWhenEmptyCheckBox->isChecked() );

    config.setNativeFileWatchEnabled( nativeFileWatchCheckBox->isChecked() );
    config.setPollingEnabled( pollingCheckBox->isChecked() );
    auto pollInterval = pollIntervalLineEdit->text().toInt();
    if ( pollInterval < PollIntervalMin )
        pollInterval = PollIntervalMin;
    else if ( pollInterval > PollIntervalMax )
        pollInterval = PollIntervalMax;

    config.setPollIntervalMs( pollInterval );
    config.setFastModificationDetection( fastModificationDetectionCheckBox->isChecked() );
    config.setAllowFollowOnScroll( allowFollowOnScrollCheckBox->isChecked() );

    config.setLoadLastSession( loadLastSessionCheckBox->isChecked() );
    config.setFollowFileOnLoad( followFileOnLoadCheckBox->isChecked() );
    config.setAllowMultipleWindows( multipleWindowsCheckBox->isChecked() );
    config.setMinimizeToTray( minimizeToTrayCheckBox->isChecked() );
    config.setConfirmTabClose( confirmTabCloseCheckBox->isChecked() );
    config.setEnableLogging( loggingCheckBox->isChecked() );
    config.setLoggingLevel( verbositySpinBox->value() );
    
    // Apply logging settings immediately
    logging::enableFileLogging( config.enableLogging(),
                                static_cast<logging::LogLevel>( config.loggingLevel() ) );
    
    // Update log file path display
    setupLogging();

    config.setExtractArchives( extractArchivesCheckBox->isChecked() );
    config.setExtractArchivesAlways( extractArchivesAlwaysCheckBox->isChecked() );

    config.setUseParallelSearch( parallelSearchCheckBox->isChecked() );
    config.setUseSearchResultsCache( searchResultsCacheCheckBox->isChecked() );
    config.setSearchResultsCacheLines( static_cast<unsigned>( searchCacheSpinBox->value() ) );
    config.setIndexReadBufferSizeMb( indexReadBufferSpinBox->value() );
    config.setSearchReadBufferSizeLines( searchReadBufferSpinBox->value() );
    config.setKeepFileClosed( keepFileClosedCheckBox->isChecked() );
    config.setUseCompressedIndex( compressedIndexCheckBox->isChecked() );
    config.setOptimizeForNotLatinEncodings( optimizeForNotLatinEncodingsCheckBox->isChecked() );

    // version checking
    config.setVersionCheckingEnabled( checkForNewVersionCheckBox->isChecked() );

    config.setVerifySslPeers( verifySslCheckBox->isChecked() );
    config.setAdbExecutable( adbExecutableLineEdit->text().trimmed() );
    config.setAdbLogcatExtraArgs( adbLogcatArgsLineEdit->text().trimmed() );
    if ( adbAnsiOutputCheckBox_ ) {
        config.setAdbLogcatAnsiOutputEnabled( adbAnsiOutputCheckBox_->isChecked() );
    }
    if ( iosLogExecutableLineEdit_ ) {
        config.setIosLogExecutable( iosLogExecutableLineEdit_->text().trimmed() );
    }
    if ( iosLogArgsLineEdit_ ) {
        config.setIosLogExtraArgs( iosLogArgsLineEdit_->text().trimmed() );
    }
    if ( iosLogAnsiOutputCheckBox_ ) {
        config.setIosLogAnsiOutputEnabled( iosLogAnsiOutputCheckBox_->isChecked() );
    }
    if ( liveSourceAutoReconnectCheckBox_ ) {
        config.setLiveAutoReconnectEnabled( liveSourceAutoReconnectCheckBox_->isChecked() );
    }
    if ( liveSourceMaxAttemptsSpinBox_ ) {
        config.setLiveAutoReconnectMaxAttempts( liveSourceMaxAttemptsSpinBox_->value() );
    }
    if ( liveSourceRollingMaxFileSizeSpinBox_ ) {
        // Convert MB from UI back to bytes for Configuration
        config.setLiveCaptureRollingMaxFileSize(
            static_cast<qint64>( liveSourceRollingMaxFileSizeSpinBox_->value() ) * 1024 * 1024 );
    }
    if ( liveSourceRollingBackupCountSpinBox_ ) {
        config.setLiveCaptureRollingBackupCount( liveSourceRollingBackupCountSpinBox_->value() );
    }

    const auto selectedStyle = styleComboBox->currentData().toString();
    restartAppMessage = config.style() != selectedStyle;

    config.setStyle( selectedStyle );
    
    // Theme mode
    const auto themeMode = static_cast<ThemeMode>( themeModeComboBox->currentData().toInt() );
    const bool themeModeChanged = config.themeMode() != themeMode;
    config.setThemeMode( themeMode );
    
    // Apply theme immediately if changed
    if ( themeModeChanged ) {
        StyleManager::applyStyle( config.style() );
    }
    
    config.setHideAnsiColorSequences( hideAnsiColorsCheckBox->isChecked() );
    config.setRenderAnsiColorSequences( !hideAnsiColorsCheckBox->isChecked()
                                        && renderAnsiColorsCheckBox->isChecked() );

    config.setDefaultEncodingMib( encodingComboBox->currentData().toInt() );

    auto shortcuts = config.shortcuts();
    for ( auto shortcutRow = 0; shortcutRow < shortcutsTable->rowCount(); ++shortcutRow ) {
        QStringList actionKeys;

        auto primaryKeySequence
            = static_cast<KeySequencePresenter*>( shortcutsTable->cellWidget( shortcutRow, 1 ) )
                  ->keySequence();
        auto secondaryKeySequence
            = static_cast<KeySequencePresenter*>( shortcutsTable->cellWidget( shortcutRow, 2 ) )
                  ->keySequence();
        actionKeys << primaryKeySequence << secondaryKeySequence;

        auto action
            = shortcutsTable->item( shortcutRow, 0 )->data( Qt::UserRole ).toString().toStdString();
        shortcuts[ action ] = actionKeys;
    }
    config.setShortcuts( shortcuts );

    // update translate when accept or apply clicked
    restartAppMessage |= config.language() != languageComboBox->currentData().toString();
    updateTranslate();
    config.setLanguage( languageComboBox->currentData().toString() );
    retranslateUi( this );

    config.save();

    auto& savedSearches = SavedSearches::get();
    savedSearches.setHistorySize( searchHistorySpinBox->value() );
    savedSearches.save();

    auto& recentFiles = RecentFiles::get();
    recentFiles.setFilesHistoryMaxItems( filesHistoryMaxItemsSpinBox->value() );
    recentFiles.save();

    if ( restartAppMessage ) {
        QMessageBox::warning(
            this, "klogg",
            QApplication::translate( "OptionsDialog",
                                     "Klogg needs to be restarted to apply some changes. " ) );
    }

    Q_EMIT optionsChanged();
}

void OptionsDialog::onButtonBoxClicked( QAbstractButton* button )
{
    QDialogButtonBox::ButtonRole role = buttonBox->buttonRole( button );
    if ( ( role == QDialogButtonBox::AcceptRole ) || ( role == QDialogButtonBox::ApplyRole ) ) {
        updateConfigFromDialog();
    }

    if ( role == QDialogButtonBox::AcceptRole )
        accept();
    else if ( role == QDialogButtonBox::RejectRole )
        reject();
}

KeySequencePresenter::KeySequencePresenter( const QString& keySequence )
{
    keySequenceLabel_ = new QLabel();
    setKeySequence( keySequence );

    auto editButton = new QPushButton();
    editButton->setText( "..." );
    editButton->setFixedWidth( 50 );

    auto layout = new QHBoxLayout();

    connect( editButton, &QPushButton::clicked, this, &KeySequencePresenter::showEditor );
    layout->addWidget( keySequenceLabel_ );
    layout->addStretch();
    layout->addWidget( editButton );
    layout->setContentsMargins( 4, 4, 4, 4 );

    this->setLayout( layout );
}

QString KeySequencePresenter::keySequence() const
{
    return keySequence_;
}

void KeySequencePresenter::setKeySequence( const QString& keySequence )
{
    keySequence_ = QKeySequence( keySequence ).toString( QKeySequence::PortableText );
    keySequenceLabel_->setText( QKeySequence( keySequence_ ).toString( QKeySequence::NativeText ) );
}

void KeySequencePresenter::showEditor()
{
    QDialog keyEditDialog;

    auto label = new QLabel( "Press new key combination" );
    auto editor = new QKeySequenceEdit( QKeySequence( keySequence_ ) );
    auto clearButton = new QToolButton();
    clearButton->setText( "Clear" );
    auto dialogButtons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel );

    auto layout = new QVBoxLayout();
    layout->addWidget( label );
    auto editorLayout = new QHBoxLayout();
    editorLayout->addWidget( editor );
    editorLayout->addWidget( clearButton );
    layout->addLayout( editorLayout );
    layout->addWidget( dialogButtons );
    keyEditDialog.setLayout( layout );

    connect( clearButton, &QToolButton::clicked, editor, &QKeySequenceEdit::clear );
    connect( dialogButtons, &QDialogButtonBox::accepted, &keyEditDialog, &QDialog::accept );
    connect( dialogButtons, &QDialogButtonBox::rejected, &keyEditDialog, &QDialog::reject );

    if ( keyEditDialog.exec() == QDialog::Accepted ) {
        setKeySequence( editor->keySequence().toString( QKeySequence::PortableText ) );
        Q_EMIT edited(); // NOTE: it's important to emit this signal only after changing
                         // \keySequenceLabel_'s text
    }
}

void OptionsDialog::buildShortcutsTable( bool useDefaultsOnly )
{
    shortcutsTable->setRowCount( 0 );

    const auto& config = Configuration::get();
    auto shortcutList = ShortcutAction::defaultShortcutList();
    if ( !useDefaultsOnly ) {
        for ( const auto& [ action, keys ] : config.shortcuts() ) {
            shortcutList[ action ].keySequence = keys;
        }
    }

    for ( const auto& [ action, shortCut ] : shortcutList ) {
        auto currentRow = shortcutsTable->rowCount();
        shortcutsTable->insertRow( currentRow );

        auto keyItem = new QTableWidgetItem( shortCut.name );
        keyItem->setFlags( Qt::ItemIsEnabled | Qt::ItemIsSelectable );
        keyItem->setData( Qt::UserRole, QString::fromStdString( action ) );
        shortcutsTable->setItem( currentRow, 0, keyItem );

        auto primaryKeySequence = new KeySequencePresenter(
            shortCut.keySequence.size() > 0 ? shortCut.keySequence[ 0 ] : "" );
        shortcutsTable->setItem( currentRow, 1, new QTableWidgetItem );
        shortcutsTable->setCellWidget( currentRow, 1, primaryKeySequence );
        connect( primaryKeySequence, &KeySequencePresenter::edited, this,
                 &OptionsDialog::checkShortcutsOnDuplicate );

        auto secondaryKeySequence = new KeySequencePresenter(
            shortCut.keySequence.size() > 1 ? shortCut.keySequence[ 1 ] : "" );
        shortcutsTable->setItem( currentRow, 2, new QTableWidgetItem );
        shortcutsTable->setCellWidget( currentRow, 2, secondaryKeySequence );
        connect( secondaryKeySequence, &KeySequencePresenter::edited, this,
                 &OptionsDialog::checkShortcutsOnDuplicate );
    }

    shortcutsTable->horizontalHeader()->setSectionResizeMode( QHeaderView::Stretch );
    shortcutsTable->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::Interactive );
    shortcutsTable->horizontalHeader()->setMinimumSectionSize( 150 );
    shortcutsTable->resizeColumnToContents( 0 );
    shortcutsTable->setHorizontalHeaderItem( 0, new QTableWidgetItem( tr( "Action" ) ) );
    shortcutsTable->setHorizontalHeaderItem( 1, new QTableWidgetItem( tr( "Primary shortcut" ) ) );
    shortcutsTable->setHorizontalHeaderItem( 2,
                                             new QTableWidgetItem( tr( "Secondary shortcut" ) ) );

    // in case if user set duplicate keys and after restores defaults
    // it is need to enable back standard buttons
    checkShortcutsOnDuplicate();

    shortcutsTable->sortItems( 0 );
}
