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

#ifndef SESSION_H
#define SESSION_H

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QDateTime>

#include "log.h"
#include "quickfindpattern.h"

class ViewInterface;
class ViewContextInterface;
class SearchableLogData;
class LogFilteredData;
class SavedSearches;
class AdbLogcatSource;
struct AdbLogcatSessionData;

enum class DocumentKind {
    File,
    AdbLogcat,
};

struct OpenedDocumentInfo {
    QString documentId;
    QString displayName;
    QString toolTip;
    DocumentKind kind = DocumentKind::File;
};

// File unreadable error
class FileUnreadableErr {
};

// The session is responsible for maintaining the list of open log files
// and their association with Views.
// It also maintains the domain objects which are common to all log files
// (SavedSearches, FileHistory, QFPattern...)

class WindowSession;

class Session : public std::enable_shared_from_this<Session> {
  public:
    Session();
    ~Session();

    // No copy/assignment please
    Session( const Session& ) = delete;
    Session& operator=( const Session& ) = delete;

    // Return the view associated to a file if it is open
    // The filename must be strictly identical to trigger a match
    // (no match in case of e.g. relative vs. absolute pathname.
    ViewInterface* getViewIfOpen( const QString& file_name ) const;

    // Open a new file, starts its asynchronous loading, and construct a new
    // view for it (the caller passes a factory to build the concrete view)
    // The ownership of the view is given to the caller
    // Throw exceptions if the file is already open or if it cannot be open.
    ViewInterface* open( const QString& file_name,
                         const std::function<ViewInterface*()>& view_factory );

    // Close the file identified by the view passed
    // Throw an exception if it does not exist.
    void close( const ViewInterface* view );

    // Open a merged view of multiple files, writing concatenated content to a temp file.
    // Returns the new view, or nullptr on failure.
    ViewInterface* openMerged( const std::vector<QString>& fileNames,
                               const std::function<ViewInterface*()>& view_factory,
                               const QString& tempDir );
    ViewInterface* openAdbLogcat( const AdbLogcatSessionData& sessionData,
                                  const std::function<ViewInterface*()>& view_factory,
                                  bool startConnected, const QString& viewContext = {} );

    // Get the file name for the passed view.
    QString getFilename( const ViewInterface* view ) const;
    QString getDocumentId( const ViewInterface* view ) const;
    QString getDisplayName( const ViewInterface* view ) const;
    QString getAssociatedPath( const ViewInterface* view ) const;
    DocumentKind getDocumentKind( const ViewInterface* view ) const;
    AdbLogcatSource* getAdbLogcatSource( const ViewInterface* view ) const;

    // Get the size (in bytes) and number of lines in the current file.
    // The file is identified by the view attached to it.
    void getFileInfo( const ViewInterface* view, uint64_t* fileSize, uint64_t* fileNbLine,
                      QDateTime* lastModified ) const;

    // Get a (non-const) reference to the QuickFind pattern.
    std::shared_ptr<QuickFindPattern> quickFindPattern() const
    {
        return quickFindPattern_;
    }

    SavedSearches& savedSearches() const
    {
        return *savedSearches_;
    }

    OpenedDocumentInfo openedDocumentInfo( const ViewInterface* view ) const;
    std::vector<OpenedDocumentInfo> openedDocuments() const;

    std::vector<WindowSession> windowSessions();

    bool exitRequested() const
    {
        return exitRequested_;
    }

    void setExitRequested( bool isRequested )
    {
        exitRequested_ = isRequested;
    }

  private:
    struct OpenFile {
        QString fileName;
        QString documentId;
        QString displayName;
        QString associatedPath;
        DocumentKind kind = DocumentKind::File;
        std::shared_ptr<SearchableLogData> logData;
        std::shared_ptr<LogFilteredData> logFilteredData;
        std::shared_ptr<AdbLogcatSource> adbLogcatSource;
        ViewInterface* view;
    };

    // Open a file without checking if it is existing/readable
    ViewInterface* openAlways( const QString& file_name,
                               const std::function<ViewInterface*()>& view_factory,
                               const QString& view_context );
    ViewInterface* openAdbAlways( const AdbLogcatSessionData& sessionData,
                                  const std::function<ViewInterface*()>& view_factory,
                                  bool startConnected, const QString& viewContext );

    // Find an open file from its associated view
    OpenFile* findOpenFileFromView( const ViewInterface* view );
    const OpenFile* findOpenFileFromView( const ViewInterface* view ) const;

    // List of open files
    typedef std::unordered_map<const ViewInterface*, OpenFile> OpenFileMap;
    OpenFileMap openFiles_;

    // Global search history
    SavedSearches* savedSearches_;

    // Global quickfind pattern
    std::shared_ptr<QuickFindPattern> quickFindPattern_;

    bool exitRequested_ = false;

    friend class WindowSession;
};

