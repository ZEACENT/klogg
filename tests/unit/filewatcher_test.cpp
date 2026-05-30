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

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include "filewatcher.h"

TEST_CASE( "FileWatcher addFile returns immediately without blocking caller" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    QTemporaryFile tempFile( tempDir.path() + "/test.log" );
    REQUIRE( tempFile.open() );
    tempFile.write( "test\n" );
    tempFile.flush();

    auto& watcher = FileWatcher::getFileWatcher();

    QElapsedTimer timer;
    timer.start();
    watcher.addFile( tempFile.fileName() );
    const auto elapsed = timer.elapsed();

    // addFile should return in well under 100ms (it dispatches to worker thread)
    CHECK( elapsed < 100 );

    watcher.removeFile( tempFile.fileName() );
}

TEST_CASE( "FileWatcher removeFile works after addFile" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    QTemporaryFile tempFile( tempDir.path() + "/test2.log" );
    REQUIRE( tempFile.open() );
    tempFile.write( "test\n" );
    tempFile.flush();

    auto& watcher = FileWatcher::getFileWatcher();

    watcher.addFile( tempFile.fileName() );
    QCoreApplication::processEvents();

    // Should not crash or hang
    watcher.removeFile( tempFile.fileName() );

    SUCCEED( "removeFile after addFile works" );
}
