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
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QSpinBox>
#include <QMessageBox>

#include "uimessage.h"

#include "highlighterset.h"
#include "optionsdialog.h"
#include "recentfiles.h"
#include "savedsearches.h"
#include "styles.h"

namespace {
// Restore the process-wide Configuration when the test ends. Catch2 aborts a
// test by throwing, so a manual restore at the end of the case would be skipped
// on a REQUIRE failure and leak the modified global state into later tests.
class ConfigurationScope {
  public:
    ConfigurationScope()
        : style_( Configuration::getSynced().style() )
        , themeMode_( Configuration::getSynced().themeMode() )
        , language_( Configuration::getSynced().language() )
        , rollingMaxFileSize_( Configuration::getSynced().liveCaptureRollingMaxFileSize() )
        , localControlEnabled_( Configuration::get().localControlEnabled() )
    {
    }
    ~ConfigurationScope()
    {
        Configuration::get().setStyle( style_ );
        Configuration::get().setThemeMode( themeMode_ );
        // updateConfigFromDialog() writes the language from the combo; restore
        // it too so later tests see the same process-wide configuration.
        Configuration::get().setLanguage( language_ );
        Configuration::get().setLiveCaptureRollingMaxFileSize( rollingMaxFileSize_ );
        Configuration::get().setLocalControlEnabled( localControlEnabled_ );
        // updateConfigFromDialog() also calls save(), persisting the test's
        // values to the QSettings store. Persist the restored snapshot too, or
        // a later getSynced() reloads the leaked test configuration and the
        // suite becomes order-dependent.
        Configuration::get().save();
    }

    ConfigurationScope( const ConfigurationScope& ) = delete;
    ConfigurationScope& operator=( const ConfigurationScope& ) = delete;

  private:
    QString style_;
    ThemeMode themeMode_;
    QString language_;
    qint64 rollingMaxFileSize_;
    bool localControlEnabled_;
};

// Restore the process-wide HighlighterSetCollection quick-defaults when the
// test ends, for the same reason ConfigurationScope exists: a REQUIRE abort
// unwinds the stack, and updateConfigFromDialog() also persists the
// collection, so the on-disk snapshot must be restored too.
class QuickHighlighterDefaultsScope {
  public:
    QuickHighlighterDefaultsScope()
        : defaults_( HighlighterSetCollection::get().quickHighlighterDefaults() )
    {
    }
    ~QuickHighlighterDefaultsScope()
    {
        HighlighterSetCollection::get().setQuickHighlighterDefaults( defaults_ );
        HighlighterSetCollection::get().save();
    }

    QuickHighlighterDefaultsScope( const QuickHighlighterDefaultsScope& ) = delete;
    QuickHighlighterDefaultsScope& operator=( const QuickHighlighterDefaultsScope& ) = delete;

  private:
    QuickHighlighterDefaults defaults_;
};
} // namespace

