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

// Starts an update download and returns a QProgressDialog that displays
// download progress. The dialog is modal — call exec() to show it.
//
// The returned QProgressDialog is owned by the caller.
// The outputFile must already be opened for writing and its lifetime must
// cover the download duration.
//
// On cancel, the download is aborted and the dialog closes.
// On completion, the dialog closes automatically — check the Downloader's
// finished signal for success/failure.

inline QProgressDialog* startUpdateDownload( const QUrl& url, QFile* outputFile,
                                              QWidget* parent = nullptr )
{
    auto* downloader = new Downloader();
    auto* progressDialog = new QProgressDialog( parent );

    progressDialog->setLabelText(
        QCoreApplication::translate( "KloggApp", "Downloading %1" ).arg( url.fileName() ) );
    progressDialog->setWindowTitle(
        QCoreApplication::translate( "KloggApp", "Klogg - Update Download" ) );
    progressDialog->setMinimumDuration( 0 );
    progressDialog->setCancelButtonText(
        QCoreApplication::translate( "KloggApp", "Cancel" ) );

    QObject::connect( downloader, &Downloader::downloadProgress,
                      [ progressDialog ]( qint64 bytesReceived, qint64 bytesTotal ) {
                          const auto progress = calculateProgress( bytesReceived, bytesTotal );
                          progressDialog->setRange( 0, 100 );
                          progressDialog->setValue( progress );
                      } );

    QObject::connect( downloader, &Downloader::finished,
                      [ progressDialog, downloader ]( bool success ) {
                          if ( !success ) {
                              progressDialog->setProperty( "downloadError",
                                                           downloader->lastError() );
                          }
                          progressDialog->done( success ? QDialog::Accepted : QDialog::Rejected );
                          downloader->deleteLater();
                      } );

    QObject::connect( progressDialog, &QProgressDialog::canceled, [ downloader ]() {
        downloader->abort();
        downloader->deleteLater();
    } );

    downloader->download( url, outputFile );

    return progressDialog;
}

#endif // KLOGG_UPDATEDOWNLOADHELPER_H
