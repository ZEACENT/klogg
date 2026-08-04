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