TEST_CASE( "Theme mode selector is disabled for the fixed Classic Dark style" )
{
    // OptionsDialog::updateDialogFromConfig() reads the SavedSearches and
    // RecentFiles persistables during construction; initialize them like the
    // other UI tests do so get() does not throw.
    SavedSearches::getSynced();
    RecentFiles::getSynced();
    // The dialog constructor also mirrors the color-label match defaults from
    // HighlighterSetCollection; initialize it for the same reason.
    HighlighterSetCollection::getSynced();

    // The theme mode (Light/Dark/Auto) governs how "Modern" and "System"
    // adapt to the platform. "Classic Dark" is an inherently dark style, so
    // the theme selector would have no effect on it; the dialog disables it
    // and shows a hint instead of presenting a dead control.
    OptionsDialog dialog;
    dialog.show();

    auto* styleCombo
        = dialog.findChild<QComboBox*>( QStringLiteral( "styleComboBox" ) );
    auto* themeCombo
        = dialog.findChild<QComboBox*>( QStringLiteral( "themeModeComboBox" ) );
    auto* themeHint
        = dialog.findChild<QLabel*>( QStringLiteral( "themeModeHintLabel" ) );
    REQUIRE( styleCombo != nullptr );
    REQUIRE( themeCombo != nullptr );
    REQUIRE( themeHint != nullptr );

    const int modernIndex = styleCombo->findData( StyleManager::ModernKey );
    const int systemIndex = styleCombo->findData( StyleManager::SystemKey );
    const int darkIndex = styleCombo->findData( StyleManager::DarkStyleKey );
    REQUIRE( modernIndex != -1 );
    REQUIRE( systemIndex != -1 );
    REQUIRE( darkIndex != -1 );

    // The Style group sits on the hidden "View" tab, so isVisible() is false
    // regardless; isHidden() reflects the widget's own flag (setVisible)
    // independently of ancestor visibility.
    styleCombo->setCurrentIndex( modernIndex );
    REQUIRE( themeCombo->isEnabled() );
    REQUIRE( themeHint->isHidden() );

    styleCombo->setCurrentIndex( systemIndex );
    REQUIRE( themeCombo->isEnabled() );
    REQUIRE( themeHint->isHidden() );

    styleCombo->setCurrentIndex( darkIndex );
    REQUIRE_FALSE( themeCombo->isEnabled() );
    REQUIRE_FALSE( themeHint->isHidden() );

    styleCombo->setCurrentIndex( modernIndex );
    REQUIRE( themeCombo->isEnabled() );
    REQUIRE( themeHint->isHidden() );
}

TEST_CASE( "Classic Dark pins the theme mode selector to Dark and restores it on leave" )
{
    // OptionsDialog::updateDialogFromConfig() reads the SavedSearches and
    // RecentFiles persistables during construction; initialize them like the
    // other UI tests do so get() does not throw.
    SavedSearches::getSynced();
    RecentFiles::getSynced();
    // The dialog constructor also mirrors the color-label match defaults from
    // HighlighterSetCollection; initialize it for the same reason.
    HighlighterSetCollection::getSynced();

    OptionsDialog dialog;
    dialog.show();

    auto* styleCombo
        = dialog.findChild<QComboBox*>( QStringLiteral( "styleComboBox" ) );
    auto* themeCombo
        = dialog.findChild<QComboBox*>( QStringLiteral( "themeModeComboBox" ) );
    REQUIRE( styleCombo != nullptr );
    REQUIRE( themeCombo != nullptr );

    const int modernIndex = styleCombo->findData( StyleManager::ModernKey );
    const int systemIndex = styleCombo->findData( StyleManager::SystemKey );
    const int darkStyleIndex = styleCombo->findData( StyleManager::DarkStyleKey );
    const int autoModeIndex = themeCombo->findData( static_cast<int>( ThemeMode::Auto ) );
    REQUIRE( modernIndex != -1 );
    REQUIRE( systemIndex != -1 );
    REQUIRE( darkStyleIndex != -1 );
    REQUIRE( autoModeIndex != -1 );

    // Start from a known state: Modern + Auto.
    styleCombo->setCurrentIndex( modernIndex );
    themeCombo->setCurrentIndex( autoModeIndex );
    REQUIRE( themeCombo->isEnabled() );

    // Selecting Classic Dark must switch the theme selector to Dark (not leave
    // it showing the previous, now meaningless mode) and disable it.
    styleCombo->setCurrentIndex( darkStyleIndex );
    REQUIRE_FALSE( themeCombo->isEnabled() );
    REQUIRE( themeCombo->currentData() == static_cast<int>( ThemeMode::Dark ) );

    // Leaving Classic Dark restores the previously selected mode (Auto).
    styleCombo->setCurrentIndex( modernIndex );
    REQUIRE( themeCombo->isEnabled() );
    REQUIRE( themeCombo->currentData() == static_cast<int>( ThemeMode::Auto ) );

    // "System" behaves like "Modern": Classic Dark pins Dark, leaving restores.
    styleCombo->setCurrentIndex( systemIndex );
    themeCombo->setCurrentIndex( autoModeIndex );
    styleCombo->setCurrentIndex( darkStyleIndex );
    REQUIRE_FALSE( themeCombo->isEnabled() );
    REQUIRE( themeCombo->currentData() == static_cast<int>( ThemeMode::Dark ) );
    styleCombo->setCurrentIndex( systemIndex );
    REQUIRE( themeCombo->isEnabled() );
    REQUIRE( themeCombo->currentData() == static_cast<int>( ThemeMode::Auto ) );
}

