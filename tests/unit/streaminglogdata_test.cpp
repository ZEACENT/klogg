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

#include <QDir>
#include <QTemporaryDir>
#include <QUuid>

#include "capturestore.h"
#include "streaminglogdata.h"
#include "test_utils.h"

namespace {
QString makeCaptureId()
{
    return QUuid::createUuid().toString( QUuid::WithoutBraces );
}
} // namespace

TEST_CASE( "StreamingLogData emits its ready signal asynchronously after listeners attach" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto captureId = makeCaptureId();
    {
        CaptureStore store( captureId, tempDir.path() );
        store.appendUtf8( QByteArrayLiteral( "one\ntwo\n" ) );
    }

    StreamingLogData logData( captureId, tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getNbLine().get() == 2 );
    REQUIRE( logData.getLineString( LineNumber( 0 ) ) == QStringLiteral( "one" ) );
    REQUIRE( logData.getLineString( LineNumber( 1 ) ) == QStringLiteral( "two" ) );
}

TEST_CASE( "StreamingLogData refreshes listeners after append and clear operations" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    loadingSpy.clear();

    logData.appendUtf8( QByteArrayLiteral( "alpha\nbeta\n" ) );
    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getNbLine().get() == 2 );
    REQUIRE( logData.getLineString( LineNumber( 1 ) ) == QStringLiteral( "beta" ) );

    loadingSpy.clear();
    logData.clearCapture();
    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getNbLine().get() == 0 );
}

TEST_CASE( "StreamingLogData exposes a trailing partial line when input finishes" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    loadingSpy.clear();

    logData.appendUtf8( QByteArrayLiteral( "partial" ) );
    REQUIRE( logData.getNbLine().get() == 0 );

    logData.finishInput();
    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getNbLine().get() == 1 );
    REQUIRE( logData.getLineString( LineNumber( 0 ) ) == QStringLiteral( "partial" ) );
}