using OpenedFilesList = std::vector<std::pair<QString, ViewInterface*>>;
using OpenedDocumentsList = std::vector<std::pair<OpenedDocumentInfo, ViewInterface*>>;
using SaveFileInfo
    = std::tuple<const ViewInterface*, uint64_t, std::shared_ptr<const ViewContextInterface>>;

class WindowSession {
  public:
    WindowSession( std::shared_ptr<Session> appSession, const QString& id, size_t index );

    ViewInterface* getViewIfOpen( const QString& file_name ) const
    {
        return appSession_->getViewIfOpen( file_name );
    }

    ViewInterface* open( const QString& file_name,
                         const std::function<ViewInterface*()>& view_factory )
    {
        openedDocuments_.push_back( file_name );
        return appSession_->open( file_name, view_factory );
    }

    void close( const ViewInterface* view )
    {
        auto it = std::find( openedDocuments_.begin(), openedDocuments_.end(), getDocumentId( view ) );
        if ( it != openedDocuments_.end() ) {
            openedDocuments_.erase( it );
        }

        appSession_->close( view );
    }

    ViewInterface* openMerged( const std::vector<QString>& fileNames,
                               const std::function<ViewInterface*()>& view_factory,
                               const QString& tempDir )
    {
        auto* view = appSession_->openMerged( fileNames, view_factory, tempDir );
        if ( view ) {
            openedDocuments_.push_back( appSession_->getDocumentId( view ) );
        }
        return view;
    }

    ViewInterface* openAdbLogcat( const AdbLogcatSessionData& sessionData,
                                  const std::function<ViewInterface*()>& view_factory,
                                  bool startConnected )
    {
        auto* view = appSession_->openAdbLogcat( sessionData, view_factory, startConnected );
        if ( view ) {
            openedDocuments_.push_back( appSession_->getDocumentId( view ) );
        }
        return view;
    }

    QString getFilename( const ViewInterface* view ) const
    {
        return appSession_->getFilename( view );
    }

    QString getDocumentId( const ViewInterface* view ) const
    {
        return appSession_->getDocumentId( view );
    }

    QString getDisplayName( const ViewInterface* view ) const
    {
        return appSession_->getDisplayName( view );
    }

    QString getAssociatedPath( const ViewInterface* view ) const
    {
        return appSession_->getAssociatedPath( view );
    }

    DocumentKind getDocumentKind( const ViewInterface* view ) const
    {
        return appSession_->getDocumentKind( view );
    }

    AdbLogcatSource* getAdbLogcatSource( const ViewInterface* view ) const
    {
        return appSession_->getAdbLogcatSource( view );
    }

    void getFileInfo( const ViewInterface* view, uint64_t* fileSize, uint64_t* fileNbLine,
                      QDateTime* lastModified ) const
    {
        return appSession_->getFileInfo( view, fileSize, fileNbLine, lastModified );
    }

    std::vector<OpenedDocumentInfo> openedDocuments() const
    {
        std::vector<OpenedDocumentInfo> documents;
        documents.reserve( openedDocuments_.size() );
        for ( const auto& documentId : openedDocuments_ ) {
            const auto it = std::find_if(
                appSession_->openFiles_.cbegin(), appSession_->openFiles_.cend(),
                [ &documentId ]( const auto& openFile ) {
                    return openFile.second.documentId == documentId;
                } );
            if ( it != appSession_->openFiles_.cend() ) {
                documents.push_back( appSession_->openedDocumentInfo( it->first ) );
            }
        }
        return documents;
    }

    // Get a (non-const) reference to the QuickFind pattern.
    std::shared_ptr<QuickFindPattern> getQuickFindPattern() const
    {
        return appSession_->quickFindPattern();
    }

    QString windowId() const
    {
        return windowId_;
    }

    size_t windowIndex() const
    {
        return windowIndex_;
    }

    bool exitRequested() const
    {
        return appSession_->exitRequested();
    }

    // Open all the files listed in the stored session
    // (see ::open)
    // returns a vector of pairs (file_name, view) and the index of the
    // current file (or -1 if none).
    OpenedDocumentsList restore( const std::function<ViewInterface*()>& view_factory,
                                 int* current_file_index );

    // Get the geometry string from persistent storage for this session.
    void restoreGeometry( QByteArray* geometry ) const;

    // Save the session to persistent storage. An ordered list of
    // (view, topline, ViewContextInterface) is passed, this is because only
    // the main window know the order in which the views are presented to
    // the user (it might have changed since file were opened).
    // Also, the geometry information is passed as an opaque string.
    void save( const std::vector<SaveFileInfo>& view_list, const QByteArray& geometry,
               int current_file_index );

    // returns true if caller needs to save settings
    bool close();

  private:
    std::shared_ptr<Session> appSession_;
    QString windowId_;
    size_t windowIndex_;

    std::vector<QString> openedDocuments_;
};

#endif
