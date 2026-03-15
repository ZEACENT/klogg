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

#include "adblogcatsource.h"
#include "log.h"

#include <algorithm>
#include <cassert>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>


#include "logdata.h"
#include "logfiltereddata.h"
#include "savedsearches.h"
#include "sessioninfo.h"
#include "streaminglogdata.h"
#include "viewinterface.h"

Session::Session()
{
    // Get the global search history (it remains the property
    // of the Persistent)
    savedSearches_ = &SavedSearches::getSynced();
    SessionInfo::getSynced();

    quickFindPattern_ = std::make_shared<QuickFindPattern>();
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
        shortNames << QFileInfo( fn ).fileName();
    }
    const QString mergedName = QString( "[Merged] %1" ).arg( shortNames.join( " + " ) );

    // Create temp file with concatenated content
    const QString tempFilePath = QDir( tempDir ).filePath(
        QString( "klogg_merged_%1.log" )
            .arg( QDateTime::currentMSecsSinceEpoch() ) );

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
            LOG_ERROR << "Failed to open source file for merge: "
                      << fileNames[ fi ].toStdString();
        }
    }
    tempFile.close();

    // Use the normal open flow with the temp file
    return openAlways( tempFilePath, view_factory, {} );
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

void Session::getFileInfo( const ViewInterface* view, uint64_t* fileSize, uint64_t* fileNbLine,
                           QDateTime* lastModified ) const
{
    const OpenFile* file = findOpenFileFromView( view );

    assert( file );

    *fileSize = static_cast<uint64_t>( file->logData->getFileSize() );
    *fileNbLine = file->logData->getNbLine().get();
    *lastModified = file->logData->getLastModifiedDate();
}

OpenedDocumentInfo Session::openedDocumentInfo( const ViewInterface* view ) const
{
    return OpenedDocumentInfo{ getDocumentId( view ),
                               getDisplayName( view ),
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
                           QFileInfo( file_name ).fileName(),
                           file_name,
                           DocumentKind::File,
                           log_data,
                           log_filtered_data,
                           {},
                           view } } );

    // Start loading the file
    log_data->attachFile( file_name );

    return view;
}

ViewInterface* Session::openAdbAlways( const AdbLogcatSessionData& sessionData,
                                       const std::function<ViewInterface*()>& view_factory,
                                       bool startConnected, const QString& viewContext )
{
    auto restoredSessionData = sessionData;
    auto logData = std::make_shared<StreamingLogData>( restoredSessionData.captureId );
    if ( !restoredSessionData.boundOutputFile.isEmpty()
         && !logData->bindOutputFile( restoredSessionData.boundOutputFile ) ) {
        LOG_WARNING << "Failed to restore ADB output file binding "
                    << restoredSessionData.boundOutputFile;
        restoredSessionData.boundOutputFile.clear();
    }
    auto logFilteredData = std::shared_ptr<LogFilteredData>( logData->getNewFilteredData() );
    auto adbSource = std::make_shared<AdbLogcatSource>( restoredSessionData, logData );

    ViewInterface* view = view_factory();
    view->setData( logData, logFilteredData );
    view->setQuickFindPattern( quickFindPattern_ );
    view->setSavedSearches( savedSearches_ );

    if ( !viewContext.isEmpty() ) {
        view->setViewContext( viewContext );
    }

    openFiles_.insert( { view,
                         { restoredSessionData.documentId(),
                           restoredSessionData.documentId(),
                           restoredSessionData.displayName(),
                           restoredSessionData.associatedPath(),
                           DocumentKind::AdbLogcat,
                           logData,
                           logFilteredData,
                           adbSource,
                           view } } );

    if ( startConnected ) {
        adbSource->connectSource();
    }

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
        session_files.emplace_back( file->documentId, top_line, view_context->toString(),
                                    file->kind == DocumentKind::AdbLogcat
                                        ? QStringLiteral( "adb_logcat" )
                                        : QString{},
                                    file->displayName,
                                    file->adbLogcatSource
                                        ? QString::fromUtf8( QJsonDocument(
                                                                 file->adbLogcatSource->sessionData()
                                                                     .toJson() )
                                                                 .toJson( QJsonDocument::Compact ) )
                                        : QString{} );
    }

    auto& session = SessionInfo::getSynced();
    session.setOpenFiles( windowId_, session_files );
    session.setGeometry( windowId_, geometry );
    session.setCurrentFileIndex( windowId_, current_file_index );
    session.save();
}

OpenedDocumentsList
WindowSession::restore( const std::function<ViewInterface*()>& view_factory,
                        int* current_file_index )
{
    const auto& session = SessionInfo::getSynced();

    std::vector<SessionInfo::OpenFile> session_files = session.openFiles( windowId_ );
    LOG_DEBUG << "Session returned " << session_files.size();
    OpenedDocumentsList result;

    for ( const auto& file : session_files ) {
        LOG_DEBUG << "Create view for " << file.fileName;
        ViewInterface* view = nullptr;
        if ( file.sourceType == QStringLiteral( "adb_logcat" ) ) {
            view = appSession_->openAdbAlways( AdbLogcatSessionData::fromJson( file.sourceSpec ),
                                               view_factory, false, file.viewContext );
        }
        else {
            view = appSession_->openAlways( file.fileName, view_factory, file.viewContext );
        }

        const auto info = appSession_->openedDocumentInfo( view );
        result.emplace_back( info, view );
        openedDocuments_.emplace_back( info.documentId );
    }

    const auto restoredCurrentIndex = session.currentFileIndex( windowId_ );
    if ( restoredCurrentIndex >= 0 && restoredCurrentIndex < klogg::isize( result ) ) {
        *current_file_index = restoredCurrentIndex;
    }
    else {
        *current_file_index = klogg::isize( result ) - 1;
    }

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
