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

#include "pathutils.h"

#include <QChar>
#include <QVector>

namespace {

QString normalizeFileNameStem( const QString& label )
{
    const auto invalidCharacters = QStringLiteral( "<>:\"/\\|?*" );
    const auto separator = QLatin1Char( '-' );
    QString stem;
    stem.reserve( label.size() );
    bool separatorPending = false;
    // Classify Unicode scalars rather than UTF-16 halves, preserving supplementary
    // characters while still excluding format controls outside the BMP.
    for ( const auto codePoint : label.toUcs4() ) {
        const auto category = QChar::category( codePoint );
        const auto excluded = QChar::isSpace( codePoint ) || category == QChar::Other_Control
                              || category == QChar::Other_Format
                              || category == QChar::Punctuation_Open
                              || category == QChar::Punctuation_Close
                              || ( codePoint < 128
                                   && invalidCharacters.contains(
                                       QChar{ static_cast<ushort>( codePoint ) } ) );
        if ( excluded || codePoint == '-' ) {
            separatorPending = !stem.isEmpty();
            continue;
        }
        if ( stem.isEmpty() && codePoint == '.' ) {
            continue;
        }
        if ( separatorPending ) {
            stem += separator;
            separatorPending = false;
        }
        if ( QChar::requiresSurrogates( codePoint ) ) {
            stem += QChar{ QChar::highSurrogate( codePoint ) };
            stem += QChar{ QChar::lowSurrogate( codePoint ) };
        }
        else {
            stem += QChar{ static_cast<ushort>( codePoint ) };
        }
    }
    while ( stem.endsWith( QLatin1Char( '.' ) ) || stem.endsWith( separator ) ) {
        stem.chop( 1 );
    }
    return stem;
}

bool isReservedDeviceStem( const QString& stem )
{
    // Windows reserves these even with an extension (including an embedded one).
    const auto base = stem.section( QLatin1Char( '.' ), 0, 0 ).toUpper();
    if ( base == QStringLiteral( "CON" ) || base == QStringLiteral( "PRN" )
         || base == QStringLiteral( "AUX" ) || base == QStringLiteral( "NUL" )
         || base == QStringLiteral( "CONIN$" ) || base == QStringLiteral( "CONOUT$" ) ) {
        return true;
    }
    if ( base.size() != 4
         || ( !base.startsWith( QStringLiteral( "COM" ) )
              && !base.startsWith( QStringLiteral( "LPT" ) ) ) ) {
        return false;
    }
    // Win32 also treats superscript 1, 2 and 3 as device-number suffixes.
    return QStringLiteral( "123456789¹²³" ).contains( base.at( 3 ) );
}

} // namespace

QString klogg::suggestedFileNameStem( const QString& label, const QString& fallback )
{
    auto stem = normalizeFileNameStem( label );
    if ( stem.isEmpty() ) {
        stem = normalizeFileNameStem( fallback );
    }
    if ( stem.isEmpty() ) {
        stem = QStringLiteral( "log" );
    }
    if ( isReservedDeviceStem( stem ) ) {
        stem.prepend( QStringLiteral( "log-" ) );
    }
    return stem;
}