TEST_CASE( "Leaving Classic Dark restores a non-default theme mode selected before it" )
{
    SavedSearches::getSynced();
    RecentFiles::getSynced();
    // The dialog constructor also mirrors the color-label match defaults from
    // HighlighterSetCollection; initialize it for the same reason.
    HighlighterSetCollection::getSynced();

    OptionsDialog dialog;
    dialog.show();

    auto* styleCombo
        = dialog.findChild<QComboBox*>( QStringLiteral( "styleComboBox" ) );
    auto* themeCombo
        = dialog.findChild<QComboBox*>( QStringLiteral( "themeModeComboBox" ) );
    REQUIRE( styleCombo != nullptr );
    REQUIRE( themeCombo != nullptr );

    const int modernIndex = styleCombo->findData( StyleManager::ModernKey );
    const int darkStyleIndex = styleCombo->findData( StyleManager::DarkStyleKey );
    const int lightModeIndex = themeCombo->findData( static_cast<int>( ThemeMode::Light ) );
    REQUIRE( modernIndex != -1 );
    REQUIRE( darkStyleIndex != -1 );
    REQUIRE( lightModeIndex != -1 );

    // The user explicitly picks Light, then Classic Dark, then back to Modern:
    // the Light choice must be restored, not silently dropped.
    styleCombo->setCurrentIndex( modernIndex );
    themeCombo->setCurrentIndex( lightModeIndex );
    REQUIRE( themeCombo->currentData() == static_cast<int>( ThemeMode::Light ) );

    styleCombo->setCurrentIndex( darkStyleIndex );
    REQUIRE( themeCombo->currentData() == static_cast<int>( ThemeMode::Dark ) );

    styleCombo->setCurrentIndex( modernIndex );
    REQUIRE( themeCombo->isEnabled() );
    REQUIRE( themeCombo->currentData() == static_cast<int>( ThemeMode::Light ) );
}

TEST_CASE( "Dialog initialized with Classic Dark shows the theme mode pinned to Dark" )
{
    // OptionsDialog::updateDialogFromConfig() reads the SavedSearches and
    // RecentFiles persistables during construction; initialize them like the
    // other UI tests do so get() does not throw.
    SavedSearches::getSynced();
    RecentFiles::getSynced();
    // The dialog constructor also mirrors the color-label match defaults from
    // HighlighterSetCollection; initialize it for the same reason.
    HighlighterSetCollection::getSynced();

    // Open the dialog as if the user had saved Classic Dark + Auto: the theme
    // selector must present itself pinned to Dark, not as the saved Auto mode.
    // The scope guard restores the process-wide configuration on stack
    // unwinding (including a REQUIRE abort); get() is used for the writes
    // because getSynced() reloads from disk on every call and a second
    // getSynced() would discard the first setStyle().
    ConfigurationScope configScope;
    Configuration::get().setStyle( StyleManager::DarkStyleKey );
    Configuration::get().setThemeMode( ThemeMode::Auto );

    {
        OptionsDialog dialog;
        dialog.show();

        auto* styleCombo
            = dialog.findChild<QComboBox*>( QStringLiteral( "styleComboBox" ) );
        auto* themeCombo
            = dialog.findChild<QComboBox*>( QStringLiteral( "themeModeComboBox" ) );
        REQUIRE( styleCombo != nullptr );
        REQUIRE( themeCombo != nullptr );

        const int modernIndex = styleCombo->findData( StyleManager::ModernKey );
        const int darkStyleIndex = styleCombo->findData( StyleManager::DarkStyleKey );
        const int autoModeIndex = themeCombo->findData( static_cast<int>( ThemeMode::Auto ) );
        REQUIRE( modernIndex != -1 );
        REQUIRE( darkStyleIndex != -1 );
        REQUIRE( autoModeIndex != -1 );

        // Opening the dialog on a saved Classic Dark style must present the
        // theme selector pinned to Dark (disabled), not the saved Auto mode.
        REQUIRE( styleCombo->currentData() == StyleManager::DarkStyleKey );
        REQUIRE_FALSE( themeCombo->isEnabled() );
        REQUIRE( themeCombo->currentData() == static_cast<int>( ThemeMode::Dark ) );

        // Moving away restores the theme mode that was configured (Auto).
        styleCombo->setCurrentIndex( modernIndex );
        REQUIRE( themeCombo->isEnabled() );
        REQUIRE( themeCombo->currentData() == static_cast<int>( ThemeMode::Auto ) );
    }
    // configScope destructor restores the previous style/theme-mode.
}

