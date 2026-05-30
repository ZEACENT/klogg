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

#include "configuration.h"
#include "styles.h"

namespace {
class ScopedModernStyle {
  public:
    ScopedModernStyle()
        : previousStyle_( Configuration::getSynced().style() )
        , previousThemeMode_( Configuration::getSynced().themeMode() )
    {
        if ( previousStyle_.isEmpty() ) {
            previousStyle_ = StyleManager::defaultStyle();
        }
        Configuration::getSynced().setThemeMode( ThemeMode::Dark );
        StyleManager::applyStyle( StyleManager::ModernKey );
    }

    ~ScopedModernStyle()
    {
        Configuration::getSynced().setThemeMode( previousThemeMode_ );
        Configuration::getSynced().setStyle( previousStyle_ );
        StyleManager::applyStyle( previousStyle_ );
    }

  private:
    QString previousStyle_;
    ThemeMode previousThemeMode_;
};
} // namespace

TEST_CASE( "Modern style sheet gives tabs a rounded iTerm-style treatment" )
{
    const ScopedModernStyle styleGuard;

    const auto style = qApp->styleSheet();
    REQUIRE( style.contains( QStringLiteral( "QTabBar {" ) ) );
    REQUIRE( style.contains( QStringLiteral( "QTabBar::tab:selected" ) ) );
    REQUIRE( style.contains( QStringLiteral( "border-radius: 14px" ) ) );
    REQUIRE( style.contains( QStringLiteral( "border-radius: 12px" ) ) );
    REQUIRE( style.contains( QStringLiteral( "font-weight: 600" ) ) );
    REQUIRE_FALSE( style.contains( QStringLiteral( "border-bottom: none" ) ) );
}
