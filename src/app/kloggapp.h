/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
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

#ifndef KLOGG_KLOGGAPP_H
#define KLOGG_KLOGGAPP_H

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <qapplication.h>
#include <stack>

#include <QApplication>
#include <vector>

#include <QCborValue>

#include <QDesktopServices>
#include <QDir>
#include <QFontDatabase>
#include <QMessageBox>
#include <QNetworkProxyFactory>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>

#include <memory>

#ifdef Q_OS_MAC
#include <QFileOpenEvent>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "configuration.h"
#include "crashhandler.h"
#include "klogg_version.h"
#include "log.h"
#include "session.h"
#include "uuid.h"

#include <kdsingleapplication.h>

#include "mainwindow.h"
#include "messagereceiver.h"
#include "newversiondialog.h"
#include "updatedownloadhelper.h"
#include "versionchecker.h"

class KloggApp : public QApplication {

    Q_OBJECT

  public:
    KloggApp( int& argc, char* argv[] )
        : QApplication( argc, argv)
    {
        QFontDatabase::addApplicationFont( ":/fonts/DejaVuSansMono.ttf" );

        qRegisterMetaType<LoadingStatus>( "LoadingStatus" );
        qRegisterMetaType<LinesCount>( "LinesCount" );
        qRegisterMetaType<LineNumber>( "LineNumber" );
        qRegisterMetaType<std::vector<LineNumber>>( "std::vector<LineNumber>" );
        qRegisterMetaType<klogg::vector<LineNumber>>( "klogg::vector<LineNumber>" );
        qRegisterMetaType<LineLength>( "LineLength" );
        qRegisterMetaType<Portion>( "Portion" );
        qRegisterMetaType<Selection>( "Selection" );
        qRegisterMetaType<QFNotification>( "QFNotification" );
        qRegisterMetaType<QFNotificationReachedEndOfFile>( "QFNotificationReachedEndOfFile" );
        qRegisterMetaType<QFNotificationReachedBegininningOfFile>(
            "QFNotificationReachedBegininningOfFile" );
        qRegisterMetaType<QFNotificationProgress>( "QFNotificationProgress" );
        qRegisterMetaType<QFNotificationInterrupted>( "QFNotificationInterrupted" );
        qRegisterMetaType<QuickFindMatcher>( "QuickFindMatcher" );

        if ( singleApplication_.isPrimaryInstance() ) {
            QObject::connect( &singleApplication_, &KDSingleApplication::messageReceived, &messageReceiver_,
                              &MessageReceiver::receiveMessage, Qt::QueuedConnection );

            QObject::connect( &messageReceiver_, &MessageReceiver::loadFile, this,
                              &KloggApp::loadFileNonInteractive );

            // Version checker notification
            connect( &versionChecker_, &VersionChecker::newVersionFound,
                     [ this ]( const QString& new_version, const QString& url,
                               const QString& downloadUrl, const QStringList& changes ) {
                         newVersionNotification( new_version, url, downloadUrl, changes );
                     } );

            // Manual check completed without finding a new version
            connect( &versionChecker_, &VersionChecker::checkCompleted,
                     []( bool newVersionFound, bool hadError ) {
                         Q_UNUSED( newVersionFound );
                         QMessageBox msgBox;
                         if ( hadError ) {
                             msgBox.setIcon( QMessageBox::Warning );
                             msgBox.setText( tr(
                                 "Unable to check for updates.\n"
                                 "Please verify your internet connection." ) );
                         }
                         else {
                             msgBox.setText(
                                 tr( "You are using the latest version of klogg." ) );
                         }
                         msgBox.exec();

                         // Drain stale Cocoa events while the dialog's NSWindow
                         // is still alive.  See newVersionNotification() for the
                         // full explanation of the macOS Cocoa lifecycle issue.
                         QCoreApplication::processEvents();
                     } );
        }
    }

    bool isSecondary() const {
        return !singleApplication_.isPrimaryInstance();
    }

    qint64 primaryPid() const {
        return singleApplication_.primaryPid();
    }

    void sendFilesToPrimaryInstance( std::vector<QString> filenames )
    {
#ifdef Q_OS_WIN
        // TODO: fix pid passing
        ::AllowSetForegroundWindow( static_cast<DWORD>( primaryPid() ) );
#endif

        QTimer::singleShot( 100, [ files = std::move( filenames ), this ] {
            QStringList filesToOpen;
            std::copy( files.cbegin(), files.cend(), std::back_inserter( filesToOpen ) );

            QVariantMap data;
            data.insert( "version", kloggVersion() );
            data.insert( "files", QVariant{ filesToOpen } );

            auto cbor = QCborValue::fromVariant( data );
            singleApplication_.sendMessageWithTimeout( cbor.toCbor(), 5000 );

            QTimer::singleShot( 100, this, &QApplication::quit );
        } );
    }