TEST_CASE( "Applying the dialog while Classic Dark is pinned preserves the pre-pin theme mode" )
{
    SavedSearches::getSynced();
    RecentFiles::getSynced();
    // The dialog constructor also mirrors the color-label match defaults from
    // HighlighterSetCollection; initialize it for the same reason.
    HighlighterSetCollection::getSynced();
    ConfigurationScope configScope;

    // Deterministic start: saved as Classic Dark + Auto, so the dialog opens
    // with the selector pinned to Dark and Auto remembered for later restore.
    // Applying without leaving Classic Dark must persist Auto, not the pinned
    // Dark. (A style switch would also work but sets restartAppMessage, whose
    // modal QMessageBox would block the offscreen test.)
    Configuration::get().setStyle( StyleManager::DarkStyleKey );
    Configuration::get().setThemeMode( ThemeMode::Auto );

    OptionsDialog dialog;
    dialog.show();

    auto* styleCombo
        = dialog.findChild<QComboBox*>( QStringLiteral( "styleComboBox" ) );
    auto* themeCombo
        = dialog.findChild<QComboBox*>( QStringLiteral( "themeModeComboBox" ) );
    auto* buttonBox
        = dialog.findChild<QDialogButtonBox*>( QStringLiteral( "buttonBox" ) );
    REQUIRE( styleCombo != nullptr );
    REQUIRE( themeCombo != nullptr );
    REQUIRE( buttonBox != nullptr );

    const int darkStyleIndex = styleCombo->findData( StyleManager::DarkStyleKey );
    const int autoModeIndex = themeCombo->findData( static_cast<int>( ThemeMode::Auto ) );
    REQUIRE( darkStyleIndex != -1 );
    REQUIRE( autoModeIndex != -1 );

    // Dialog initialized with Classic Dark + Auto: selector pinned to Dark.
    REQUIRE( styleCombo->currentData() == StyleManager::DarkStyleKey );
    REQUIRE_FALSE( themeCombo->isEnabled() );
    REQUIRE( themeCombo->currentData() == static_cast<int>( ThemeMode::Dark ) );

    auto* applyButton = buttonBox->button( QDialogButtonBox::Apply );
    REQUIRE( applyButton != nullptr );

    // Invoke the same slot the Apply button drives. Clicking the button
    // (QAbstractButton::click()) synthesizes a window-system mouse event that
    // blocks on the offscreen platform in CI (klogg_tests timed out after the
    // dialog construction); QMetaObject::invokeMethod is the established
    // pattern for updateConfigFromDialog (see adb_ui_transport_test.cpp).
    REQUIRE( QMetaObject::invokeMethod( &dialog, "updateConfigFromDialog",
                                        Qt::DirectConnection ) );

    // The persisted theme mode must be the pre-pin Auto, not the pinned Dark,
    // so switching away from Classic Dark later can restore the original mode.
    REQUIRE( Configuration::get().style() == StyleManager::DarkStyleKey );
    REQUIRE( Configuration::get().themeMode() == ThemeMode::Auto );
}

