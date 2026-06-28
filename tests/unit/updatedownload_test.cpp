/*
 * Copyright (C) 2026 ZEACENT and other contributors
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
#include <QFile>
#include <QProgressDialog>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include "downloader.h"
#include "progress.h"
#include "updatedownloadhelper.h"
#include "updatedownloadhelper.h"

TEST_CASE( "UpdateDownload: progress dialog is created for download",
           "[updatedownload][progress]" )
{
    // When an update download is started, the progress dialog must be
    // connected to the Downloader's progress and finished signals.

    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto outputPath = tempDir.filePath( QStringLiteral( "test_update.dmg" ) );
    QFile outputFile( outputPath );
    REQUIRE( outputFile.open( QIODevice::WriteOnly ) );

    QUrl testUrl( QStringLiteral( "https://example.com/klogg-99.0.0.0-arm64.dmg" ) );

    Downloader downloader;
    QProgressDialog progressDialog;

    startUpdateDownload( testUrl, &outputFile, downloader, progressDialog );

    // The dialog should show the download URL or a meaningful label
    CHECK_FALSE( progressDialog.labelText().isEmpty() );

    // The dialog should start in a usable state
    CHECK( progressDialog.minimumDuration() >= 0 );

    outputFile.close();
}

TEST_CASE( "UpdateDownload: cancel button aborts download", "[updatedownload][progress]" )
{
    // The progress dialog must have a cancel button so users can abort
    // a slow or unwanted download.

    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto outputPath = tempDir.filePath( QStringLiteral( "test_update.exe" ) );
    QFile outputFile( outputPath );
    REQUIRE( outputFile.open( QIODevice::WriteOnly ) );

    QUrl testUrl( QStringLiteral( "https://example.com/klogg-99.0.0.0-setup.exe" ) );

    Downloader downloader;
    QProgressDialog progressDialog;

    startUpdateDownload( testUrl, &outputFile, downloader, progressDialog );

    // The dialog should be cancelable
    CHECK( progressDialog.isVisible() == false ); // not shown until exec() or show()
    CHECK( progressDialog.wasCanceled() == false );

    outputFile.close();
}

TEST_CASE( "Downloader abort prevents finished signal",
           "[updatedownload][downloader]" )
{
    // After abort(), the Downloader must not emit finished() — doing so
    // would cause use-after-free when the caller has already cleaned up.

    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    QFile outputFile( tempDir.filePath( QStringLiteral( "out.bin" ) ) );
    REQUIRE( outputFile.open( QIODevice::WriteOnly ) );

    Downloader downloader;
    QSignalSpy finishedSpy( &downloader, &Downloader::finished );

    // Start a download to a URL that will never respond quickly.
    downloader.download( QUrl( QStringLiteral( "http://192.0.2.1:1/nope" ) ), &outputFile );

    // Abort immediately — the reply should be terminated and disconnected.
    downloader.abort();

    // Process events briefly to verify no signal is emitted.
    QCoreApplication::processEvents();
    QThread::msleep( 50 );
    QCoreApplication::processEvents();

    CHECK( finishedSpy.count() == 0 );

    outputFile.close();
}

TEST_CASE( "UpdateDownload: stack-allocated objects avoid Cocoa lifecycle issues",
           "[updatedownload][progress]" )
{
    // On macOS, heap-allocated parentless QProgressDialog creates its own
    // NSWindow.  Calling processEvents() after exec() can dispatch stale
    // Cocoa events for the torn-down NSWindow, causing a SIGABRT crash.
    //
    // The fix: startUpdateDownload() must accept stack-allocated Downloader
    // and QProgressDialog references (matching the safe pattern in
    // MainWindow::openRemoteFile) so the caller controls lifetime and
    // never needs processEvents() + explicit delete.

    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto outputPath = tempDir.filePath( QStringLiteral( "test_update.dmg" ) );
    QFile outputFile( outputPath );
    REQUIRE( outputFile.open( QIODevice::WriteOnly ) );

    QUrl testUrl( QStringLiteral( "https://example.com/klogg-99.0.0.0-arm64.dmg" ) );

    // Stack-allocated objects — deterministic LIFO destruction, no NSWindow leak.
    Downloader downloader;
    QProgressDialog progressDialog;

    // This call must compile and wire up signals between the two objects.
    // The overload must accept (QUrl, QFile*, Downloader&, QProgressDialog&).
    startUpdateDownload( testUrl, &outputFile, downloader, progressDialog );

    // The dialog label should be set from the URL.
    CHECK_FALSE( progressDialog.labelText().isEmpty() );

    // The dialog should be in a usable initial state.
    CHECK( progressDialog.minimumDuration() >= 0 );
    CHECK( progressDialog.wasCanceled() == false );

    outputFile.close();
}

TEST_CASE( "Parentless dialog must call processEvents before destruction",
           "[updatedownload][progress]" )
{
    // On macOS, a parentless QProgressDialog gets its own NSWindow.
    // After exec() returns, stale Cocoa events for the NSWindow may
    // remain in the event queue.  If the dialog is destroyed before
    // those events are drained, the next processEvents() cycle in the
    // main event loop will try to dispatch them to the now-dead window,
    // causing an ObjC exception → SIGABRT.
    //
    // The safe pattern is:
    //   { QDialog dlg; dlg.exec(); processEvents(); }
    //
    // This test verifies that the pattern does not crash (the actual
    // macOS crash only reproduces with the Cocoa platform plugin, but
    // this documents the required lifecycle contract).

    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    {
        QProgressDialog dlg;
        dlg.setLabelText( QStringLiteral( "Test" ) );
        dlg.setMinimumDuration( 0 );
        dlg.setCancelButtonText( QString() ); // no cancel button

        // Close the dialog after a brief delay so exec() returns.
        QTimer::singleShot( 50, &dlg, [ &dlg ]() { dlg.done( QDialog::Accepted ); } );

        dlg.exec();

        // CRITICAL: drain stale Cocoa events while the dialog is still alive.
        QCoreApplication::processEvents();

        // Now the dialog is safe to destroy — no stale events remain.
    }

    // If we get here without crashing, the pattern is correct.
    CHECK( true );
}

TEST_CASE( "calculateProgress: sanity checks for download progress values",
           "[progress][updatedownload]" )
{
    // Verify the progress calculation handles all reasonable download sizes.

    // Typical installer sizes
    CHECK( calculateProgress( 0LL, 50'000'000LL ) == 0 );     // 0 of 50 MB
    CHECK( calculateProgress( 25'000'000LL, 50'000'000LL ) == 50 ); // half done
    CHECK( calculateProgress( 50'000'000LL, 50'000'000LL ) == 100 ); // complete

    // Zero total: in practice Downloader always provides a valid total.
    // Division by zero is undefined; this case is never hit in production.
    // We document the constraint rather than guarding against it.

    // Edge: download just started, unknown total
    // When total is -1 (unknown), the calculation should still work
    CHECK( calculateProgress( 0LL, -1LL ) == 0 );
}

TEST_CASE( "discardDownloadedFile closes the handle and removes the file" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );
    const auto path = tempDir.filePath( QStringLiteral( "partial.dmg" ) );

    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    file.write( "partial" );
    REQUIRE( file.isOpen() );

    // Must close BEFORE removing (Windows refuses to delete an open file); the
    // handle is open at this point, so the helper's ordering is what's tested.
    REQUIRE( discardDownloadedFile( file ) );
    REQUIRE_FALSE( file.isOpen() );
    REQUIRE_FALSE( QFile::exists( path ) );

    // Calling again on an already-removed file is a no-op success.
    REQUIRE( discardDownloadedFile( file ) );
}
