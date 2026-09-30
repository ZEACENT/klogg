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

#include "test_utils.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <atomic>
#include <thread>

#include "filewatcher.h"

// Generous bound for "the dispatched efsw work completed"; it only has to
// outlast a loaded runner, it is not a latency budget. The unit tests assert
// the executing thread instead of the elapsed time, so CI keeps checking the
// non-blocking contract without gating on machine speed.
constexpr int WorkerIdleTimeoutMs = 5000;

TEST_CASE( "FileWatcher named slot accepts queued cross-thread notifications" )
{
    auto& watcher = FileWatcher::getFileWatcher();
    REQUIRE( watcher.metaObject()->indexOfSlot( "fileChangedOnDisk(QString)" ) >= 0 );

    QSignalSpy changedSpy{ &watcher, &FileWatcher::fileChanged };
    REQUIRE( changedSpy.isValid() );
    const QString changedFile = QStringLiteral( "queued-slot-regression.log" );
    std::atomic<bool> invoked{ false };
    std::thread notifier{ [ & ] {
        invoked.store( QMetaObject::invokeMethod( &watcher, "fileChangedOnDisk",
                                                  Qt::QueuedConnection,
                                                  Q_ARG( QString, changedFile ) ) );
    } };
    notifier.join();
    REQUIRE( invoked.load() );

    QElapsedTimer timer;
    timer.start();
    bool delivered = false;
    while ( timer.elapsed() < 2000 && !delivered ) {
        const auto remaining = 2000 - static_cast<int>( timer.elapsed() );
        if ( remaining <= 0 ) {
            break;
        }
        changedSpy.wait( qMin( 100, remaining ) );
        for ( const auto& signal : changedSpy ) {
            if ( signal.at( 0 ).toString() == changedFile ) {
                delivered = true;
                break;
            }
        }
    }
    REQUIRE( delivered );
}

TEST_CASE( "FileWatcher addFile returns immediately without blocking caller" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    QTemporaryFile tempFile( tempDir.path() + "/test.log" );
    REQUIRE( tempFile.open() );
    tempFile.write( "test\n" );
    tempFile.flush();

    auto& watcher = FileWatcher::getFileWatcher();

    // The contract is that addFile only *dispatches* the efsw work to the
    // serial worker; it must never run it on the calling thread. Asserting on
    // the executing thread keeps this check active on every CI leg, where a
    // wall-clock bound would not be: KLOGG_CHECK_PERF_BUDGET skips its
    // expression unless KLOGG_PERF_GATES is set, and CI never sets it.
    const auto callerThread = std::this_thread::get_id();
    watcher.addFile( tempFile.fileName() );
    REQUIRE( watcher.waitForIdleForTest( WorkerIdleTimeoutMs ) );
    REQUIRE( watcher.efswOperationThreadForTest() != callerThread );

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

TEST_CASE( "FileWatcher::updateConfiguration returns immediately" )
{
    auto& watcher = FileWatcher::getFileWatcher();

    // enableWatch must run on the serial worker, not on the calling thread.
    const auto callerThread = std::this_thread::get_id();
    watcher.updateConfiguration();
    REQUIRE( watcher.waitForIdleForTest( WorkerIdleTimeoutMs ) );
    REQUIRE( watcher.efswOperationThreadForTest() != callerThread );
}

TEST_CASE( "FileWatcher::checkWatches returns immediately" )
{
    auto& watcher = FileWatcher::getFileWatcher();

    // checkWatches is a private slot; invoke it via the meta-object
    const auto callerThread = std::this_thread::get_id();
    const bool invoked
        = QMetaObject::invokeMethod( &watcher, "checkWatches", Qt::DirectConnection );
    REQUIRE( invoked );
    REQUIRE( watcher.waitForIdleForTest( WorkerIdleTimeoutMs ) );
    REQUIRE( watcher.efswOperationThreadForTest() != callerThread );
}