    void initCrashHandler()
    {
        crashHandler_ = std::make_unique<CrashHandler>();
    }

    MainWindow* reloadSession()
    {
        if ( !session_ ) {
            session_ = std::make_shared<Session>();
        }

        for ( auto&& windowSession : session_->windowSessions() ) {
            auto w = newWindow( std::move( windowSession ) );
            w->reloadGeometry();
            w->reloadSession();
            w->show();
        }

        if ( mainWindows_.empty() ) {
            auto w = newWindow();
            w->show();
        }

        return mainWindows_.back().second;
    }

    void clearInactiveSessions()
    {
        LOG_INFO << "Clear inactive sessions";

        auto existingSessions = session_->windowSessions();
        existingSessions.erase( std::remove_if( existingSessions.begin(), existingSessions.end(),
                                                [ this ]( const auto& session ) {
                                                    return std::any_of(
                                                        mainWindows_.begin(), mainWindows_.end(),
                                                        [ &session ]( const auto& window ) {
                                                            return window.first.windowId()
                                                                   == session.windowId();
                                                        } );
                                                } ),
                                existingSessions.end() );

        for ( auto& session : existingSessions ) {
            session.close();
        }
    }

    MainWindow* newWindow()
    {
        if ( !session_ ) {
            session_ = std::make_shared<Session>();
        }

        const auto previousSessions = session_->windowSessions();

        QByteArray geometry;
        if ( !previousSessions.empty() ) {
            previousSessions.back().restoreGeometry( &geometry );
        }

        auto window = newWindow( { session_, generateIdFromUuid(), nextWindowIndex() } );
        window->restoreGeometry( geometry );

        return window;
    }

    void loadFileNonInteractive( const QString& file )
    {
        while ( !activeWindows_.empty() && activeWindows_.top().isNull() ) {
            activeWindows_.pop();
        }

        if ( activeWindows_.empty() ) {
            newWindow();
        }

        activeWindows_.top()->loadFileNonInteractive( file );
    }

    void startBackgroundTasks()
    {
        LOG_DEBUG << "startBackgroundTasks";
        QTimer::singleShot( 0, this, [ this ] {
            QNetworkProxyFactory::setUseSystemConfiguration( true );
            versionChecker_.startCheck();
        } );
    }

#ifdef Q_OS_MAC
    bool event( QEvent* event ) override
    {
        if ( event->type() == QEvent::FileOpen ) {
            QFileOpenEvent* openEvent = static_cast<QFileOpenEvent*>( event );
            LOG_INFO << "File open request " << openEvent->file();

            if ( !isSecondary() ) {
                loadFileNonInteractive( openEvent->file() );
            }
            else {
                sendFilesToPrimaryInstance( { openEvent->file() } );
            }
        }

        return QApplication::event( event );
    }
#endif

  private:
    MainWindow* newWindow( WindowSession&& session )
    {
        mainWindows_.emplace_back( session, new MainWindow( session ) );

        auto& window = mainWindows_.back().second;

        activeWindows_.push( QPointer<MainWindow>( window ) );

        LOG_INFO << "Window " << &window << " created";
        connect( window, &MainWindow::newWindow, [ = ]() { newWindow()->show(); } );
        connect( window, &MainWindow::windowActivated,
                 [ this, window ]() { onWindowActivated( *window ); } );
        connect( window, &MainWindow::windowClosed,
                 [ this, window ]() { onWindowClosed( *window ); } );
        connect( window, &MainWindow::exitRequested, [ this ] { exitApplication(); } );
        connect( window, &MainWindow::checkForNewVersionRequested,
                 [ this ] { versionChecker_.forceCheck(); } );

        return window;
    }

    void onWindowActivated( MainWindow& window )
    {
        LOG_INFO << "Window " << &window << " activated";
        activeWindows_.push( QPointer<MainWindow>( &window ) );
    }

    void onWindowClosed( MainWindow& window )
    {
        LOG_INFO << "Window " << &window << " closed";
        auto w = std::find_if( mainWindows_.begin(), mainWindows_.end(),
                               [ &window ]( const auto& p ) { return p.second == &window; } );

        if ( w != mainWindows_.end() ) {
            mainWindows_.erase( w );
        }
    }

    void exitApplication()
    {
        LOG_INFO << "exit application";
        session_->setExitRequested( true );
        auto mainWindows = mainWindows_;
        mainWindows.reverse();
        for ( const auto& [ session, window ] : mainWindows ) {
            Q_UNUSED( session );
            window->close();
        }

        QTimer::singleShot( 100, this, &QCoreApplication::quit );
    }