TEST_CASE( "Options dialog edits the single local-control switch on Apply", "[control-settings]" )
{
    SavedSearches::getSynced();
    RecentFiles::getSynced();
    HighlighterSetCollection::getSynced();
    ConfigurationScope scope;
    auto& config = Configuration::get();
    config.setStyle( StyleManager::DarkStyleKey );
    config.setLocalControlEnabled( true );
    OptionsDialog dialog;
    auto* enabled = dialog.findChild<QCheckBox*>( QStringLiteral( "localControlEnabledCheckBox" ) );
    REQUIRE( enabled != nullptr );
    CHECK( enabled->isChecked() );
    enabled->setChecked( false );
    CHECK( config.localControlEnabled() );
    REQUIRE( QMetaObject::invokeMethod( &dialog, "updateConfigFromDialog", Qt::DirectConnection ) );
    CHECK_FALSE( config.localControlEnabled() );
    CHECK_FALSE( Configuration::getSynced().localControlEnabled() );
    enabled->setChecked( true );
    REQUIRE( QMetaObject::invokeMethod( &dialog, "updateConfigFromDialog", Qt::DirectConnection ) );
    CHECK( Configuration::getSynced().localControlEnabled() );
}

TEST_CASE( "Control CLI installation is explicit, previewed, and independent of control policy",
           "[control-install-ui]" )
{
    using namespace klogg::control;
    SavedSearches::getSynced();
    RecentFiles::getSynced();
    HighlighterSetCollection::getSynced();
    ConfigurationScope scope;
    const auto enabled = Configuration::get().localControlEnabled();
    std::vector<InstallAction> actions;
    InstallResult preview;
    preview.status = InstallStatus::NotInstalled;
    preview.message = QStringLiteral( "Ready to install" );
    preview.plannedPaths = QStringList{ QStringLiteral( "/fixture/<home>/.bashrc" ),
                                       QStringLiteral( "/fixture/bin/kloggctl" ) };
    OptionsDialog dialog( nullptr, [&]( InstallAction action, const QStringList& approvedPaths ) {
        actions.push_back( action );
        if ( action == InstallAction::Install || action == InstallAction::Remove )
            CHECK( approvedPaths == preview.plannedPaths );
        else
            CHECK( approvedPaths.isEmpty() );
        if ( action == InstallAction::Check || action == InstallAction::CheckRemoval ) {
            return preview;
        }
        InstallResult result;
        result.status = InstallStatus::PartialFailure;
        result.message = QStringLiteral( "Concurrent edit; inspect the backup" );
        result.changedPaths = QStringList{ QStringLiteral( "/fixture/bin/kloggctl" ) };
        result.backupPaths = QStringList{ QStringLiteral( "/fixture/backup" ) };
        return result;
    } );
    CHECK( actions.empty() );
    auto* check = dialog.findChild<QPushButton*>( QStringLiteral( "controlCliCheckButton" ) );
    auto* install = dialog.findChild<QPushButton*>( QStringLiteral( "controlCliInstallButton" ) );
    auto* remove = dialog.findChild<QPushButton*>( QStringLiteral( "controlCliRemoveButton" ) );
    auto* status = dialog.findChild<QLabel*>( QStringLiteral( "controlCliStatusLabel" ) );
    REQUIRE( check );
    REQUIRE( install );
    REQUIRE( remove );
    REQUIRE( status );
    check->click();
    REQUIRE( actions == std::vector<InstallAction>{ InstallAction::Check } );
    CHECK( status->textFormat() == Qt::PlainText );
    CHECK( status->text().contains( preview.plannedPaths.front() ) );

    int answer = QMessageBox::No; // lint-allow: platform-fragile -- enum result for injected non-modal dialog handler
    int prompts = 0;
    klogg::ui::ScopedDialogHandler handler( [&]( QDialog& confirmation ) {
        auto* box = qobject_cast<QMessageBox*>( &confirmation ); // lint-allow: platform-fragile -- inspect intercepted dialog, never execute it
        REQUIRE( box );
        CHECK( box->textFormat() == Qt::PlainText );
        CHECK( box->defaultButton() == box->button( QMessageBox::No ) ); // lint-allow: platform-fragile -- inspect default button inside injected handler
        for ( const auto& path : preview.plannedPaths ) CHECK( box->text().contains( path ) );
        ++prompts;
        return answer;
    } );
    actions.clear();
    install->click();
    CHECK( actions == std::vector<InstallAction>{ InstallAction::Check } );
    CHECK( prompts == 1 );
    answer = QMessageBox::Yes; // lint-allow: platform-fragile -- injected result, no native modal execution
    actions.clear();
    install->click();
    CHECK( actions == std::vector<InstallAction>{ InstallAction::Check, InstallAction::Install } );
    CHECK( status->text().contains( QStringLiteral( "/fixture/backup" ) ) );
    CHECK( status->text().contains( QStringLiteral( "Concurrent edit" ) ) );
    preview.status = InstallStatus::Installed;
    actions.clear();
    remove->click();
    CHECK( actions == std::vector<InstallAction>{ InstallAction::CheckRemoval, InstallAction::Remove } );
    CHECK( Configuration::get().localControlEnabled() == enabled );

    preview.status = InstallStatus::UnsafePath;
    actions.clear();
    const auto previousPrompts = prompts;
    install->click();
    CHECK( actions == std::vector<InstallAction>{ InstallAction::Check } );
    CHECK( prompts == previousPrompts );
    actions.clear();
    remove->click();
    CHECK( actions == std::vector<InstallAction>{ InstallAction::CheckRemoval } );
    CHECK( prompts == previousPrompts );

    preview.status = InstallStatus::NotInstalled;
    preview.plannedPaths.clear();
    actions.clear();
    install->click();
    CHECK( actions == std::vector<InstallAction>{ InstallAction::Check } );
    CHECK( prompts == previousPrompts );
}

