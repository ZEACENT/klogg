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

#include <QCheckBox>
#include <QColorDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QMessageBox>
#include <QToolButton>
#include <QtGui>

#include <algorithm>

#include "encodings.h"
#include "fontutils.h"
#include "highlighteredit.h"
#include "highlighterset.h"
#include "log.h"
#include "logger.h"
#include "mainwindow.h"
#include "openfilehelper.h"
#include "recentfiles.h"
#include "savedsearches.h"
#include "shortcuts.h"
#include "styles.h"

#include "platform/platform_files.h"

#include "optionsdialog.h"
#include "ui_optionsdialog.h"
#include "uimessage.h"

static constexpr int PollIntervalMin = 10;
static constexpr int PollIntervalMax = 3600000;

OptionsDialog::~OptionsDialog() = default;

void OptionsDialog::showControlInstallationResult( const klogg::control::InstallResult& result )
{
    QStringList lines;
    if ( result.status == klogg::control::InstallStatus::PartialFailure )
        lines.append( tr( "Installation changed only partially. Inspect the paths below before retrying." ) );
    lines.append( result.message );
    if ( !result.executable.isEmpty() )
        lines.append( tr( "Bundled executable: %1" ).arg( result.executable ) );
    if ( !result.plannedPaths.isEmpty() )
        lines.append( tr( "Planned changes:\n%1" ).arg( result.plannedPaths.join( QLatin1Char( '\n' ) ) ) );
    if ( !result.changedPaths.isEmpty() )
        lines.append( tr( "Changed:\n%1" ).arg( result.changedPaths.join( QLatin1Char( '\n' ) ) ) );
    if ( !result.backupPaths.isEmpty() )
        lines.append( tr( "Backups:\n%1" ).arg( result.backupPaths.join( QLatin1Char( '\n' ) ) ) );
    if ( result.restartRequired )
        lines.append( tr( "Running shells keep their environment. Restart your shell; if PATH has not refreshed, sign out and back in." ) );
    ui_->controlCliStatusLabel->setText( lines.join( QLatin1Char( '\n' ) ) );
}

void OptionsDialog::checkControlInstallation()
{
    showControlInstallationResult( controlInstaller_( klogg::control::InstallAction::Check, {} ) );
}

void OptionsDialog::changeControlInstallation( klogg::control::InstallAction action )
{
    using namespace klogg::control;
    const bool removing = action == InstallAction::Remove;
    const auto preview = controlInstaller_(
        removing ? InstallAction::CheckRemoval : InstallAction::Check, {} );
    showControlInstallationResult( preview );
    const bool ready = removing ? preview.status == InstallStatus::Installed
                               : ( preview.status == InstallStatus::NotInstalled
                                   || preview.status == InstallStatus::NeedsRepair );
    if ( !ready || preview.plannedPaths.isEmpty() ) return;

    QMessageBox confirmation( QMessageBox::Question, tr( "kloggctl shell installation" ),
        ( removing ? tr( "Remove only the managed shell installation at these paths?\n%1" )
                   : tr( "Install or repair shell access using these paths?\n%1" ) )
            .arg( preview.plannedPaths.join( QLatin1Char( '\n' ) ) ),
        QMessageBox::Yes | QMessageBox::No, this );
    confirmation.setTextFormat( Qt::PlainText );
    confirmation.setDefaultButton( QMessageBox::No );
    if ( klogg::ui::execDialog( confirmation ) != QMessageBox::Yes ) return;
    showControlInstallationResult( controlInstaller_( action, preview.plannedPaths ) );
}

void OptionsDialog::installControlCli()
{
    changeControlInstallation( klogg::control::InstallAction::Install );
}

void OptionsDialog::removeControlCli()
{
    changeControlInstallation( klogg::control::InstallAction::Remove );
}