    void newVersionNotification( const QString& new_version, const QString& url,
                                 const QString& downloadUrl, const QStringList& changes )
    {
        LOG_DEBUG << "newVersionNotification( " << new_version << " from " << url
                  << ", download: " << downloadUrl << " )";

        NewVersionDialog dlg( new_version, url, changes );
        dlg.exec();

        // Drain stale Cocoa events while the dialog's NSWindow is still alive.
        // Without this, the destructor releases the NSWindow but orphaned events
        // remain in the queue; the next processEvents() cycle in the main event
        // loop tries to dispatch them to the dead window → ObjC exception → SIGABRT.
        QCoreApplication::processEvents();

        if ( dlg.clickedButton() == NewVersionDialog::Download ) {
            if ( !downloadUrl.isEmpty() ) {
                downloadAndOpenUpdate( downloadUrl, new_version );
            }
            else {
                // Fallback: open the release page in the browser
                QDesktopServices::openUrl( QUrl( url ) );
            }
        }
        else if ( dlg.clickedButton() == NewVersionDialog::RemindLater ) {
            // Set deadline to 1 day from now
            auto& config = VersionCheckerConfig::get();
            config.setNextDeadline( std::time( nullptr ) + 86400 ); // 24 hours
            config.save();
        }
        else if ( dlg.clickedButton() == NewVersionDialog::SkipVersion ) {
            // Store the version to ignore
            auto& config = VersionCheckerConfig::get();
            config.setIgnoredVersion( new_version );
            config.save();
        }
    }

    void downloadAndOpenUpdate( const QString& downloadUrl, const QString& version )
    {
        Q_UNUSED( version );
        LOG_INFO << "Downloading update from " << downloadUrl;

        const auto downloadsPath
            = QStandardPaths::writableLocation( QStandardPaths::DownloadLocation );
        const auto urlFileName = QUrl( downloadUrl ).fileName();
        const auto localPath = downloadsPath + QDir::separator() + urlFileName;

        QFile outputFile( localPath );
        if ( !outputFile.open( QIODevice::WriteOnly ) ) {
            LOG_ERROR << "Cannot open file for writing: " << localPath;
            QMessageBox::warning( nullptr, tr( "Download Failed" ),
                                  tr( "Could not create file:\n%1" ).arg( localPath ) );
            return;
        }

        // Stack-allocated objects — same safe pattern as
        // MainWindow::openRemoteFile().  Deterministic LIFO destruction
        // avoids the macOS Cocoa crash caused by processEvents() +
        // delete after a modal dialog exec().
        Downloader downloader;
        QProgressDialog progressDialog;

        startUpdateDownload( QUrl( downloadUrl ), &outputFile, downloader, progressDialog );

        const auto result = progressDialog.exec();

        // Drain stale Cocoa events while the dialog's NSWindow is still alive.
        // See comment in newVersionNotification() for the full explanation.
        QCoreApplication::processEvents();

        if ( result == QDialog::Accepted ) {
            LOG_INFO << "Update downloaded to " << localPath;
            outputFile.close();
            QDesktopServices::openUrl( QUrl::fromLocalFile( localPath ) );
        }
        else {
            const auto error = progressDialog.property( "downloadError" ).toString();
            if ( !error.isEmpty() ) {
                LOG_ERROR << "Update download failed: " << error;
                QMessageBox::warning( nullptr, tr( "Download Failed" ),
                                      tr( "Download failed:\n%1" ).arg( error ) );
            }
            else {
                LOG_INFO << "Update download canceled by user";
            }
            // Closes our handle before removing: Windows refuses to delete an
            // open file, so removing first silently leaked the partial download.
            discardDownloadedFile( outputFile );
        }
    }

    size_t nextWindowIndex() const
    {
        if ( mainWindows_.empty() ) {
            return 0;
        }
        else {
            const auto windowWithMaxIndex = std::max_element(
                mainWindows_.begin(), mainWindows_.end(), []( const auto& lhs, const auto& rhs ) {
                    return lhs.first.windowIndex() < rhs.first.windowIndex();
                } );
            return windowWithMaxIndex->first.windowIndex() + 1;
        }
    }

  private:
    KDSingleApplication singleApplication_;
    std::unique_ptr<CrashHandler> crashHandler_;

    MessageReceiver messageReceiver_;

    std::shared_ptr<Session> session_;

    std::list<std::pair<WindowSession, MainWindow*>> mainWindows_;
    std::stack<QPointer<MainWindow>> activeWindows_;

    VersionChecker versionChecker_;
};

#endif // KLOGG_KLOGGAPP_H