TEST_CASE( "ConfigurationScope restores the persisted configuration on disk" )
{
    // updateConfigFromDialog() calls Configuration::save(), which writes the
    // in-memory state to the QSettings store. ConfigurationScope must restore
    // not just the in-memory singleton but the on-disk snapshot, otherwise a
    // later Configuration::getSynced() (which reloads from disk on every call)
    // observes the leaked test configuration and the suite becomes
    // order-dependent.
    //
    // Pre-seed a distinct persisted state so any baseline leak is observable:
    // this case must hand the store back exactly as it found it, not as an
    // artificial baseline normalized for its own assertions.
    Configuration::get().setStyle( StyleManager::DarkStyleKey );
    Configuration::get().setThemeMode( ThemeMode::Dark );
    Configuration::get().setLanguage( QStringLiteral( "preseed_lang" ) );
    Configuration::get().save();

    {
        // Outer scope snapshots the TRUE pre-test persisted state, so the
        // baseline normalization below is itself rolled back on disk when this
        // case exits. Without it, the baseline write escapes to the shared
        // QSettings store and a later Configuration::getSynced() observes it —
        // the order-dependence this test exists to prevent.
        ConfigurationScope testIsolation;

        SavedSearches::getSynced();
        RecentFiles::getSynced();
        // The dialog constructor also mirrors the color-label match defaults
        // from HighlighterSetCollection; initialize it for the same reason.
        HighlighterSetCollection::getSynced();

        // Normalize to a known baseline first so the assertions below are
        // deterministic regardless of what earlier tests left in the shared
        // store.
        Configuration::get().setStyle( StyleManager::ModernKey );
        Configuration::get().setThemeMode( ThemeMode::Light );
        Configuration::get().setLanguage( QStringLiteral( "baseline_lang" ) );
        Configuration::get().save();

        const auto originalStyle = Configuration::getSynced().style();
        const auto originalThemeMode = Configuration::getSynced().themeMode();
        const auto originalLanguage = Configuration::getSynced().language();

        {
            ConfigurationScope configScope;

            // Simulate the OptionsDialog Apply path: mutate and persist.
            Configuration::get().setStyle( StyleManager::DarkStyleKey );
            Configuration::get().setThemeMode( ThemeMode::Auto );
            Configuration::get().setLanguage( QStringLiteral( "test_language" ) );
            Configuration::get().save();
        }

        // The disk-backed snapshot must be restored, not the leaked values
        // above.
        REQUIRE( Configuration::getSynced().style() == originalStyle );
        REQUIRE( Configuration::getSynced().themeMode() == originalThemeMode );
        REQUIRE( Configuration::getSynced().language() == originalLanguage );
    }

    // The pre-test seed, not the artificial baseline, must be what persists.
    REQUIRE( Configuration::getSynced().style() == StyleManager::DarkStyleKey );
    REQUIRE( Configuration::getSynced().themeMode() == ThemeMode::Dark );
    REQUIRE( Configuration::getSynced().language() == QStringLiteral( "preseed_lang" ) );
}

