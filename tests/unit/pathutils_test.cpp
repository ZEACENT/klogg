/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
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

#include <QString>

#include <utility>

#include "pathutils.h"

TEST_CASE( "Suggested filename stems separate device labels without UI punctuation",
           "[pathutils][save-filename]" )
{
    const auto example = GENERATE(
        std::make_pair( "Pixel 8 (ABC123)", "Pixel-8-ABC123" ),
        std::make_pair( "iPhone 15 Pro (USB)", "iPhone-15-Pro-USB" ),
        std::make_pair( "device [online] {USB}", "device-online-USB" ),
        std::make_pair( "测试设备（USB）［在线］｛记录｝", "测试设备-USB-在线-记录" ),
        std::make_pair( "设备【在线】「日志」《备份》", "设备-在线-日志-备份" ),
        std::make_pair( "  --device ( [USB] )--  ", "device-USB" ),
        std::make_pair( "device<>:\"/\\|?*name", "device-name" ),
        std::make_pair( "../../device/name", "device-name" ),
        std::make_pair( "...device...", "device" ),
        std::make_pair( "device_01.trace", "device_01.trace" ),
        std::make_pair( "中文-é-é-📱-𠮷", "中文-é-é-📱-𠮷" ),
        std::make_pair( "ZEACENT's iPhone", "ZEACENT's-iPhone" ),
        std::make_pair( "device\U000e0001USB", "device-USB" ) );
    const auto label = QString::fromUtf8( example.first );
    const auto expected = QString::fromUtf8( example.second );
    CAPTURE( example.first );
    const auto stem = klogg::suggestedFileNameStem( label, QStringLiteral( "live-log" ) );
    CHECK( stem == expected );
    CHECK( klogg::suggestedFileNameStem( stem, QStringLiteral( "live-log" ) ) == stem );
}

TEST_CASE( "Suggested filename stems exclude whitespace and invisible characters",
           "[pathutils][save-filename]" )
{
    // Include NUL, C0, DEL, C1, non-breaking/Unicode spaces, and format controls.
    const auto codePoint = GENERATE( 0x0000, 0x0009, 0x000a, 0x000d, 0x001b, 0x001f,
                                     0x007f, 0x0085, 0x009f, 0x00a0, 0x1680, 0x2003,
                                     0x200b, 0x2028, 0x2029, 0x202e, 0x202f, 0x2060,
                                     0x3000, 0xfeff );
    const auto character = QChar{ static_cast<ushort>( codePoint ) };
    CAPTURE( codePoint );
    const auto label = QStringLiteral( "device" ) + character + character + QStringLiteral( "USB" );
    CHECK( klogg::suggestedFileNameStem( label, QStringLiteral( "live-log" ) )
           == QStringLiteral( "device-USB" ) );
}

TEST_CASE( "Suggested filename stems use a clean fallback when labels have no filename",
           "[pathutils][save-filename]" )
{
    const auto label = GENERATE( "", " ()[]{} ", "...", "--", "/\\:*?\"<>|", "\t\r\n" );
    CHECK( klogg::suggestedFileNameStem( QString::fromUtf8( label ), QStringLiteral( "live-log" ) )
           == QStringLiteral( "live-log" ) );
    CHECK( klogg::suggestedFileNameStem( QString::fromUtf8( label ), QStringLiteral( "fallback (log)" ) )
           == QStringLiteral( "fallback-log" ) );
    CHECK( klogg::suggestedFileNameStem( QString::fromUtf8( label ), QStringLiteral( "[]" ) )
           == QStringLiteral( "log" ) );
}

TEST_CASE( "Suggested filename stems avoid Windows reserved device names on every platform",
           "[pathutils][save-filename]" )
{
    const auto reserved = GENERATE( "CON", "con", "PRN", "AUX", "NUL", "COM1", "COM9",
                                    "LPT1", "LPT9", "COM¹", "COM²", "LPT³", "CON.trace",
                                    "CONIN$", "CONOUT$", "conin$", "CONOUT$.trace" );
    const auto label = QString::fromUtf8( reserved );
    const auto expected = QStringLiteral( "log-" ) + label;
    CAPTURE( reserved );
    CHECK( klogg::suggestedFileNameStem( label, QStringLiteral( "live-log" ) ) == expected );
    CHECK( klogg::suggestedFileNameStem( expected, QStringLiteral( "live-log" ) ) == expected );
    CHECK( klogg::suggestedFileNameStem( {}, label ) == expected );
}

TEST_CASE( "Suggested filename stems retain names merely resembling reserved devices",
           "[pathutils][save-filename]" )
{
    const auto label = QString::fromUtf8( GENERATE( "console", "CONSOLE", "COM0", "COM10",
                                                  "LPT0", "LPT10", "device.CON", "_NUL" ) );
    CHECK( klogg::suggestedFileNameStem( label, QStringLiteral( "live-log" ) ) == label );
}
