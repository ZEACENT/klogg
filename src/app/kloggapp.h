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
#include "downloader.h"
#include "klogg_version.h"
#include "log.h"
#include "session.h"
#include "uuid.h"

#include <kdsingleapplication.h>

#include "mainwindow.h"
#include "messagereceiver.h"
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

        QString message = tr( "<p>A new version of klogg (%1) is available for download.</p>"
                              "<p><a href=\"%2\">%2</a></p>" )
                              .arg( new_version, url );

        if ( !changes.empty() ) {
            message.append( tr( "<p>Important changes:</p><ul>" ) );
            for ( const auto& change : changes ) {
                message.append( tr( "<li>%1</li>" ).arg( change ) );
            }
            message.append( QStringLiteral( "</ul>" ) );
        }

        QMessageBox msgBox;
        msgBox.setWindowTitle( tr( "New Version Available" ) );
        msgBox.setText( message );
        msgBox.setTextFormat( Qt::RichText );
        msgBox.setIcon( QMessageBox::Information );

        QPushButton* downloadButton
            = msgBox.addButton( tr( "Download" ), QMessageBox::AcceptRole );
        QPushButton* remindButton
            = msgBox.addButton( tr( "Remind Later" ), QMessageBox::RejectRole );
        QPushButton* skipButton
            = msgBox.addButton( tr( "Skip This Version" ),
                                QMessageBox::DestructiveRole );

        msgBox.setDefaultButton( downloadButton );
        msgBox.exec();

        if ( msgBox.clickedButton() == downloadButton ) {
            if ( !downloadUrl.isEmpty() ) {
                downloadAndOpenUpdate( downloadUrl, new_version );
            }
            else {
                // Fallback: open the release page in the browser
                QDesktopServices::openUrl( QUrl( url ) );
            }
        }
        else if ( msgBox.clickedButton() == remindButton ) {
            // Set deadline to 1 day from now
            auto& config = VersionCheckerConfig::get();
            config.setNextDeadline( std::time( nullptr ) + 86400 ); // 24 hours
            config.save();
        }
        else if ( msgBox.clickedButton() == skipButton ) {
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

        // outputFile is shared with the completion lambda so it stays alive
        // until the download finishes (no cycle — QFile is not a QObject).
        auto outputFile = std::make_shared<QFile>( localPath );
        if ( !outputFile->open( QIODevice::WriteOnly ) ) {
            LOG_ERROR << "Cannot open file for writing: " << localPath;
            QMessageBox::warning( nullptr, tr( "Download Failed" ),
                                  tr( "Could not create file:\n%1" ).arg( localPath ) );
            return;
        }

        // Use raw new + deleteLater to avoid the shared_ptr cycle that would
        // leak, while keeping the Downloader alive for the async request.
        auto* downloader = new Downloader();

        QObject::connect( downloader, &Downloader::finished,
                          [ outputFile, downloader, localPath ]( bool success ) {
                              outputFile->close();
                              if ( success ) {
                                  LOG_INFO << "Update downloaded to " << localPath;
                                  QDesktopServices::openUrl( QUrl::fromLocalFile( localPath ) );
                              }
                              else {
                                  LOG_ERROR << "Update download failed: "
                                            << downloader->lastError();
                                  QMessageBox::warning(
                                      nullptr, tr( "Download Failed" ),
                                      tr( "Failed to download update:\n%1" )
                                          .arg( downloader->lastError() ) );
                                  outputFile->remove();
                              }
                              downloader->deleteLater();
                          } );

        downloader->download( QUrl( downloadUrl ), outputFile.get() );
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
