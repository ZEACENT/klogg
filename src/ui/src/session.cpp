/*
 * Copyright (C) 2013, 2014 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "session.h"

#include "adbinfrastructuremanager.h"
#include "adblogcatsource.h"
#include "ioscatalogprovider.h"
#include "livelogcontroller.h"
#include "livelogsession.h"
#include "log.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>

#include "foldercrawlerwidget.h"
#include "folderenumeration.h"
#include "logdata.h"
#include "logfiltereddata.h"
#include "pathutils.h"
#include "savedsearches.h"
#include "sessioninfo.h"
#include "streaminglogdata.h"
#include "viewinterface.h"

namespace {

// Fatal diagnostics only: info diagnostics never refuse a restore.
QStringList fatalMessagesOf( const std::vector<klogg::livelog::Diagnostic>& diagnostics )
{
    QStringList messages;
    for ( const auto& diagnostic : diagnostics ) {
        if ( diagnostic.severity == klogg::livelog::Diagnostic::Severity::Fatal ) {
            messages << diagnostic.message;
        }
    }
    return messages;
}

class SessionLiveLogEffects final : public klogg::livelog::LiveLogControllerEffects {
public:
    SessionLiveLogEffects(
        std::shared_ptr<AdbLogcatSource> source, klogg::livelog::LiveLogSessionSpec spec,
        klogg::livecapture::adb::AdbInfrastructureManager* adbInfrastructure,
        klogg::livecapture::ios::IosCatalogSnapshotProvider* iosCatalog )
        : source_( std::move( source ) )
        , spec_( std::move( spec ) )
        , adbInfrastructure_( adbInfrastructure )
        , iosCatalog_( iosCatalog )
    {
    }

    ~SessionLiveLogEffects() override
    {
        QObject::disconnect( captureConnection_ );
        stopAvailabilityObservation();
        source_->setControllerCallbacks( {}, {}, {} );
    }

    void attach( klogg::livelog::LiveLogController& controller )
    {
        controller_ = &controller;
        captureConnection_ = QObject::connect(
            source_.get(), &AdbLogcatSource::captureOutputChanged, source_.get(),
            [ this ]( bool healthy, const QString& detail ) {
                if ( controller_ == nullptr ) {
                    return;
                }
                const auto generation = controller_->snapshot().captureGeneration;
                if ( healthy ) {
                    controller_->captureChanged( generation,
                                                 klogg::livecapture::CaptureState::OpenHealthy );
                    return;
                }
                controller_->captureChanged(
                    generation, klogg::livecapture::CaptureState::OutputDegraded,
                    klogg::livecapture::LiveSourceError{
                        klogg::livecapture::ErrorCategory::Capture, "output-write-failed",
                        klogg::livecapture::ErrorScope::Capture,
                        klogg::livecapture::RetryPolicy::Never,
                        "The bound capture output could not be written.", detail.toStdString() } );
            } );
        source_->setControllerCallbacks(
            [ this ]( auto generation, const QByteArray& bytes ) {
                controller_->streamBytesReceived( generation, bytes );
            },
            [ this ]( auto generation, LiveSourceTransport::State state ) {
                switch ( state ) {
                case LiveSourceTransport::State::Connected:
                    controller_->protocolServiceReady( generation );
                    controller_->streamHandleOpened( generation );
                    controller_->streamReadArmed( generation );
                    break;
                case LiveSourceTransport::State::Disconnected:
                    if ( controller_->snapshot().runIntent
                         == klogg::livecapture::RunIntent::Stopped ) {
                        controller_->stopCompleted( generation );
                    }
                    else {
                        controller_->streamFailed(
                            generation,
                            klogg::livecapture::LiveSourceError{
                                klogg::livecapture::ErrorCategory::Stream,
                                "live-stream-disconnected",
                                klogg::livecapture::ErrorScope::Stream,
                                klogg::livecapture::RetryPolicy::Backoff,
                                "The live stream disconnected unexpectedly.",
                                "The transport reported Disconnected for the active generation."
                            } );
                    }
                    break;
                case LiveSourceTransport::State::Connecting:
                case LiveSourceTransport::State::Error:
                    break;
                }
            },
            [ this ]( auto generation, klogg::livecapture::LiveSourceError error ) {
                controller_->streamFailed( generation, std::move( error ) );
            },
            [ this ] { controller_->stopRequested(); },
            [ this ] { controller_->startRequested(); } );
    }

    void invalidateGeneration( klogg::livecapture::Generation generation ) override
    {
        source_->invalidateTransportGeneration( generation );
    }

    void cancelStream( klogg::livecapture::Generation generation ) override
    {
        source_->cancelTransport( generation );
        if ( controller_ != nullptr
             && controller_->snapshot().source.status
                    == klogg::livecapture::SourceStatus::Stopping ) {
            controller_->stopCompleted( generation );
        }
        if ( controller_ != nullptr
             && controller_->snapshot().runIntent == klogg::livecapture::RunIntent::Stopped ) {
            const auto captureGeneration = controller_->snapshot().captureGeneration;
            controller_->captureChanged( captureGeneration,
                                         klogg::livecapture::CaptureState::Finalizing );
            controller_->captureChanged( captureGeneration,
                                         klogg::livecapture::CaptureState::Finalized );
            stopAvailabilityObservation();
        }
    }

    void startInfrastructure( klogg::livecapture::Generation generation ) override
    {
        if ( spec_.sourceKind == klogg::livelog::SourceKind::AndroidLogcat
             && adbInfrastructure_ != nullptr ) {
            startAdbObservation();
            return;
        }
        if ( spec_.sourceKind == klogg::livelog::SourceKind::IosSyslog
             && iosCatalog_ != nullptr ) {
            startIosObservation();
            return;
        }

        // Isolated tests and compatibility callers without application service
        // roots retain the deterministic direct transport path.
        controller_->infrastructureChanged(
            klogg::livecapture::InfrastructureStatus::Ready,
            klogg::livecapture::InfrastructureOwnership::AppShared );
        controller_->deviceAvailable( generation );
    }

    void openStream( klogg::livecapture::Generation generation,
                     const LiveSourceTransportConfig& config ) override
    {
        source_->openTransport( generation, config );
    }

    void appendBytes( klogg::livecapture::Generation generation,
                      const QByteArray& bytes ) override
    {
        source_->appendTransportBytes( generation, bytes );
    }

private:
    void startAdbObservation()
    {
        if ( adbConnection_ ) {
            observeAdbSnapshot( adbInfrastructure_->snapshot() );
            return;
        }

        adbConnection_ = QObject::connect(
            adbInfrastructure_,
            &klogg::livecapture::adb::AdbInfrastructureManager::snapshotChanged, source_.get(),
            [ this ]( const klogg::livecapture::adb::AdbInfrastructureSnapshot& snapshot ) {
                observeAdbSnapshot( snapshot );
            } );
        adbLease_ = adbInfrastructure_->acquireLease();
        observeAdbSnapshot( adbInfrastructure_->snapshot() );
    }

    void observeAdbSnapshot(
        const klogg::livecapture::adb::AdbInfrastructureSnapshot& snapshot )
    {
        if ( controller_ == nullptr
             || controller_->snapshot().runIntent != klogg::livecapture::RunIntent::Running ) {
            return;
        }

        controller_->infrastructureChanged(
            snapshot.infrastructure.status,
            snapshot.infrastructure.ownership.value_or(
                klogg::livecapture::InfrastructureOwnership::AppShared ) );
        if ( snapshot.infrastructure.status
             != klogg::livecapture::InfrastructureStatus::Ready ) {
            if ( snapshot.error.has_value()
                 && snapshot.error->retryPolicy == klogg::livecapture::RetryPolicy::Never ) {
                controller_->infrastructureFailed( controller_->snapshot().generation,
                                                   *snapshot.error );
            }
            return;
        }
        if ( !snapshot.hasCurrentDevices() ) {
            return;
        }

        const auto serial = spec_.device.deviceId.toStdString();
        const auto device = std::find_if(
            snapshot.devices.devices.cbegin(), snapshot.devices.devices.cend(),
            [ &serial ]( const auto& candidate ) { return candidate.serial == serial; } );
        const auto generation = controller_->snapshot().generation;
        if ( device == snapshot.devices.devices.cend() ) {
            controller_->deviceAbsent( generation );
            return;
        }

        using AdbDeviceState = klogg::livecapture::adb::AdbDeviceState;
        switch ( device->state ) {
        case AdbDeviceState::Online:
            controller_->deviceAvailable( generation );
            break;
        case AdbDeviceState::Unauthorized:
            controller_->userActionRequired(
                generation, klogg::livecapture::AwaitingUserReason::Authorize );
            break;
        case AdbDeviceState::Offline:
        case AdbDeviceState::Other:
            controller_->deviceAbsent( generation );
            break;
        }
    }

    klogg::livecapture::ios::IosEndpointKey iosEndpoint() const
    {
        klogg::livecapture::ios::IosEndpointKey endpoint;
        endpoint.udid = spec_.device.deviceId.toStdString();
        endpoint.connectionType
            = spec_.device.connection == klogg::livelog::DeviceIdentity::Connection::Network
                  ? klogg::livecapture::ios::NativeConnectionType::Network
                  : klogg::livecapture::ios::NativeConnectionType::Usb;
        return endpoint;
    }

    bool retryRecoverableIosMetadata(
        const klogg::livecapture::ios::IosCatalogSnapshot& snapshot )
    {
        auto* const requester
            = dynamic_cast<klogg::livecapture::ios::IosCatalogMetadataRequester*>( iosCatalog_ );
        if ( requester == nullptr ) {
            return false;
        }
        const auto endpoint = iosEndpoint();
        const auto entry = std::find_if(
            snapshot.entries.cbegin(), snapshot.entries.cend(),
            [ &endpoint ]( const auto& candidate ) { return candidate.endpoint == endpoint; } );
        if ( entry == snapshot.entries.cend() || !entry->error.has_value() ) {
            return false;
        }
        const auto error = entry->error.value_or( klogg::livecapture::ios::IosCatalogError{} );
        if ( error.error.retryPolicy == klogg::livecapture::RetryPolicy::Never ) {
            return false;
        }
        requester->requestMetadata( endpoint );
        return true;
    }

    void startIosObservation()
    {
        if ( const auto startupError = iosCatalog_->startupError(); startupError.has_value() ) {
            controller_->infrastructureChanged(
                klogg::livecapture::InfrastructureStatus::Unavailable,
                klogg::livecapture::InfrastructureOwnership::AppShared );
            controller_->infrastructureFailed(
                controller_->snapshot().generation,
                startupError.value_or( klogg::livecapture::LiveSourceError{} ) );
            return;
        }

        if ( !iosSubscription_.has_value() ) {
            const auto observationEpoch = ++iosObservationEpoch_;
            const QPointer<AdbLogcatSource> context( source_.get() );
            iosSubscription_ = iosCatalog_->subscribe(
                [ this, context,
                  observationEpoch ]( const klogg::livecapture::ios::IosCatalogSnapshot& ) {
                    // The catalog invokes callbacks from its native monitor thread;
                    // nothing here may escape into vendor code.
                    try { // NOLINT(bugprone-exception-escape)
                        if ( context == nullptr ) {
                            return;
                        }
                        QMetaObject::invokeMethod(
                            context.data(),
                            // NOLINTNEXTLINE(bugprone-exception-escape)
                            [ this, context, observationEpoch ] {
                                // A callback invalidates the catalog view. Read the latest
                                // snapshot after queued delivery so an older notification
                                // cannot overwrite a synchronous reconnect replay.
                                try { // NOLINT(bugprone-exception-escape)
                                    if ( context != nullptr
                                         && observationEpoch == iosObservationEpoch_ ) {
                                        observeIosSnapshot( iosCatalog_->snapshot() );
                                    }
                                } catch ( ... ) { // NOLINT(bugprone-empty-catch)
                                }
                            },
                            Qt::AutoConnection );
                    } catch ( ... ) { // NOLINT(bugprone-empty-catch)
                    }
                } );
        }
        controller_->infrastructureChanged(
            klogg::livecapture::InfrastructureStatus::Ready,
            klogg::livecapture::InfrastructureOwnership::AppShared );
        const auto snapshot = iosCatalog_->snapshot();
        // requestMetadata() leaves the old error in this captured snapshot until
        // asynchronous completion. Do not replay the failure while that retry is pending.
        if ( !retryRecoverableIosMetadata( snapshot ) ) {
            observeIosSnapshot( snapshot );
        }
    }

    void observeIosSnapshot( const klogg::livecapture::ios::IosCatalogSnapshot& snapshot )
    {
        if ( controller_ == nullptr || snapshot.generation < lastIosSnapshotGeneration_
             || controller_->snapshot().runIntent != klogg::livecapture::RunIntent::Running ) {
            return;
        }
        lastIosSnapshotGeneration_ = snapshot.generation;

        const auto endpoint = iosEndpoint();
        const auto entry = std::find_if(
            snapshot.entries.cbegin(), snapshot.entries.cend(),
            [ &endpoint ]( const auto& candidate ) { return candidate.endpoint == endpoint; } );
        const auto generation = controller_->snapshot().generation;
        if ( entry == snapshot.entries.cend() ) {
            controller_->deviceAbsent( generation );
            return;
        }
        if ( entry->error.has_value() ) {
            const auto error
                = entry->error.value_or( klogg::livecapture::ios::IosCatalogError{} );
            if ( error.error.retryPolicy == klogg::livecapture::RetryPolicy::AwaitUser
                 && error.awaitingUserReason.has_value() ) {
                controller_->userActionRequired(
                    generation,
                    error.awaitingUserReason.value_or(
                        klogg::livecapture::AwaitingUserReason::Authorize ) );
            }
            else {
                controller_->availabilityFailed( generation, error.error );
            }
            return;
        }
        controller_->deviceAvailable( generation );
    }

    void stopAvailabilityObservation()
    {
        if ( adbConnection_ ) {
            QObject::disconnect( adbConnection_ );
            adbConnection_ = {};
        }
        adbLease_.reset();
        if ( iosCatalog_ != nullptr && iosSubscription_.has_value() ) {
            ++iosObservationEpoch_;
            iosCatalog_->unsubscribe( *iosSubscription_ );
            iosSubscription_.reset();
        }
    }

    std::shared_ptr<AdbLogcatSource> source_;
    klogg::livelog::LiveLogSessionSpec spec_;
    klogg::livecapture::adb::AdbInfrastructureManager* adbInfrastructure_{ nullptr };
    klogg::livecapture::ios::IosCatalogSnapshotProvider* iosCatalog_{ nullptr };
    klogg::livecapture::adb::AdbInfrastructureLease adbLease_;
    QMetaObject::Connection adbConnection_;
    QMetaObject::Connection captureConnection_;
    std::optional<klogg::livecapture::ios::IosCatalogSnapshotProvider::SubscriptionId>
        iosSubscription_;
    std::uint64_t iosObservationEpoch_{ 0 };
    klogg::livecapture::Generation lastIosSnapshotGeneration_{ 0 };
    klogg::livelog::LiveLogController* controller_{ nullptr };
};

klogg::livelog::LiveLogControllerConfig
controllerConfigFor( const klogg::livelog::LiveLogSessionSpec& spec )
{
    klogg::livelog::LiveLogControllerConfig config;
    if ( spec.capture.maxReconnectAttempts > 0 ) {
        config.reducer.maxRetryAttempts
            = static_cast<unsigned>( spec.capture.maxReconnectAttempts ) + 1u;
    }
    else if ( spec.capture.autoReconnectEnabled ) {
        config.reducer.maxRetryAttempts = std::numeric_limits<unsigned>::max();
    }
    return config;
}

} // namespace

Session::Session()
{
    // Get the global search history (it remains the property
    // of the Persistent)
    savedSearches_ = &SavedSearches::getSynced();
    SessionInfo::getSynced();

    quickFindPattern_ = std::make_shared<QuickFindPattern>();
}

Session::Session( const LiveSourceTransportFactory& transportFactory )
    : Session( transportFactory, nullptr, nullptr )
{
}

Session::Session(
    const LiveSourceTransportFactory& transportFactory,
    klogg::livecapture::adb::AdbInfrastructureManager* adbInfrastructure,
    klogg::livecapture::ios::IosCatalogSnapshotProvider* iosCatalog )
    : Session()
{
    transportFactory_ = &transportFactory;
    adbInfrastructure_ = adbInfrastructure;
    iosCatalog_ = iosCatalog;
}

const LiveSourceTransportFactory& Session::transportFactory() const
{
    assert( transportFactory_ != nullptr );
    return *transportFactory_;
}

Session::~Session()
{
    // FIXME Clean up all the data objects...
}

ViewInterface* Session::getViewIfOpen( const QString& file_name ) const
{
    auto result = std::find_if( openFiles_.begin(), openFiles_.end(),
                                [ & ]( const std::pair<const ViewInterface*, OpenFile>& o ) {
                                    return ( o.second.fileName == file_name );
                                } );

    if ( result != openFiles_.end() )
        return result->second.view;
    else
        return nullptr;
}

ViewInterface* Session::open( const QString& file_name,
                              const std::function<ViewInterface*()>& view_factory )
{
    return openAlways( file_name, view_factory, {} );
}

void Session::close( const ViewInterface* view )
{
    openFiles_.erase( openFiles_.find( view ) );
}

ViewInterface* Session::openMerged( const std::vector<QString>& fileNames,
                                    const std::function<ViewInterface*()>& view_factory,
                                    const QString& tempDir )
{
    if ( fileNames.empty() ) {
        return nullptr;
    }

    // Build display name from source file names
    QStringList shortNames;
    for ( const auto& fn : fileNames ) {
        shortNames << klogg::displayNameForPath( fn );
    }
    const QString mergedName = QString( "[Merged] %1" ).arg( shortNames.join( " + " ) );

    // Create temp file with concatenated content
    const QString tempFilePath = QDir( tempDir ).filePath(
        QString( "klogg_merged_%1.log" ).arg( QDateTime::currentMSecsSinceEpoch() ) );

    QFile tempFile( tempFilePath );
    if ( !tempFile.open( QIODevice::WriteOnly ) ) {
        LOG_ERROR << "Failed to create merged temp file: " << tempFilePath.toStdString();
        return nullptr;
    }

    // Copy raw bytes to preserve original encoding (no text transcoding)
    for ( size_t fi = 0; fi < fileNames.size(); ++fi ) {
        QFile sourceFile( fileNames[ fi ] );
        if ( sourceFile.open( QIODevice::ReadOnly ) ) {
            static constexpr qint64 BufSize = 65536;
            char buf[ BufSize ];
            qint64 bytesRead;
            char lastByte = '\n'; // default to newline so we don't prepend one for the first file
            while ( ( bytesRead = sourceFile.read( buf, BufSize ) ) > 0 ) {
                tempFile.write( buf, bytesRead );
                lastByte = buf[ bytesRead - 1 ];
            }
            // If this is not the last file and the file didn't end with a newline,
            // insert one so the next file starts on its own line
            if ( fi + 1 < fileNames.size() && lastByte != '\n' ) {
                tempFile.write( "\n", 1 );
            }
        }
        else {
            LOG_ERROR << "Failed to open source file for merge: " << fileNames[ fi ].toStdString();
        }
    }
    tempFile.close();

    // Use the normal open flow with the temp file
    return openAlways( tempFilePath, view_factory, {} );
}

ViewInterface* Session::openFolder( const QString& folderPath,
                                    const std::vector<QString>& filePaths )
{
    if ( folderPath.isEmpty() || filePaths.empty() ) {
        // Empty filePaths means the folder is gone/empty: return nullptr so the
        // restore loop skips it gracefully rather than showing an empty tab.
        return nullptr;
    }

    auto* view = new FolderCrawlerWidget();
    view->setQuickFindPattern( quickFindPattern_ );
    view->setSavedSearches( savedSearches_ );

    QStringList paths;
    paths.reserve( static_cast<int>( filePaths.size() ) );
    for ( const auto& p : filePaths ) {
        paths << p;
    }
    view->setFolder( folderPath, paths );

    // Robust to trailing slashes (drag-dropped folders carry one): Qt's
    // QFileInfo(path).fileName() returns "" for "/.../Logs/", which would
    // produce a blank tab title. See klogg::displayNameForPath.
    const QString displayName = klogg::displayNameForPath( folderPath );

    openFiles_.insert( { view,
                         OpenFile{ folderPath, // fileName
                                   folderPath, // documentId
                                   displayName,
                                   folderPath, // associatedPath
                                   DocumentKind::Folder,
                                   nullptr, // logData (folder mode streams, no index)
                                   nullptr, // logFilteredData
                                   nullptr, // adbLogcatSource
                                   view,
                                   nullptr, // liveLogEffects
                                   nullptr } } ); // liveLogController

    return view;
}

ViewInterface* Session::openAdbLogcat( const AdbLogcatSessionData& sessionData,
                                       const std::function<ViewInterface*()>& view_factory,
                                       bool startConnected, const QString& viewContext )
{
    return openAdbAlways( sessionData, view_factory, startConnected, viewContext );
}

QString Session::getFilename( const ViewInterface* view ) const
{
    const OpenFile* file = findOpenFileFromView( view );

    assert( file );

    return file->fileName;
}

QString Session::getDocumentId( const ViewInterface* view ) const
{
    const OpenFile* file = findOpenFileFromView( view );

    assert( file );

    return file->documentId;
}

QString Session::getDisplayName( const ViewInterface* view ) const
{
    const OpenFile* file = findOpenFileFromView( view );

    assert( file );

    if ( file->kind == DocumentKind::AdbLogcat && file->adbLogcatSource ) {
        return file->adbLogcatSource->sessionData().displayName();
    }

    return file->displayName;
}

QString Session::getAssociatedPath( const ViewInterface* view ) const
{
    const OpenFile* file = findOpenFileFromView( view );

    assert( file );

    if ( file->kind == DocumentKind::AdbLogcat && file->adbLogcatSource ) {
        return file->adbLogcatSource->sessionData().associatedPath();
    }

    return file->associatedPath;
}

DocumentKind Session::getDocumentKind( const ViewInterface* view ) const
{
    const OpenFile* file = findOpenFileFromView( view );

    assert( file );

    return file->kind;
}

AdbLogcatSource* Session::getAdbLogcatSource( const ViewInterface* view ) const
{
    const OpenFile* file = findOpenFileFromView( view );

    assert( file );

    return file->adbLogcatSource.get();
}

klogg::livelog::LiveLogController*
Session::getLiveLogController( const ViewInterface* view ) const
{
    const OpenFile* file = findOpenFileFromView( view );
    assert( file );
    return file->liveLogController.get();
}

QStringList Session::lastRestoreRejections() const
{
    return restoreRejections_;
}

QStringList Session::lastRestoreNotices() const
{
    return restoreNotices_;
}

void Session::appendRestoreNoticeOncePerDocument( const QString& documentId, QString notice )
{
    if ( documentId.isEmpty() || notice.isEmpty() ) {
        return;
    }

    const auto noticeKey = documentId + QChar{ 0x1f } + notice;
    if ( !restoreNotifiedNoticeKeys_.contains( noticeKey ) ) {
        restoreNotifiedNoticeKeys_.insert( noticeKey );
        restoreNotices_.append( std::move( notice ) );
    }
}

void Session::getFileInfo( const ViewInterface* view, uint64_t* fileSize, uint64_t* fileNbLine,
                           QDateTime* lastModified ) const
{
    const OpenFile* file = findOpenFileFromView( view );

    assert( file );

    // Folder documents have no single backing LogData.
    if ( file->logData == nullptr ) {
        *fileSize = 0;
        *fileNbLine = 0;
        *lastModified = {};
        return;
    }

    *fileSize = static_cast<uint64_t>( file->logData->getFileSize() );
    *fileNbLine = file->logData->getNbLine().get();
    *lastModified = file->logData->getLastModifiedDate();
}

OpenedDocumentInfo Session::openedDocumentInfo( const ViewInterface* view ) const
{
    return OpenedDocumentInfo{ getDocumentId( view ), getDisplayName( view ),
                               getAssociatedPath( view ).isEmpty() ? getDisplayName( view )
                                                                   : getAssociatedPath( view ),
                               getDocumentKind( view ) };
}

std::vector<OpenedDocumentInfo> Session::openedDocuments() const
{
    std::vector<OpenedDocumentInfo> documents;
    documents.reserve( openFiles_.size() );
    for ( const auto& [ view, openFile ] : openFiles_ ) {
        documents.emplace_back( openedDocumentInfo( view ) );
    }
    return documents;
}

ViewInterface* Session::openAlways( const QString& file_name,
                                    const std::function<ViewInterface*()>& view_factory,
                                    const QString& view_context )
{
    // Create the data objects
    auto log_data = std::make_shared<LogData>();
    auto log_filtered_data = std::shared_ptr<LogFilteredData>( log_data->getNewFilteredData() );

    ViewInterface* view = view_factory();
    view->setData( log_data, log_filtered_data );
    view->setQuickFindPattern( quickFindPattern_ );
    view->setSavedSearches( savedSearches_ );

    if ( !view_context.isEmpty() )
        view->setViewContext( view_context );

    // Insert in the hash
    openFiles_.insert( { view,
                         { file_name,
                           file_name,
                           klogg::displayNameForPath( file_name ),
                           file_name,
                           DocumentKind::File,
                           log_data,
                           log_filtered_data,
                           {},
                           view,
                           {},
                           {} } } );

    // Start loading the file
    log_data->attachFile( file_name );

    return view;
}

bool Session::isLiveCaptureIdOpen( const QString& captureId ) const
{
    return std::any_of(
        openFiles_.cbegin(), openFiles_.cend(), [ &captureId ]( const auto& entry ) {
            return entry.second.adbLogcatSource != nullptr
                   && entry.second.adbLogcatSource->sessionData().captureId == captureId;
        } );
}

ViewInterface* Session::openAdbAlways( const AdbLogcatSessionData& sessionData,
                                       const std::function<ViewInterface*()>& view_factory,
                                       bool startConnected, const QString& viewContext )
{
    auto restoredSessionData = sessionData;
    if ( !restoredSessionData.isValid() ) {
        LOG_WARNING << "Refusing live source with invalid capture id "
                    << restoredSessionData.captureId;
        return nullptr;
    }
    if ( isLiveCaptureIdOpen( restoredSessionData.captureId ) ) {
        LOG_WARNING << "Refusing duplicate live capture storage id "
                    << restoredSessionData.captureId;
        return nullptr;
    }

    std::shared_ptr<StreamingLogData> logData;
    try {
        logData = std::make_shared<StreamingLogData>( restoredSessionData.captureId );
    } catch ( const std::exception& error ) {
        LOG_WARNING << "Failed to initialize live capture " << restoredSessionData.captureId << ": "
                    << error.what();
        return nullptr;
    }

    CaptureStore::Limits captureLimits;
    captureLimits.rollingMaxFileSize = restoredSessionData.captureMaxFileSize;
    captureLimits.rollingBackupCount = restoredSessionData.captureBackupCount;
    logData->setCaptureLimits( captureLimits );

    if ( !restoredSessionData.boundOutputFile.isEmpty()
         && !logData->bindOutputFile( restoredSessionData.boundOutputFile,
                                      restoredSessionData.outputAnsiMode,
                                      OutputBindMode::Restore ) ) {
        LOG_WARNING << "Failed to restore ADB output file binding "
                    << restoredSessionData.boundOutputFile;
        restoredSessionData.boundOutputFile.clear();
    }
    auto logFilteredData = std::shared_ptr<LogFilteredData>( logData->getNewFilteredData() );
    auto adbSource = transportFactory_ != nullptr
                         ? std::make_shared<AdbLogcatSource>( restoredSessionData, logData,
                                                              *transportFactory_ )
                         : std::make_shared<AdbLogcatSource>( restoredSessionData, logData );

    auto liveSpec = klogg::livelog::sessionSpecFromSessionData( restoredSessionData );
    liveSpec.runIntent = startConnected ? klogg::livecapture::RunIntent::Running
                                        : klogg::livecapture::RunIntent::Stopped;
    auto liveEffects = std::make_shared<SessionLiveLogEffects>(
        adbSource, liveSpec, adbInfrastructure_, iosCatalog_ );
    auto liveController = std::make_shared<klogg::livelog::LiveLogController>(
        liveSpec, controllerConfigFor( liveSpec ), *liveEffects );
    liveEffects->attach( *liveController );

    ViewInterface* view = view_factory();
    view->setData( logData, logFilteredData );
    view->setQuickFindPattern( quickFindPattern_ );
    view->setSavedSearches( savedSearches_ );

    if ( !viewContext.isEmpty() ) {
        view->setViewContext( viewContext );
    }

    // Restore can re-enter application shutdown through view construction or
    // event delivery. Re-check the lifecycle gate at the last responsible
    // moment so a previously-computed Running intent cannot arm after exit.
    if ( startConnected && !exitRequested_ ) {
        liveController->armRunIntent();
        if ( liveController->snapshot().source.status
             == klogg::livecapture::SourceStatus::Failed ) {
            const auto& failure = liveController->snapshot().source.failure;
            const bool transportUnavailable
                = failure.has_value()
                  && ( failure->code == "live-transport-unavailable"
                       || failure->code == "live-transport-create-failed" );
            if ( transportUnavailable ) {
                // Session owns the factory-created view once openAdbAlways is called.
                // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
                delete view;
                return nullptr;
            }
        }
    }

    openFiles_.insert( { view,
                         { restoredSessionData.documentId(), restoredSessionData.documentId(),
                           restoredSessionData.displayName(), restoredSessionData.associatedPath(),
                           DocumentKind::AdbLogcat, logData, logFilteredData, adbSource, view,
                           liveEffects, liveController } } );

    return view;
}

Session::OpenFile* Session::findOpenFileFromView( const ViewInterface* view )
{
    assert( view );

    OpenFile* file = &( openFiles_.at( view ) );

    // OpenfileMap::at might throw out_of_range but since a view MUST always
    // be attached to a file, we don't handle it!

    return file;
}

const Session::OpenFile* Session::findOpenFileFromView( const ViewInterface* view ) const
{
    assert( view );

    const OpenFile* file = &( openFiles_.at( view ) );

    // OpenfileMap::at might throw out_of_range but since a view MUST always
    // be attached to a file, we don't handle it!

    return file;
}

std::vector<WindowSession> Session::windowSessions()
{
    const auto& session = SessionInfo::getSynced();
    const auto& sessionWindows = session.windows();

    std::vector<WindowSession> windows;
    for ( auto i = 0; i < sessionWindows.size(); ++i ) {
        windows.emplace_back( shared_from_this(), sessionWindows.at( i ), i );
    }

    return windows;
}

void WindowSession::save(
    const std::vector<std::tuple<const ViewInterface*, uint64_t,
                                 std::shared_ptr<const ViewContextInterface>>>& view_list,
    const QByteArray& geometry, int current_file_index )
{
    LOG_DEBUG << "Session::save";

    std::vector<SessionInfo::OpenFile> session_files;
    for ( const auto& view : view_list ) {
        const ViewInterface* view_object;
        uint64_t top_line;
        std::shared_ptr<const ViewContextInterface> view_context;

        std::tie( view_object, top_line, view_context ) = view;

        const Session::OpenFile* file = appSession_->findOpenFileFromView( view_object );
        assert( file );

        LOG_DEBUG << "Saving " << file->fileName.toLocal8Bit().data() << " in session.";

        // Discriminate the document kind via the free-form sourceType column
        // (no SessionInfo version bump: the v2 schema already stores it). Folders
        // are tagged "folder" so restore can re-enumerate + openFolder instead of
        // trying to index a directory as a plain file.
        QString sourceType;
        if ( file->kind == DocumentKind::AdbLogcat && file->adbLogcatSource ) {
            sourceType = file->adbLogcatSource->sessionData().persistedSourceType();
        }
        else if ( file->kind == DocumentKind::Folder ) {
            sourceType = QStringLiteral( "folder" );
        }

        // Live tabs persist through the versioned typed session spec (schema
        // stamped by serializeSpec). The migration marker rides on the runtime
        // session data, so every re-save of a migrated session keeps it
        // stamped instead of losing it to a retrieve-only write.
        QString sourceSpec;
        if ( file->adbLogcatSource ) {
            auto spec = file->liveLogController
                            ? file->liveLogController->spec()
                            : klogg::livelog::sessionSpecFromSessionData(
                                  file->adbLogcatSource->sessionData() );
            const auto& runtimeData = file->adbLogcatSource->sessionData();
            if ( file->liveLogController ) {
                spec.runIntent = file->liveLogController->snapshot().runIntent;
            }
            spec.boundOutputFile = runtimeData.boundOutputFile;
            spec.capture.preserveAnsiOnSave
                = runtimeData.outputAnsiMode == LiveLogSaveAnsiMode::Preserve;
            sourceSpec = klogg::livelog::serializeSpec( spec );
        }

        // Defensive null-guard: a future buggy view returning a null context must
        // not crash save (FolderCrawlerWidget::doGetViewContext never returns
        // null, but guard regardless).
        const QString viewContextStr = view_context ? view_context->toString() : QString{};

        session_files.emplace_back( file->documentId, top_line, viewContextStr, sourceType,
                                    file->displayName, sourceSpec );
    }

    auto& session = SessionInfo::getSynced();
    session.setOpenFiles( windowId_, session_files );
    session.setGeometry( windowId_, geometry );
    session.setCurrentFileIndex( windowId_, current_file_index );
    session.save();
}

OpenedDocumentsList WindowSession::restore( const std::function<ViewInterface*()>& view_factory,
                                            int* current_file_index )
{
    const auto& session = SessionInfo::getSynced();

    std::vector<SessionInfo::OpenFile> session_files = session.openFiles( windowId_ );
    LOG_DEBUG << "Session returned " << session_files.size();
    OpenedDocumentsList result;
    const auto persistedCurrentIndex = session.currentFileIndex( windowId_ );
    int mappedCurrentIndex = -1;

    // Structured refusals from this pass (e.g. sessions saved with raw
    // command-line options) are collected for the UI to surface after the
    // restore loop completes — never silently dropped. Non-error notices
    // (one-time migrations, compatibility read-only presentation) ride the
    // parallel notice channel.
    appSession_->restoreRejections_.clear();
    appSession_->restoreNotices_.clear();

    // One structured rejection per refused tab: names the document and carries
    // the actionable fatal message(s).
    auto refuseRestoredLiveSource = [ this ]( const SessionInfo::OpenFile& refusedFile,
                                              const QStringList& messages ) {
        LOG_WARNING << "Refusing saved live source " << refusedFile.fileName.toLocal8Bit().data()
                    << ": " << messages.join( QStringLiteral( "; " ) ).toStdString();
        appSession_->restoreRejections_.append(
            QStringLiteral( "%1: %2" )
                .arg( refusedFile.displayName.isEmpty() ? refusedFile.fileName
                                                        : refusedFile.displayName,
                      messages.join( QStringLiteral( "\n" ) ) ) );
    };

    for ( int fileIndex = 0; fileIndex < klogg::isize( session_files ); ++fileIndex ) {
        const auto& file = session_files.at( static_cast<std::size_t>( fileIndex ) );
        LOG_DEBUG << "Create view for " << file.fileName;
        ViewInterface* view = nullptr;
        if ( AdbLogcatSessionData::isPersistedSourceType( file.sourceType ) ) {
            const auto parsed = klogg::livelog::parsePersistedSpec( file.sourceSpec );
            if ( !parsed.spec.has_value() ) {
                refuseRestoredLiveSource( file, fatalMessagesOf( parsed.diagnostics ) );
                continue;
            }
            const auto& restoredSpec = parsed.spec.value();

            // Gate routing: restored tabs pass the same typed accept gate as
            // fresh composition before anything can arm. The transitional
            // compatibility backends are the one soft case — they stay
            // loadable read-only (never armed, never an error); every other
            // fatality refuses the tab with a structured rejection.
            bool compatibilityTransport = false;
            QStringList gateRejections;
            for ( const auto& diagnostic : klogg::livelog::validateForAccept( restoredSpec ) ) {
                if ( diagnostic.severity != klogg::livelog::Diagnostic::Severity::Fatal ) {
                    continue;
                }
                if ( diagnostic.code == QLatin1String( "transitional-backend-not-creatable" ) ) {
                    compatibilityTransport = true;
                    continue;
                }
                gateRejections << diagnostic.message;
            }

            if ( !gateRejections.isEmpty() ) {
                refuseRestoredLiveSource( file, gateRejections );
                continue;
            }
            // Info-level parse diagnostics and compatibility-transport
            // presentation surface as non-error notices, once per source document.
            for ( const auto& diagnostic : parsed.diagnostics ) {
                if ( diagnostic.severity == klogg::livelog::Diagnostic::Severity::Info ) {
                    appSession_->appendRestoreNoticeOncePerDocument( restoredSpec.documentId(),
                                                                     diagnostic.message );
                }
            }
            if ( compatibilityTransport ) {
                appSession_->appendRestoreNoticeOncePerDocument(
                    restoredSpec.documentId(),
                    klogg::livelog::messages::compatibilityTransportReadOnly() );
            }
            if ( appSession_->isLiveCaptureIdOpen( restoredSpec.captureId ) ) {
                refuseRestoredLiveSource(
                    file, { klogg::livelog::messages::captureIdentifierAlreadyInUse() } );
                continue;
            }

            // Arming decision comes from the gated mapping itself: Running
            // intents on accepted specs yield exactly one StartRequested.
            const auto armEvents = klogg::livelog::initialLiveStateEvents(
                restoredSpec, std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch() ) );

            const auto sessionData = klogg::livelog::sessionDataFromSpec( restoredSpec );
            if ( sessionData.isValid() ) {
                view = appSession_->openAdbAlways( sessionData, view_factory, !armEvents.empty(),
                                                   file.viewContext );
            }
            if ( view == nullptr && !armEvents.empty() ) {
                refuseRestoredLiveSource(
                    file, { klogg::livelog::messages::transportUnavailableOnRestore() } );
                continue;
            }
        }
        else if ( file.sourceType == QStringLiteral( "folder" ) ) {
            // Folder tab: re-enumerate the directory (keeps the session file
            // small and always reflects current contents) and open it via the
            // folder path. view_factory is ignored (openFolder builds its own
            // FolderCrawlerWidget). An empty/gone folder yields nullptr and is
            // skipped below.
            const auto filePaths = enumerateFolderFiles( file.fileName );
            if ( !filePaths.empty() ) {
                view = appSession_->openFolder(
                    file.fileName, std::vector<QString>( filePaths.begin(), filePaths.end() ) );
                if ( view != nullptr && !file.viewContext.isEmpty() ) {
                    view->setViewContext( file.viewContext );
                }
            }
            else {
                LOG_WARNING << "Folder has no readable files, skipping: "
                            << file.fileName.toLocal8Bit().data();
            }
        }
        else {
            view = appSession_->openAlways( file.fileName, view_factory, file.viewContext );
        }

        if ( !view ) {
            continue;
        }

        const auto info = appSession_->openedDocumentInfo( view );
        if ( fileIndex == persistedCurrentIndex ) {
            mappedCurrentIndex = klogg::isize( result );
        }
        result.emplace_back( info, view );
        openedDocuments_.emplace_back( info.documentId );
    }

    *current_file_index = mappedCurrentIndex >= 0 ? mappedCurrentIndex : klogg::isize( result ) - 1;

    return result;
}

WindowSession::WindowSession( std::shared_ptr<Session> appSession, const QString& id, size_t index )
    : appSession_{ std::move( appSession ) }
    , windowId_{ id }
    , windowIndex_{ index }
{
    LOG_INFO << "created session for " << id;
    auto sessionInfo = SessionInfo::getSynced();
    sessionInfo.add( id );
    sessionInfo.save();
}

void WindowSession::restoreGeometry( QByteArray* geometry ) const
{
    const auto& session = SessionInfo::getSynced();
    *geometry = session.geometry( windowId_ );
}

bool WindowSession::close()
{
    LOG_INFO << "close window session " << windowId_;

    if ( appSession_->exitRequested() ) {
        return true;
    }

    auto& session = SessionInfo::getSynced();
    auto isRemoved = session.remove( windowId_ );
    session.save();

    LOG_INFO << "session is removed " << isRemoved;

    return !isRemoved;
}