TEST_CASE( "Color label match option defaults are configurable in the options dialog",
           "[configuration]" )
{
    // updateDialogFromConfig() reads these persistables during construction;
    // initialize them (and the highlighter collection) like the other tests do
    // so get() does not throw.
    SavedSearches::getSynced();
    RecentFiles::getSynced();
    // The dialog constructor also mirrors the color-label match defaults from
    // HighlighterSetCollection; initialize it for the same reason.
    HighlighterSetCollection::getSynced();

    // The Apply path writes and saves both the process-wide Configuration and
    // the HighlighterSetCollection; the scope guards restore in-memory and
    // on-disk state so serial sibling tests are unaffected.
    ConfigurationScope configScope;
    QuickHighlighterDefaultsScope defaultsScope;

    // Seed known values (the product defaults: ignore case on, whole word
    // off). HighlighterSetCollection stays the single source of truth; the
    // dialog only mirrors it.
    HighlighterSetCollection::get().setQuickHighlighterDefaults( { true, false } );

    OptionsDialog dialog;
    dialog.show();

    auto* ignoreCaseCheckBox
        = dialog.findChild<QCheckBox*>( QStringLiteral( "colorLabelsIgnoreCaseCheckBox" ) );
    auto* wholeWordCheckBox
        = dialog.findChild<QCheckBox*>( QStringLiteral( "colorLabelsWholeWordCheckBox" ) );
    REQUIRE( ignoreCaseCheckBox != nullptr );
    REQUIRE( wholeWordCheckBox != nullptr );

    // Initial state mirrors the stored defaults.
    REQUIRE( ignoreCaseCheckBox->isChecked() );
    REQUIRE_FALSE( wholeWordCheckBox->isChecked() );

    // Toggle both, then Apply. Do NOT touch the style/language combos: a
    // style change pops a modal restart prompt that hangs offscreen.
    ignoreCaseCheckBox->setChecked( false );
    wholeWordCheckBox->setChecked( true );

    // Same pattern as the theme-mode Apply test above: invoke the Apply slot
    // directly because QAbstractButton::click() blocks on the offscreen
    // platform.
    REQUIRE( QMetaObject::invokeMethod( &dialog, "updateConfigFromDialog",
                                        Qt::DirectConnection ) );

    const auto defaults = HighlighterSetCollection::get().quickHighlighterDefaults();
    REQUIRE_FALSE( defaults.ignoreCase );
    REQUIRE( defaults.wholeWord );
}

TEST_CASE( "OptionsDialog preserves rounded positive sub-megabyte rolling limits",
           "[configuration]" )
{
    SavedSearches::getSynced();
    RecentFiles::getSynced();
    HighlighterSetCollection::getSynced();

    ConfigurationScope configScope;
    Configuration::get().setLiveCaptureRollingMaxFileSize( 512LL * 1024 );

    OptionsDialog dialog;
    auto* const maxFileSizeSpinBox
        = dialog.findChild<QSpinBox*>( QStringLiteral( "liveSourceRollingMaxFileSizeSpinBox" ) );
    REQUIRE( maxFileSizeSpinBox != nullptr );
    REQUIRE( maxFileSizeSpinBox->value() == 1 );

    REQUIRE( QMetaObject::invokeMethod( &dialog, "updateConfigFromDialog",
                                        Qt::DirectConnection ) );
    REQUIRE( Configuration::get().liveCaptureRollingMaxFileSize()
             == Configuration::LiveCaptureRollingBytesPerMb );
}