// Constructor
OptionsDialog::OptionsDialog( QWidget* parent, ControlInstaller controlInstaller )
    : QDialog( parent ), ui_( std::make_unique<Ui::OptionsDialog>() ),
      controlInstaller_( std::move( controlInstaller ) )
{
    ui_->setupUi( this );
    if ( !controlInstaller_ ) {
        // Resolve only on explicit action, and retain the same options across
        // preview/confirmation. Never select an unrelated executable from PATH.
        controlInstaller_ = [options = std::optional<klogg::control::InstallOptions>{}](
            klogg::control::InstallAction action, const QStringList& approvedPaths ) mutable {
            using namespace klogg::control;
            const bool preview = action == InstallAction::Check || action == InstallAction::CheckRemoval;
            if ( preview ) {
                options = defaultInstallOptions( QDir( QCoreApplication::applicationDirPath() )
                    .filePath( QStringLiteral( KLOGG_CONTROL_EXECUTABLE_NAME ) ) );
            }
            if ( !options ) {
                InstallResult result;
                result.status = InstallStatus::Unsupported;
                result.message = tr( "Automatic shell installation is unavailable for this environment. Use the bundled kloggctl by its full path." );
                return result;
            }
            return configureControlInstallation( *options, action, nullptr,
                                                 preview ? nullptr : &approvedPaths );
        };
    }
    connect( ui_->controlCliCheckButton, &QPushButton::clicked,
             this, &OptionsDialog::checkControlInstallation );
    connect( ui_->controlCliInstallButton, &QPushButton::clicked,
             this, &OptionsDialog::installControlCli );
    connect( ui_->controlCliRemoveButton, &QPushButton::clicked,
             this, &OptionsDialog::removeControlCli );
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
    // Parent the validator to the dialog so Qt parent-child ownership reclaims
    // it; QLineEdit::setValidator() does not take ownership, so an unparented
    // validator would leak for every OptionsDialog instance (seen as an
    // LeakSanitizer exit leak in the unit tests).
    QValidator* pollingIntervalValidator
        = new QIntValidator( PollIntervalMin, PollIntervalMax, this );
    ui_->pollIntervalLineEdit->setValidator( pollingIntervalValidator );

    connect( ui_->buttonBox, &QDialogButtonBox::clicked, this, &OptionsDialog::onButtonBoxClicked );
    connect( ui_->fontFamilyBox, &QComboBox::currentTextChanged, this, &OptionsDialog::updateFontSize );
    connect( ui_->pollingCheckBox, &QCheckBox::toggled, [ this ]( auto ) { this->setupPolling(); } );
    connect( ui_->searchResultsCacheCheckBox, &QCheckBox::toggled,
             [ this ]( auto ) { this->setupSearchResultsCache(); } );
    connect( ui_->loggingCheckBox, &QCheckBox::toggled, [ this ]( auto ) { this->setupLogging(); } );
    connect( ui_->openLogFileButton, &QPushButton::clicked, this, &OptionsDialog::openLogFile );

    connect( ui_->extractArchivesCheckBox, &QCheckBox::toggled,
             [ this ]( auto ) { this->setupArchives(); } );
    connect( ui_->hideAnsiColorsCheckBox, &QCheckBox::toggled, this, [ this ]( bool checked ) {
        if ( checked ) {
            ui_->renderAnsiColorsCheckBox->setChecked( false );
        }
    } );
    connect( ui_->renderAnsiColorsCheckBox, &QCheckBox::toggled, this, [ this ]( bool checked ) {
        if ( checked ) {
            ui_->hideAnsiColorsCheckBox->setChecked( false );
        }
    } );

    connect( ui_->mainSearchColorButton, &QPushButton::clicked, this, &OptionsDialog::changeMainColor );
    connect( ui_->quickFindColorButton, &QPushButton::clicked, this, &OptionsDialog::changeQfColor );

    ui_->restoreShortcutsDefaults->setText( tr( "Reset to defaults" ) );
    connect( ui_->restoreShortcutsDefaults, &QPushButton::clicked, this,
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
    if ( !klogg::platform::hasExclusiveFileLocks ) {
        ui_->keepFileClosedCheckBox->setVisible( false );
    }

#ifdef Q_OS_MAC
    ui_->minimizeToTrayCheckBox->setVisible( false );
#endif

#ifndef KLOGG_HAS_VECTORSCAN
    ui_->regexpEngineLabel->setVisible( false );
    ui_->regexpEngineComboBox->setVisible( false );
#endif
}

// Populates the 'family' ComboBox
void OptionsDialog::setupFontList()
{
    const auto families = FontUtils::availableFonts();
    for ( const QString& str : families ) {
        ui_->fontFamilyBox->addItem( str );
    }
}

// Populate the regexp ComboBoxes
void OptionsDialog::setupRegexp()
{
    QStringList regexpTypes;
    regexpTypes << tr( "Extended Regexp" ) << tr( "Fixed Strings" );

    ui_->mainSearchBox->addItems( regexpTypes );
    ui_->quickFindSearchBox->addItems( regexpTypes );

    QStringList regexpEngines;
    regexpEngines << tr( "Vectorscan" ) << tr( "Qt" );

    ui_->regexpEngineComboBox->addItems( regexpEngines );
}

void OptionsDialog::setupStyles()
{
    ui_->styleComboBox->clear();
    ui_->styleComboBox->addItem( tr( "Modern" ), StyleManager::ModernKey );
    ui_->styleComboBox->addItem( tr( "System" ), StyleManager::SystemKey );
    ui_->styleComboBox->addItem( tr( "Classic Dark" ), StyleManager::DarkStyleKey );

    // Setup theme mode combo box
    ui_->themeModeComboBox->addItem( tr( "Light" ), static_cast<int>( ThemeMode::Light ) );
    ui_->themeModeComboBox->addItem( tr( "Dark" ), static_cast<int>( ThemeMode::Dark ) );
    ui_->themeModeComboBox->addItem( tr( "Auto (Follow System)" ), static_cast<int>( ThemeMode::Auto ) );

    // "Classic Dark" is an inherently dark style; the theme mode selector has
    // no effect on it, so disable it (and explain why) instead of presenting a
    // dead control. The theme mode still applies to "Modern" and "System".
    connect( ui_->styleComboBox, qOverload<int>( &QComboBox::currentIndexChanged ), this,
             [ this ]( int ) { updateThemeModeAvailability(); } );
    updateThemeModeAvailability();
}

void OptionsDialog::updateThemeModeAvailability()
{
    const bool classicDark = ui_->styleComboBox->currentData() == StyleManager::DarkStyleKey;

    if ( classicDark && !themeModePinnedToDark_ ) {
        // Entering Classic Dark: remember the theme-mode the user had selected
        // so it can be restored when leaving, then pin the selector to Dark.
        // An inherently dark style makes the mode selector a no-op, and leaving
        // it showing e.g. "Auto" is confusing.
        themeModeRestore_ = static_cast<ThemeMode>( ui_->themeModeComboBox->currentData().toInt() );
        const int darkModeIndex
            = ui_->themeModeComboBox->findData( static_cast<int>( ThemeMode::Dark ) );
        if ( darkModeIndex != -1 ) {
            ui_->themeModeComboBox->setCurrentIndex( darkModeIndex );
        }
        themeModePinnedToDark_ = true;
    }
    else if ( !classicDark && themeModePinnedToDark_ ) {
        // Leaving Classic Dark: restore the theme-mode selected before it.
        themeModePinnedToDark_ = false;
        if ( themeModeRestore_ ) {
            const int restoreIndex
                = ui_->themeModeComboBox->findData( static_cast<int>( *themeModeRestore_ ) );
            if ( restoreIndex != -1 ) {
                ui_->themeModeComboBox->setCurrentIndex( restoreIndex );
            }
            themeModeRestore_.reset();
        }
    }

    ui_->themeModeComboBox->setEnabled( !classicDark );
    ui_->themeModeLabel->setEnabled( !classicDark );
    ui_->themeModeHintLabel->setVisible( classicDark );
}

void OptionsDialog::setupEncodings()
{
    const auto availableEncodings = EncodingMenu::supportedEncodings();
    ui_->encodingComboBox->addItem( "Auto", -1 );

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
        ui_->encodingComboBox->addItem( codec.first, codec.second );
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
            ui_->languageComboBox->addItem( attributes.value( "name" ).toString(),
                                       attributes.value( "ietfCode" ).toString() );
        }
    }
}

