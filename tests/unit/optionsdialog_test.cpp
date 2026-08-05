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

#include <QApplication>
#include <QComboBox>
#include <QLabel>

#include "optionsdialog.h"
#include "recentfiles.h"
#include "savedsearches.h"
#include "styles.h"

TEST_CASE( "Theme mode selector is disabled for the fixed Classic Dark style" )
{
    // OptionsDialog::updateDialogFromConfig() reads the SavedSearches and
    // RecentFiles persistables during construction; initialize them like the
    // other UI tests do so get() does not throw.
    SavedSearches::getSynced();
    RecentFiles::getSynced();

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

    // Open the dialog as if the user had saved Classic Dark + Auto: the theme
    // selector must present itself pinned to Dark, not as the saved Auto mode.
    // Preserve the process-wide configuration so the test does not leak state.
    // Use get() for the writes: getSynced() reloads from disk on every call, so
    // a second getSynced() would discard the first setStyle().
    const auto previousStyle = Configuration::getSynced().style();
    const auto previousThemeMode = Configuration::getSynced().themeMode();
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

    Configuration::get().setStyle( previousStyle );
    Configuration::get().setThemeMode( previousThemeMode );
}
