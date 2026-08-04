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
#include <QPalette>

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

// Applies a fixed style + theme mode for the duration of the test and
// restores both afterwards. Mirrors ScopedModernStyle so style tests never
// leak state into each other.
class ScopedStyleSetting {
  public:
    ScopedStyleSetting( const QString& style, ThemeMode mode )
        : previousStyle_( Configuration::getSynced().style() )
        , previousThemeMode_( Configuration::getSynced().themeMode() )
    {
        if ( previousStyle_.isEmpty() ) {
            previousStyle_ = StyleManager::defaultStyle();
        }
        Configuration::getSynced().setStyle( style );
        Configuration::getSynced().setThemeMode( mode );
        StyleManager::applyStyle( style );
    }

    ~ScopedStyleSetting()
    {
        Configuration::getSynced().setStyle( previousStyle_ );
        Configuration::getSynced().setThemeMode( previousThemeMode_ );
        StyleManager::applyStyle( previousStyle_ );
    }

  private:
    QString previousStyle_;
    ThemeMode previousThemeMode_;
};
} // namespace

TEST_CASE( "Classic Dark is not overridden by a light theme mode" )
{
    // Regression for https://github.com/ZEACENT/klogg/issues/51 ("Classic
    // dark won't stay enabled"). The theme mode (Light/Dark/Auto) governs how
    // "Modern" and "System" adapt to the platform; it must not silently
    // replace an explicit "Classic Dark" selection with the light platform
    // default. Before the fix, ThemeMode::Light (and ThemeMode::Auto on a
    // light system) rendered Classic Dark light.
    const ScopedStyleSetting styleGuard{ StyleManager::DarkStyleKey, ThemeMode::Light };

    const auto windowColor = qApp->palette().color( QPalette::Window );
    const auto textColor = qApp->palette().color( QPalette::WindowText );
    REQUIRE( windowColor.lightness() < 100 );
    REQUIRE( textColor.lightness() > 150 );
}

TEST_CASE( "Classic Dark is not overridden by auto theme mode" )
{
    // Auto theme mode must not turn an explicit Classic Dark style light when
    // the system theme is light (the default configuration is Auto).
    const ScopedStyleSetting styleGuard{ StyleManager::DarkStyleKey, ThemeMode::Auto };

    const auto windowColor = qApp->palette().color( QPalette::Window );
    REQUIRE( windowColor.lightness() < 100 );
}

TEST_CASE( "System style still follows a light theme mode" )
{
    // Counter-check: the theme-mode feature itself is preserved. With an
    // explicit dark style, theme mode is bypassed (tests above); with the
    // System style it still resolves through the theme mode.
    const ScopedStyleSetting styleGuard{ StyleManager::SystemKey, ThemeMode::Light };

    const auto windowColor = qApp->palette().color( QPalette::Window );
    REQUIRE( windowColor.lightness() >= 100 );
}

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

TEST_CASE( "Modern style sheet uses compact tab height for macOS 26 pill-tab look" )
{
    const ScopedModernStyle styleGuard;

    const auto style = qApp->styleSheet();
    // Tabs should be shorter — macOS 26 / iTerm2 style is compact
    REQUIRE( style.contains( QStringLiteral( "min-height: 20px" ) ) );
    // Tab bar padding should be tighter (QTabBar container padding)
    REQUIRE( style.contains( QStringLiteral( "padding: 2px" ) ) );
    // Tab padding should be compact (horizontal + vertical)
    REQUIRE( style.contains( QStringLiteral( "padding: 3px 12px" ) ) );
}

TEST_CASE( "Modern style sheet suppresses native focus rectangle on tabs" )
{
    const ScopedModernStyle styleGuard;

    const auto style = qApp->styleSheet();
    // Tab focus must use outline:none to prevent Windows native focus rect
    // from drawing a sharp rectangle over the pill-shaped tab
    REQUIRE( style.contains( QStringLiteral( "QTabBar::tab:focus" ) ) );
    REQUIRE( style.contains( QStringLiteral( "outline: none" ) ) );
}