void OptionsDialog::setupIosLogSettings()
{
    // New live sessions are composed exclusively through application-owned
    // transports. Preferences expose presentation policy only; executable
    // paths, environment detection and free-form arguments are compatibility-
    // backend concerns and have no editable UI.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): parent widget owns this control.
    adbAnsiOutputCheckBox_ = new QCheckBox( tr( "Enable ANSI color output" ), ui_->adbGroupBox );
    adbAnsiOutputCheckBox_->setObjectName( QStringLiteral( "adbAnsiOutputCheckBox" ) );
    if ( auto* adbLayout = qobject_cast<QVBoxLayout*>( ui_->adbGroupBox->layout() ) ) {
        adbLayout->addWidget( adbAnsiOutputCheckBox_ );
    }

    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): parent widget owns this control.
    iosLogGroupBox_ = new QGroupBox( tr( "iOS Log Stream" ), ui_->liveSourceTab );
    iosLogGroupBox_->setObjectName( QStringLiteral( "iosLogGroupBox" ) );

    auto* layout = new QVBoxLayout( iosLogGroupBox_ );
    layout->setSpacing( 6 );
    iosLogAnsiOutputCheckBox_ = new QCheckBox( tr( "Enable ANSI color output" ), iosLogGroupBox_ );
    iosLogAnsiOutputCheckBox_->setObjectName( QStringLiteral( "iosLogAnsiOutputCheckBox" ) );
    layout->addWidget( iosLogAnsiOutputCheckBox_ );

    const auto insertIndex = std::max( 0, ui_->verticalLayout_liveSource->count() - 1 );
    ui_->verticalLayout_liveSource->insertWidget( insertIndex, iosLogGroupBox_ );

#ifndef Q_OS_MAC
    iosLogGroupBox_->setVisible( false );
#endif
}

