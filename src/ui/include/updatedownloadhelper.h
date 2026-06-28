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

#ifndef KLOGG_UPDATEDOWNLOADHELPER_H
#define KLOGG_UPDATEDOWNLOADHELPER_H

#include <QCoreApplication>
#include <QFile>
#include <QProgressDialog>
#include <QUrl>

#include "downloader.h"
#include "progress.h"

// Wires up a stack-allocated Downloader and QProgressDialog for an update
// download.  The caller owns both objects and controls their lifetime.
//
// This is the safe pattern for macOS: stack allocation gives deterministic
// LIFO destruction, so no processEvents() or explicit delete is needed
// and no stale Cocoa NSWindow references can survive the function return.
//
// The outputFile must already be opened for writing and its lifetime must
// cover the download duration.
//
// On cancel, the download is aborted and the dialog closes (Rejected).
// On completion, the dialog closes automatically (Accepted on success,
// Rejected on failure).  Check progressDialog.property("downloadError")
// for the error string after exec() returns Rejected.
//
// IMPORTANT: abort() disconnects the network reply, so the finished()
// signal is never emitted after cancel.  Therefore it is safe to capture
// &downloader in the finished handler — the handler only runs when the
// download completes naturally (success or network error), never after
// a user-initiated abort.

inline void startUpdateDownload( const QUrl& url, QFile* outputFile,
                                 Downloader& downloader,
                                 QProgressDialog& progressDialog )
{
    progressDialog.setLabelText(
        QCoreApplication::translate( "KloggApp", "Downloading %1" ).arg( url.fileName() ) );
    progressDialog.setWindowTitle(
        QCoreApplication::translate( "KloggApp", "Klogg - Update Download" ) );
    progressDialog.setMinimumDuration( 0 );
    progressDialog.setCancelButtonText(
        QCoreApplication::translate( "KloggApp", "Cancel" ) );

    QObject::connect( &downloader, &Downloader::downloadProgress,
                      [ &progressDialog ]( qint64 bytesReceived, qint64 bytesTotal ) {
                          const auto progress = calculateProgress( bytesReceived, bytesTotal );
                          progressDialog.setRange( 0, 100 );
                          progressDialog.setValue( progress );
                      } );

    QObject::connect( &downloader, &Downloader::finished,
                      [ &progressDialog, &downloader ]( bool success ) {
                          if ( !success ) {
                              progressDialog.setProperty( "downloadError",
                                                         downloader.lastError() );
                          }
                          progressDialog.done( success ? QDialog::Accepted
                                                      : QDialog::Rejected );
                      } );

    QObject::connect( &progressDialog, &QProgressDialog::canceled, [ &downloader ]() {
        downloader.abort();
    } );

    downloader.download( url, outputFile );
}

// Close our write handle (if open) and delete a (partial) downloaded file.
// Closing BEFORE removing is required on Windows, which refuses to delete a
// file that is still open — the previous cancel path removed the file while the
// handle was still open, so on Windows the delete silently failed and the
// partial download leaked onto disk. On POSIX the file is unlinked regardless
// of an open handle, so this is an ordering no-op there, but the idiom is
// correct cross-platform.
inline bool discardDownloadedFile( QFile& file )
{
    if ( file.isOpen() ) {
        file.close();
    }
    if ( file.exists() ) {
        return file.remove();
    }
    return true;
}

#endif // KLOGG_UPDATEDOWNLOADHELPER_H