void OptionsDialog::setupLiveSourceSettings()
{
    // Live Source group box with auto-reconnect and capture file rolling settings.
    // These settings control the behavior of live log streams (ADB logcat, iOS log).
    // Inserted on the File tab alongside the ADB and iOS log stream settings.

    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): parent widget owns this control.
    liveSourceGroupBox_ = new QGroupBox( tr( "Live Source" ), ui_->liveSourceTab );
    liveSourceGroupBox_->setObjectName( QStringLiteral( "liveSourceGroupBox" ) );

    auto* layout = new QVBoxLayout( liveSourceGroupBox_ );
    layout->setSpacing( 6 );

    // Auto-reconnect toggle — enables/disables automatic reconnection when the
    // live source unexpectedly disconnects or encounters an error. Uses
    // exponential backoff starting at 1 second and capping at 30 seconds
    // between attempts.
    liveSourceAutoReconnectCheckBox_ // NOLINT(cppcoreguidelines-owning-memory)
        = new QCheckBox( tr( "Enable auto-reconnect on connection loss" ), liveSourceGroupBox_ );
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
    maxFileSizeLabel->setToolTip( tr( "When the capture file exceeds this size, it is rotated. "
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
    // rotation. Files beyond this count are deleted. Set to 0 to keep every
    // rotated file.
    auto* backupCountRow = new QHBoxLayout();
    auto* backupCountLabel = new QLabel( tr( "Rolling backup count" ), liveSourceGroupBox_ );
    backupCountLabel->setToolTip( tr( "Number of old capture files to keep when rolling. "
                                      "Older files beyond this count are automatically deleted. "
                                      "Set to 0 to keep all rotated files." ) );
    liveSourceRollingBackupCountSpinBox_ = new QSpinBox( liveSourceGroupBox_ );
    liveSourceRollingBackupCountSpinBox_->setObjectName(
        QStringLiteral( "liveSourceRollingBackupCountSpinBox" ) );
    liveSourceRollingBackupCountSpinBox_->setRange( 0, 999 );
    liveSourceRollingBackupCountSpinBox_->setSpecialValueText( tr( "Keep all" ) );
    liveSourceRollingBackupCountSpinBox_->setToolTip(
        tr( "Number of backup capture files to retain during rotation. "
            "Older files beyond this count are deleted. "
            "Set to 0 to keep all rotated files." ) );
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
    const auto insertIndex = std::max( 0, ui_->verticalLayout_liveSource->count() - 1 );
    ui_->verticalLayout_liveSource->insertWidget( insertIndex, liveSourceGroupBox_ );
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

    addResetButton( ui_->general_tab, QStringLiteral( "resetGeneralDefaultsButton" ),
                    &OptionsDialog::resetGeneralDefaults );
    addResetButton( ui_->viewTab, QStringLiteral( "resetViewDefaultsButton" ),
                    &OptionsDialog::resetViewDefaults );
    addResetButton( ui_->file_watch_tab, QStringLiteral( "resetFileDefaultsButton" ),
                    &OptionsDialog::resetFileDefaults );
    addResetButton( ui_->liveSourceTab, QStringLiteral( "resetLiveSourceDefaultsButton" ),
                    &OptionsDialog::resetLiveSourceDefaults );
    addResetButton( ui_->advanced_tab, QStringLiteral( "resetAdvancedDefaultsButton" ),
                    &OptionsDialog::resetAdvancedDefaults );
}

// Apply uniform vertical spacing to every group-box layout in every tab.
// This prevents "line overlap" caused by inconsistent or missing spacing
// between widgets inside group boxes.
void OptionsDialog::standardizeLayoutSpacing()
{
    static constexpr int kGroupBoxLayoutSpacing = 6;
    static constexpr int kTabLayoutSpacing = 6;

    const QList<QWidget*> tabs
        = { ui_->general_tab, ui_->viewTab, ui_->file_watch_tab, ui_->liveSourceTab, ui_->shortcutsTab, ui_->advanced_tab };

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
    ui_->pollIntervalLineEdit->setEnabled( ui_->pollingCheckBox->isChecked() );
}

void OptionsDialog::setupSearchResultsCache()
{
    ui_->searchCacheSpinBox->setEnabled( ui_->searchResultsCacheCheckBox->isChecked() );
}

void OptionsDialog::setupLogging()
{
    ui_->verbositySpinBox->setEnabled( ui_->loggingCheckBox->isChecked() );

    // Update log file path display
    if ( ui_->loggingCheckBox->isChecked() ) {
        const auto logFilePath = logging::getLogFilePath();
        if ( !logFilePath.isEmpty() ) {
            ui_->logFilePathLabel->setText( tr( "Log file: %1" ).arg( logFilePath ) );
            ui_->logFilePathLabel->setVisible( true );
            ui_->openLogFileButton->setVisible( true );
            ui_->openLogFileButton->setEnabled( true );
        }
        else {
            ui_->logFilePathLabel->setText( tr( "Log file: Console output only" ) );
            ui_->logFilePathLabel->setVisible( true );
            ui_->openLogFileButton->setVisible( false );
        }
    }
    else {
        ui_->logFilePathLabel->setVisible( false );
        ui_->openLogFileButton->setVisible( false );
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
    ui_->extractArchivesAlwaysCheckBox->setEnabled( ui_->extractArchivesCheckBox->isChecked() );
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

    int familyIndex = ui_->fontFamilyBox->findText( configuredFamily );
    if ( familyIndex == -1 && !configuredFamily.isEmpty() ) {
        ui_->fontFamilyBox->addItem( configuredFamily );
        familyIndex = ui_->fontFamilyBox->findText( configuredFamily );
    }
    if ( familyIndex != -1 ) {
        ui_->fontFamilyBox->setCurrentIndex( familyIndex );
    }

    updateFontSizePreservingSelection( ui_->fontFamilyBox->currentText(), configuredPointSize );

    ui_->lineSpacingSpinBox->setValue( config.lineSpacingPercent() );
    ui_->fontSmoothCheckBox->setChecked( config.forceFontAntialiasing() );
    ui_->boldFontCheckBox->setChecked( config.useBoldFont() );
    ui_->wrapTextCheckBox->setChecked( config.useTextWrap() );
    ui_->enableQtHiDpiCheckBox->setChecked( config.enableQtHighDpi() );
    ui_->scaleRoundingComboBox->setCurrentIndex( config.scaleFactorRounding() - 1 );

    // Language
    auto langIdx = ui_->languageComboBox->findData( { config.language() } );
    if ( langIdx == -1 ) {
        langIdx = 0;
    }
    ui_->languageComboBox->setCurrentIndex( langIdx );

    // Theme mode is set before the style: selecting Classic Dark pins the theme
    // selector to Dark via the style combo's change handler, which must read
    // the configured mode here rather than the pre-init default.
    const auto themeMode = config.themeMode();
    const int themeModeIndex = ui_->themeModeComboBox->findData( static_cast<int>( themeMode ) );
    if ( themeModeIndex != -1 ) {
        ui_->themeModeComboBox->setCurrentIndex( themeModeIndex );
    }
    else {
        ui_->themeModeComboBox->setCurrentIndex( 2 ); // Default to Auto
    }

    const auto style = config.style();
    const int styleIndex = ui_->styleComboBox->findData( style );
    ui_->styleComboBox->setCurrentIndex( styleIndex == -1 ? 0 : styleIndex );

    // Reflect the saved style in the theme-selector availability (the style
    // combo's own index change already fires the handler, but not when the
    // saved style matches the current selection).
    updateThemeModeAvailability();

    ui_->hideAnsiColorsCheckBox->setChecked( config.hideAnsiColorSequences() );
    ui_->renderAnsiColorsCheckBox->setChecked( !config.hideAnsiColorSequences()
                                          && config.renderAnsiColorSequences() );

    // Regexp types
    ui_->mainSearchBox->setCurrentIndex( getRegexpTypeIndex( config.mainRegexpType() ) );
    mainSearchColor_ = config.mainSearchBackColor();
    HighlighterEdit::updateIcon( ui_->mainSearchColorButton, mainSearchColor_ );
    ui_->quickFindSearchBox->setCurrentIndex( getRegexpTypeIndex( config.quickfindRegexpType() ) );
    qfSearchColor_ = config.qfBackColor();
    HighlighterEdit::updateIcon( ui_->quickFindColorButton, qfSearchColor_ );
    ui_->regexpEngineComboBox->setCurrentIndex( getRegexpEngineIndex( config.regexpEngine() ) );
    ui_->autoRunSearchOnAddCheckBox->setChecked( config.autoRunSearchOnPatternChange() );
    ui_->showAllFilteredWhenEmptyCheckBox->setChecked( config.showAllInFilteredViewWhenSearchEmpty() );

    ui_->highlightMainSearchCheckBox->setChecked( config.mainSearchHighlight() );
    ui_->variateHighlightCheckBox->setChecked( config.variateMainSearchHighlight() );
    ui_->incrementalCheckBox->setChecked( config.isQuickfindIncremental() );
    ui_->caseSensitiveCheckBox->setChecked( !config.isSearchIgnoreCaseDefault() );
    ui_->logicalCombiningCheckBox->setChecked( config.isSearchLogicalCombiningDefault() );
    ui_->autoRefreshCheckBox->setChecked( config.isSearchAutoRefreshDefault() );

    // Color label match defaults live in HighlighterSetCollection (the single
    // source of truth shared with the context-menu toggles), not in
    // Configuration; the dialog only mirrors them.
    const auto colorLabelDefaults = HighlighterSetCollection::get().quickHighlighterDefaults();
    ui_->colorLabelsIgnoreCaseCheckBox->setChecked( colorLabelDefaults.ignoreCase );
    ui_->colorLabelsWholeWordCheckBox->setChecked( colorLabelDefaults.wholeWord );

    // Polling
    ui_->nativeFileWatchCheckBox->setChecked( config.nativeFileWatchEnabled() );
    ui_->fastModificationDetectionCheckBox->setChecked( config.fastModificationDetection() );
    ui_->pollingCheckBox->setChecked( config.pollingEnabled() );
    ui_->pollIntervalLineEdit->setText( QString::number( config.pollIntervalMs() ) );
    ui_->allowFollowOnScrollCheckBox->setChecked( config.allowFollowOnScroll() );

    // Last session
    ui_->loadLastSessionCheckBox->setChecked( config.loadLastSession() );
    ui_->followFileOnLoadCheckBox->setChecked( config.followFileOnLoad() );
    ui_->minimizeToTrayCheckBox->setChecked( config.minimizeToTray() );
    ui_->multipleWindowsCheckBox->setChecked( config.allowMultipleWindows() );
    ui_->confirmTabCloseCheckBox->setChecked( config.confirmTabClose() );
    ui_->localControlEnabledCheckBox->setChecked( config.localControlEnabled() );

    ui_->loggingCheckBox->setChecked( config.enableLogging() );
    ui_->verbositySpinBox->setValue( config.loggingLevel() );

    // Apply logging settings to get log file path
    logging::enableFileLogging( config.enableLogging(),
                                static_cast<logging::LogLevel>( config.loggingLevel() ) );

    // Update log file path display immediately
    setupLogging();

    ui_->extractArchivesCheckBox->setChecked( config.extractArchives() );
    ui_->extractArchivesAlwaysCheckBox->setChecked( config.extractArchivesAlways() );

    // Perf
    ui_->parallelSearchCheckBox->setChecked( config.useParallelSearch() );
    ui_->searchResultsCacheCheckBox->setChecked( config.useSearchResultsCache() );
    ui_->searchCacheSpinBox->setValue( static_cast<int>( config.searchResultsCacheLines() ) );
    ui_->indexReadBufferSpinBox->setValue( config.indexReadBufferSizeMb() );
    ui_->searchReadBufferSpinBox->setValue( config.searchReadBufferSizeLines() );
    ui_->keepFileClosedCheckBox->setChecked( config.keepFileClosed() );
    ui_->compressedIndexCheckBox->setChecked( config.useCompressedIndex() );
    ui_->optimizeForNotLatinEncodingsCheckBox->setChecked( config.optimizeForNotLatinEncodings() );

    // version checking
    ui_->checkForNewVersionCheckBox->setChecked( config.versionCheckingEnabled() );

    // downloads
    ui_->verifySslCheckBox->setChecked( config.verifySslPeers() );
    if ( adbAnsiOutputCheckBox_ ) {
        adbAnsiOutputCheckBox_->setChecked( config.adbLogcatAnsiOutputEnabled() );
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
        liveSourceRollingMaxFileSizeSpinBox_->setValue(
            config.liveCaptureRollingMaxFileSizeMb() );
    }
    if ( liveSourceRollingBackupCountSpinBox_ ) {
        liveSourceRollingBackupCountSpinBox_->setValue( config.liveCaptureRollingBackupCount() );
    }

    const auto encodingIndex = ui_->encodingComboBox->findData( config.defaultEncodingMib() );
    ui_->encodingComboBox->setCurrentIndex( encodingIndex < 0 ? 0 : encodingIndex );

    buildShortcutsTable( false );

    const auto& savedSearches = SavedSearches::get();
    ui_->searchHistorySpinBox->setValue( savedSearches.historySize() );

    const auto& recentFiles = RecentFiles::get();
    ui_->filesHistoryMaxItemsSpinBox->setMinimum( 1 );
    ui_->filesHistoryMaxItemsSpinBox->setMaximum( MAX_RECENT_FILES );
    ui_->filesHistoryMaxItemsSpinBox->setValue( recentFiles.filesHistoryMaxItems() );
}

//
// Q_SLOTS:
//

void OptionsDialog::updateFontSize( const QString& fontFamily )
{
    bool ok = false;
    const auto oldFontSize = ui_->fontSizeBox->currentText().toInt( &ok );
    updateFontSizePreservingSelection( fontFamily, ok ? oldFontSize : -1 );
}

void OptionsDialog::updateFontSizePreservingSelection( const QString& fontFamily,
                                                       int preferredPointSize )
{
    const auto sizes = FontUtils::availableFontSizes( fontFamily );

    ui_->fontSizeBox->clear();
    for ( int size : sizes ) {
        ui_->fontSizeBox->addItem( QString::number( size ) );
    }

    if ( preferredPointSize > 0
         && ui_->fontSizeBox->findText( QString::number( preferredPointSize ) ) == -1 ) {
        auto insertIndex = 0;
        while ( insertIndex < ui_->fontSizeBox->count()
                && ui_->fontSizeBox->itemText( insertIndex ).toInt() < preferredPointSize ) {
            ++insertIndex;
        }
        ui_->fontSizeBox->insertItem( insertIndex, QString::number( preferredPointSize ) );
    }

    // Now restore the size we had before
    const int sizeIndex = ui_->fontSizeBox->findText( QString::number( preferredPointSize ) );
    if ( sizeIndex != -1 ) {
        ui_->fontSizeBox->setCurrentIndex( sizeIndex );
    }
}

void OptionsDialog::changeMainColor()
{
    QColor newColor;
    if ( HighlighterEdit::showColorPicker( mainSearchColor_, newColor ) ) {
        mainSearchColor_ = newColor;
        HighlighterEdit::updateIcon( ui_->mainSearchColorButton, mainSearchColor_ );
    }
}

void OptionsDialog::changeQfColor()
{
    QColor newColor;
    if ( HighlighterEdit::showColorPicker( qfSearchColor_, newColor ) ) {
        qfSearchColor_ = newColor;
        HighlighterEdit::updateIcon( ui_->quickFindColorButton, qfSearchColor_ );
    }
}

void OptionsDialog::resetGeneralDefaults()
{
    const Configuration defaults;
    const SavedSearches defaultSavedSearches;

    ui_->mainSearchBox->setCurrentIndex( getRegexpTypeIndex( defaults.mainRegexpType() ) );
    mainSearchColor_ = defaults.mainSearchBackColor();
    HighlighterEdit::updateIcon( ui_->mainSearchColorButton, mainSearchColor_ );
    ui_->quickFindSearchBox->setCurrentIndex( getRegexpTypeIndex( defaults.quickfindRegexpType() ) );
    qfSearchColor_ = defaults.qfBackColor();
    HighlighterEdit::updateIcon( ui_->quickFindColorButton, qfSearchColor_ );

    ui_->highlightMainSearchCheckBox->setChecked( defaults.mainSearchHighlight() );
    ui_->variateHighlightCheckBox->setChecked( defaults.variateMainSearchHighlight() );
    ui_->incrementalCheckBox->setChecked( defaults.isQuickfindIncremental() );
    ui_->caseSensitiveCheckBox->setChecked( !defaults.isSearchIgnoreCaseDefault() );
    ui_->logicalCombiningCheckBox->setChecked( defaults.isSearchLogicalCombiningDefault() );
    ui_->autoRefreshCheckBox->setChecked( defaults.isSearchAutoRefreshDefault() );
    ui_->autoRunSearchOnAddCheckBox->setChecked( defaults.autoRunSearchOnPatternChange() );
    ui_->showAllFilteredWhenEmptyCheckBox->setChecked( defaults.showAllInFilteredViewWhenSearchEmpty() );

    // Compiled defaults (ignore case on, whole word off) come from the
    // QuickHighlighterDefaults struct itself, not from the stored collection.
    const QuickHighlighterDefaults defaultColorLabelDefaults;
    ui_->colorLabelsIgnoreCaseCheckBox->setChecked( defaultColorLabelDefaults.ignoreCase );
    ui_->colorLabelsWholeWordCheckBox->setChecked( defaultColorLabelDefaults.wholeWord );

    ui_->searchHistorySpinBox->setValue( defaultSavedSearches.historySize() );

    ui_->loadLastSessionCheckBox->setChecked( defaults.loadLastSession() );
    ui_->followFileOnLoadCheckBox->setChecked( defaults.followFileOnLoad() );
    ui_->minimizeToTrayCheckBox->setChecked( defaults.minimizeToTray() );
    ui_->multipleWindowsCheckBox->setChecked( defaults.allowMultipleWindows() );
    ui_->confirmTabCloseCheckBox->setChecked( defaults.confirmTabClose() );
    ui_->localControlEnabledCheckBox->setChecked( defaults.localControlEnabled() );

    ui_->checkForNewVersionCheckBox->setChecked( defaults.versionCheckingEnabled() );
}

void OptionsDialog::resetViewDefaults()
{
    const Configuration defaults;

    const auto defaultFont = defaults.mainFont();
    auto familyIndex = ui_->fontFamilyBox->findText( defaultFont.family() );
    if ( familyIndex == -1 ) {
        ui_->fontFamilyBox->addItem( defaultFont.family() );
        familyIndex = ui_->fontFamilyBox->findText( defaultFont.family() );
    }
    if ( familyIndex != -1 ) {
        ui_->fontFamilyBox->setCurrentIndex( familyIndex );
    }
    updateFontSizePreservingSelection( ui_->fontFamilyBox->currentText(), defaultFont.pointSize() );
    ui_->lineSpacingSpinBox->setValue( defaults.lineSpacingPercent() );
    ui_->fontSmoothCheckBox->setChecked( defaults.forceFontAntialiasing() );
    ui_->boldFontCheckBox->setChecked( defaults.useBoldFont() );
    ui_->wrapTextCheckBox->setChecked( defaults.useTextWrap() );

    auto langIdx = ui_->languageComboBox->findData( defaults.language() );
    ui_->languageComboBox->setCurrentIndex( langIdx == -1 ? 0 : langIdx );

    const auto styleIndex = ui_->styleComboBox->findData( defaults.style() );
    ui_->styleComboBox->setCurrentIndex( styleIndex == -1 ? 0 : styleIndex );
    const auto themeModeIndex
        = ui_->themeModeComboBox->findData( static_cast<int>( defaults.themeMode() ) );
    ui_->themeModeComboBox->setCurrentIndex( themeModeIndex == -1 ? 0 : themeModeIndex );

    ui_->enableQtHiDpiCheckBox->setChecked( defaults.enableQtHighDpi() );
    ui_->scaleRoundingComboBox->setCurrentIndex( defaults.scaleFactorRounding() - 1 );
    ui_->hideAnsiColorsCheckBox->setChecked( defaults.hideAnsiColorSequences() );
    ui_->renderAnsiColorsCheckBox->setChecked( !defaults.hideAnsiColorSequences()
                                          && defaults.renderAnsiColorSequences() );
}

void OptionsDialog::resetFileDefaults()
{
    const Configuration defaults;
    const RecentFiles defaultRecentFiles;

    ui_->nativeFileWatchCheckBox->setChecked( defaults.nativeFileWatchEnabled() );
    ui_->fastModificationDetectionCheckBox->setChecked( defaults.fastModificationDetection() );
    ui_->pollingCheckBox->setChecked( defaults.pollingEnabled() );
    ui_->pollIntervalLineEdit->setText( QString::number( defaults.pollIntervalMs() ) );
    ui_->allowFollowOnScrollCheckBox->setChecked( defaults.allowFollowOnScroll() );
    setupPolling();

    const auto encodingIndex = ui_->encodingComboBox->findData( defaults.defaultEncodingMib() );
    ui_->encodingComboBox->setCurrentIndex( encodingIndex < 0 ? 0 : encodingIndex );
    ui_->filesHistoryMaxItemsSpinBox->setValue( defaultRecentFiles.filesHistoryMaxItems() );

    ui_->extractArchivesCheckBox->setChecked( defaults.extractArchives() );
    ui_->extractArchivesAlwaysCheckBox->setChecked( defaults.extractArchivesAlways() );
    setupArchives();

    ui_->verifySslCheckBox->setChecked( defaults.verifySslPeers() );
}

void OptionsDialog::resetLiveSourceDefaults()
{
    const Configuration defaults;

    if ( adbAnsiOutputCheckBox_ ) {
        adbAnsiOutputCheckBox_->setChecked( defaults.adbLogcatAnsiOutputEnabled() );
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

    ui_->regexpEngineComboBox->setCurrentIndex( getRegexpEngineIndex( defaults.regexpEngine() ) );
    ui_->parallelSearchCheckBox->setChecked( defaults.useParallelSearch() );
    ui_->searchResultsCacheCheckBox->setChecked( defaults.useSearchResultsCache() );
    ui_->searchCacheSpinBox->setValue( static_cast<int>( defaults.searchResultsCacheLines() ) );
    ui_->indexReadBufferSpinBox->setValue( defaults.indexReadBufferSizeMb() );
    ui_->searchReadBufferSpinBox->setValue( defaults.searchReadBufferSizeLines() );
    ui_->keepFileClosedCheckBox->setChecked( defaults.keepFileClosed() );
    ui_->compressedIndexCheckBox->setChecked( defaults.useCompressedIndex() );
    ui_->optimizeForNotLatinEncodingsCheckBox->setChecked( defaults.optimizeForNotLatinEncodings() );
    setupSearchResultsCache();

    ui_->loggingCheckBox->setChecked( defaults.enableLogging() );
    ui_->verbositySpinBox->setValue( defaults.loggingLevel() );
    setupLogging();
}

void OptionsDialog::checkShortcutsOnDuplicate() const
{
    static constexpr int PRIMARY_COL = 1;
    static constexpr int SECONDARY_COL = 2;

    if ( !ui_->shortcutsTable->rowCount() ) {
        return;
    }

    const auto DEFAULT_BACKGROUND = ui_->shortcutsTable->item( 0, PRIMARY_COL )->background();

    for ( auto shortcutRow = 0; shortcutRow < ui_->shortcutsTable->rowCount(); ++shortcutRow ) {
        ui_->shortcutsTable->item( shortcutRow, PRIMARY_COL )->setBackground( DEFAULT_BACKGROUND );
        ui_->shortcutsTable->item( shortcutRow, SECONDARY_COL )->setBackground( DEFAULT_BACKGROUND );
    }

    std::unordered_map<std::string, std::pair<int, int>> uniqueShortcuts;
    bool hasDuplicateShortcuts = false;
    for ( auto shortcutRow = 0; shortcutRow < ui_->shortcutsTable->rowCount(); ++shortcutRow ) {

        auto hasDuplicates = [ &uniqueShortcuts, shortcutRow, this ]( int ncol ) {
            auto keySequence = static_cast<KeySequencePresenter*>(
                                   ui_->shortcutsTable->cellWidget( shortcutRow, ncol ) )
                                   ->keySequence();

            if ( !keySequence.isEmpty() ) {
                if ( auto it = uniqueShortcuts.find( keySequence.toStdString() );
                     it != uniqueShortcuts.end() ) {

                    ui_->shortcutsTable->item( it->second.first, it->second.second )
                        ->setBackground( Qt::red );
                    ui_->shortcutsTable->item( shortcutRow, ncol )->setBackground( Qt::red );

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

    ui_->buttonBox->button( QDialogButtonBox::Ok )->setEnabled( !hasDuplicateShortcuts );
    ui_->buttonBox->button( QDialogButtonBox::Apply )->setEnabled( !hasDuplicateShortcuts );
}

int OptionsDialog::updateTranslate()
{
    return MainWindow::installLanguage( ui_->languageComboBox->currentData().toString() );
}

void OptionsDialog::updateConfigFromDialog()
{
    bool restartAppMessage = false;
    auto& config = Configuration::get();

    bool fontSizeOk = false;
    auto fontPointSize = ui_->fontSizeBox->currentText().toInt( &fontSizeOk );
    if ( !fontSizeOk || fontPointSize <= 0 ) {
        fontPointSize = Configuration{}.mainFont().pointSize();
    }
    QFont font = QFont( ui_->fontFamilyBox->currentText(), fontPointSize );
    config.setMainFont( font );
    config.setLineSpacingPercent( ui_->lineSpacingSpinBox->value() );
    config.setForceFontAntialiasing( ui_->fontSmoothCheckBox->isChecked() );
    config.setUseBoldFont( ui_->boldFontCheckBox->isChecked() );
    config.setUseTextWrap( ui_->wrapTextCheckBox->isChecked() );
    config.setEnableQtHighDpi( ui_->enableQtHiDpiCheckBox->isChecked() );
    config.setScaleFactorRounding( ui_->scaleRoundingComboBox->currentIndex() + 1 );

    config.setMainRegexpType( getRegexpTypeFromIndex( ui_->mainSearchBox->currentIndex() ) );
    config.setMainSearchBackColor( mainSearchColor_ );
    config.setEnableMainSearchHighlight( ui_->highlightMainSearchCheckBox->isChecked() );
    config.setVariateMainSearchHighlight( ui_->variateHighlightCheckBox->isChecked() );
    config.setSearchIgnoreCaseDefault( !ui_->caseSensitiveCheckBox->isChecked() );
    config.setSearchAutoRefreshDefault( ui_->autoRefreshCheckBox->isChecked() );
    config.setSearchLogicalCombiningDefault( ui_->logicalCombiningCheckBox->isChecked() );
    config.setQuickfindRegexpType( getRegexpTypeFromIndex( ui_->quickFindSearchBox->currentIndex() ) );
    config.setQfBackColor( qfSearchColor_ );
    config.setQuickfindIncremental( ui_->incrementalCheckBox->isChecked() );
    config.setRegexpEnging( getRegexpEngineFromIndex( ui_->regexpEngineComboBox->currentIndex() ) );
    config.setAutoRunSearchOnPatternChange( ui_->autoRunSearchOnAddCheckBox->isChecked() );
    config.setShowAllInFilteredViewWhenSearchEmpty( ui_->showAllFilteredWhenEmptyCheckBox->isChecked() );

    config.setNativeFileWatchEnabled( ui_->nativeFileWatchCheckBox->isChecked() );
    config.setPollingEnabled( ui_->pollingCheckBox->isChecked() );
    auto pollInterval = ui_->pollIntervalLineEdit->text().toInt();
    if ( pollInterval < PollIntervalMin )
        pollInterval = PollIntervalMin;
    else if ( pollInterval > PollIntervalMax )
        pollInterval = PollIntervalMax;

    config.setPollIntervalMs( pollInterval );
    config.setFastModificationDetection( ui_->fastModificationDetectionCheckBox->isChecked() );
    config.setAllowFollowOnScroll( ui_->allowFollowOnScrollCheckBox->isChecked() );

    config.setLoadLastSession( ui_->loadLastSessionCheckBox->isChecked() );
    config.setFollowFileOnLoad( ui_->followFileOnLoadCheckBox->isChecked() );
    config.setAllowMultipleWindows( ui_->multipleWindowsCheckBox->isChecked() );
    config.setMinimizeToTray( ui_->minimizeToTrayCheckBox->isChecked() );
    config.setConfirmTabClose( ui_->confirmTabCloseCheckBox->isChecked() );
    config.setLocalControlEnabled( ui_->localControlEnabledCheckBox->isChecked() );
    config.setEnableLogging( ui_->loggingCheckBox->isChecked() );
    config.setLoggingLevel( ui_->verbositySpinBox->value() );

    // Apply logging settings immediately
    logging::enableFileLogging( config.enableLogging(),
                                static_cast<logging::LogLevel>( config.loggingLevel() ) );

    // Update log file path display
    setupLogging();

    config.setExtractArchives( ui_->extractArchivesCheckBox->isChecked() );
    config.setExtractArchivesAlways( ui_->extractArchivesAlwaysCheckBox->isChecked() );

    config.setUseParallelSearch( ui_->parallelSearchCheckBox->isChecked() );
    config.setUseSearchResultsCache( ui_->searchResultsCacheCheckBox->isChecked() );
    config.setSearchResultsCacheLines( static_cast<unsigned>( ui_->searchCacheSpinBox->value() ) );
    config.setIndexReadBufferSizeMb( ui_->indexReadBufferSpinBox->value() );
    config.setSearchReadBufferSizeLines( ui_->searchReadBufferSpinBox->value() );
    config.setKeepFileClosed( ui_->keepFileClosedCheckBox->isChecked() );
    config.setUseCompressedIndex( ui_->compressedIndexCheckBox->isChecked() );
    config.setOptimizeForNotLatinEncodings( ui_->optimizeForNotLatinEncodingsCheckBox->isChecked() );

    // version checking
    config.setVersionCheckingEnabled( ui_->checkForNewVersionCheckBox->isChecked() );

    config.setVerifySslPeers( ui_->verifySslCheckBox->isChecked() );
    if ( adbAnsiOutputCheckBox_ ) {
        config.setAdbLogcatAnsiOutputEnabled( adbAnsiOutputCheckBox_->isChecked() );
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

    const auto selectedStyle = ui_->styleComboBox->currentData().toString();
    restartAppMessage = config.style() != selectedStyle;

    config.setStyle( selectedStyle );

    // Theme mode: while Classic Dark is active the selector is pinned to Dark,
    // so persist the mode the user selected before the pin (themeModeRestore_)
    // rather than the disabled selector's value. Otherwise an Apply/OK while
    // Classic Dark is selected would silently rewrite Auto/Light to Dark and a
    // later switch away from Classic Dark could never restore the earlier mode.
    const auto selectedThemeMode
        = static_cast<ThemeMode>( ui_->themeModeComboBox->currentData().toInt() );
    const auto themeMode
        = themeModePinnedToDark_ && themeModeRestore_ ? *themeModeRestore_ : selectedThemeMode;
    const bool themeModeChanged = config.themeMode() != themeMode;
    config.setThemeMode( themeMode );

    // Apply theme immediately if changed
    if ( themeModeChanged ) {
        StyleManager::applyStyle( config.style() );
    }

    config.setHideAnsiColorSequences( ui_->hideAnsiColorsCheckBox->isChecked() );
    config.setRenderAnsiColorSequences( !ui_->hideAnsiColorsCheckBox->isChecked()
                                        && ui_->renderAnsiColorsCheckBox->isChecked() );

    config.setDefaultEncodingMib( ui_->encodingComboBox->currentData().toInt() );

    auto shortcuts = config.shortcuts();
    for ( auto shortcutRow = 0; shortcutRow < ui_->shortcutsTable->rowCount(); ++shortcutRow ) {
        QStringList actionKeys;

        auto primaryKeySequence
            = qobject_cast<KeySequencePresenter*>( ui_->shortcutsTable->cellWidget( shortcutRow, 1 ) )
                  ->keySequence();
        auto secondaryKeySequence
            = qobject_cast<KeySequencePresenter*>( ui_->shortcutsTable->cellWidget( shortcutRow, 2 ) )
                  ->keySequence();
        actionKeys << primaryKeySequence << secondaryKeySequence;

        auto action
            = ui_->shortcutsTable->item( shortcutRow, 0 )->data( Qt::UserRole ).toString().toStdString();
        shortcuts[ action ] = actionKeys;
    }
    config.setShortcuts( shortcuts );

    // update translate when accept or apply clicked
    // An empty selector (the i18n resource is unavailable, e.g. in the unit
    // tests) has no selection to compare against; a missing selector must not
    // demand a restart, which would otherwise block Apply on a modal warning.
    if ( !ui_->languageComboBox->currentData().isNull() ) {
        restartAppMessage |= config.language() != ui_->languageComboBox->currentData().toString();
    }
    updateTranslate();
    config.setLanguage( ui_->languageComboBox->currentData().toString() );
    ui_->retranslateUi( this );

    config.save();

    auto& savedSearches = SavedSearches::get();
    savedSearches.setHistorySize( ui_->searchHistorySpinBox->value() );
    savedSearches.save();

    auto& recentFiles = RecentFiles::get();
    recentFiles.setFilesHistoryMaxItems( ui_->filesHistoryMaxItemsSpinBox->value() );
    recentFiles.save();

    // Same non-Configuration-persistable pattern as SavedSearches/RecentFiles
    // above: HighlighterSetCollection owns the color-label match defaults.
    auto& highlighterSetCollection = HighlighterSetCollection::get();
    highlighterSetCollection.setQuickHighlighterDefaults(
        { ui_->colorLabelsIgnoreCaseCheckBox->isChecked(), ui_->colorLabelsWholeWordCheckBox->isChecked() } );
    highlighterSetCollection.save();

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
    QDialogButtonBox::ButtonRole role = ui_->buttonBox->buttonRole( button );
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
    ui_->shortcutsTable->setRowCount( 0 );

    const auto& config = Configuration::get();
    auto shortcutList = ShortcutAction::defaultShortcutList();
    if ( !useDefaultsOnly ) {
        for ( const auto& [ action, keys ] : config.shortcuts() ) {
            shortcutList[ action ].keySequence = keys;
        }
    }

    for ( const auto& [ action, shortCut ] : shortcutList ) {
        auto currentRow = ui_->shortcutsTable->rowCount();
        ui_->shortcutsTable->insertRow( currentRow );

        auto keyItem = new QTableWidgetItem( shortCut.name );
        keyItem->setFlags( Qt::ItemIsEnabled | Qt::ItemIsSelectable );
        keyItem->setData( Qt::UserRole, QString::fromStdString( action ) );
        ui_->shortcutsTable->setItem( currentRow, 0, keyItem );

        auto primaryKeySequence = new KeySequencePresenter(
            shortCut.keySequence.size() > 0 ? shortCut.keySequence[ 0 ] : "" );
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): setItem transfers ownership to the table.
        ui_->shortcutsTable->setItem( currentRow, 1, new QTableWidgetItem );
        ui_->shortcutsTable->setCellWidget( currentRow, 1, primaryKeySequence );
        connect( primaryKeySequence, &KeySequencePresenter::edited, this,
                 &OptionsDialog::checkShortcutsOnDuplicate );

        auto secondaryKeySequence = new KeySequencePresenter(
            shortCut.keySequence.size() > 1 ? shortCut.keySequence[ 1 ] : "" );
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): setItem transfers ownership to the table.
        ui_->shortcutsTable->setItem( currentRow, 2, new QTableWidgetItem );
        ui_->shortcutsTable->setCellWidget( currentRow, 2, secondaryKeySequence );
        connect( secondaryKeySequence, &KeySequencePresenter::edited, this,
                 &OptionsDialog::checkShortcutsOnDuplicate );
    }

    ui_->shortcutsTable->horizontalHeader()->setSectionResizeMode( QHeaderView::Stretch );
    ui_->shortcutsTable->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::Interactive );
    ui_->shortcutsTable->horizontalHeader()->setMinimumSectionSize( 150 );
    ui_->shortcutsTable->resizeColumnToContents( 0 );
    ui_->shortcutsTable->setHorizontalHeaderLabels(
        { tr( "Action" ), tr( "Primary shortcut" ), tr( "Secondary shortcut" ) } );

    // in case if user set duplicate keys and after restores defaults
    // it is need to enable back standard buttons
    checkShortcutsOnDuplicate();

    ui_->shortcutsTable->sortItems( 0 );
}
