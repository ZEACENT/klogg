#include "fileholder.h"

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

#include "log.h"
#include "platform/platform_files.h"
#include <QtCore/QFileInfo>

namespace {
void openFileByHandle( QFile* file )
{
    if ( klogg::platform::openFileSharedForReplacement(
             *file, QIODevice::ReadOnly ) ) {
        LOG_INFO << "QFile opened";
        return;
    }

    LOG_WARNING << "Failed to open file " << file->fileName() << " error "
                << file->errorString();
}
} // namespace

FileHolder::FileHolder( bool keepClosed )
    : keep_closed_{ keepClosed }
{
    LOG_INFO << "created file holder " << reinterpret_cast<void*>(this);
}

FileHolder::~FileHolder()
{
    LOG_INFO << "destroy file holder "  << reinterpret_cast<void*>(this) << " for " << file_name_;

    // FileWatcher deregistration is owned by LogData (~LogData, guarded by its
    // fileWatcherRegistered_ flag) and must NOT be repeated here: this dtor
    // used to call removeFile whenever an inner QFile existed, which both
    // double-removed successfully watched files (the second removeFile hits
    // EfswFileWatcher's "The file is not watched" warning) and removed files
    // that were never registered at all (interrupted/failed indexing reaches
    // here with an open handle but no addFile). LogData is the only owner of
    // FileHolder and the only component that knows whether addFile happened.
}

FileId FileHolder::getFileId()
{
    ScopedRecursiveLock locker( file_mutex_ );
    return attached_file_id_;
}

qint64 FileHolder::size()
{
    ScopedRecursiveLock locker( file_mutex_ );
    return attached_file_ ? attached_file_->size() : 0;
}

bool FileHolder::isOpen()
{
    ScopedRecursiveLock locker( file_mutex_ );
    return attached_file_ ? attached_file_->openMode() != QIODevice::NotOpen : false;
}

void FileHolder::open( const QString& fileName )
{
    ScopedRecursiveLock locker( file_mutex_ );
    file_name_ = fileName;

    LOG_INFO << "open file " << file_name_ << " keep closed " << keep_closed_;

    if ( !keep_closed_ ) {
        counter_ = 1;
        reOpenFile();
    }
}

void FileHolder::lock()
{
    file_mutex_.lock();
}

void FileHolder::unlock()
{
    file_mutex_.unlock();
}

void FileHolder::attachReader()
{
    ScopedRecursiveLock locker( file_mutex_ );

    if ( keep_closed_ && counter_ == 0 ) {
        LOG_INFO << "fist reader opened for " << file_name_;
        reOpenFile();
    }

    counter_++;

    LOG_DEBUG << "has " << counter_ << " readers for " << file_name_;
}

void FileHolder::detachReader()
{
    ScopedRecursiveLock locker( file_mutex_ );
    if ( counter_ > 0 ) {
        counter_--;
    }

    if ( keep_closed_ && counter_ == 0 ) {
        attached_file_->close();
        LOG_INFO << "last reader closed for " << file_name_;
    }
}

void FileHolder::reOpenFile()
{
    LOG_DEBUG << "reopen " << file_name_;

    auto reopened = std::make_unique<QFile>( file_name_ );
    if ( QFileInfo( file_name_ ).isReadable() ) {
        openFileByHandle( reopened.get() );
    }

    ScopedRecursiveLock locker( file_mutex_ );
    attached_file_ = std::move( reopened );
    attached_file_id_ = FileId::getFileId( file_name_ );
}

QFile* FileHolder::getFile()
{
    return attached_file_.get();
}

FileId FileId::getFileId( const QString& filename )
{
#ifdef Q_OS_WIN
    DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    int accessRights = GENERIC_READ;
    DWORD creationDisp = OPEN_EXISTING;

    // Create the file handle.
    SECURITY_ATTRIBUTES securityAtts = { sizeof( SECURITY_ATTRIBUTES ), NULL, FALSE };

    HANDLE fileHandle
        = CreateFileW( (const wchar_t*)filename.utf16(), accessRights, shareMode, &securityAtts,
                       creationDisp, FILE_FLAG_BACKUP_SEMANTICS, NULL );

    if ( fileHandle == INVALID_HANDLE_VALUE ) {
        LOG_DEBUG << "Failed to get file info for " << filename.toStdString() << ", gle "
                  << ::GetLastError();
        return FileId{};
    }

    using FileHandleGuard = std::unique_ptr<void, decltype( &CloseHandle )>;
    auto fileHandleGuard = FileHandleGuard{ fileHandle, CloseHandle };

    BY_HANDLE_FILE_INFORMATION info;
    if ( !::GetFileInformationByHandle( fileHandle, &info ) ) {
        LOG_DEBUG << "Failed to get file info for " << filename.toStdString() << ", gle "
                  << ::GetLastError();
        return FileId{};
    }

    ULARGE_INTEGER fileIndex = { info.nFileIndexLow, info.nFileIndexHigh };
    return FileId{ fileIndex.QuadPart, static_cast<uint64_t>( info.dwVolumeSerialNumber ) };
#else
    struct stat info;
    if ( lstat( filename.toUtf8().constData(), &info ) != 0 ) {
        LOG_DEBUG << "Failed to get file info for " << filename.toStdString();
        return FileId{};
    }

    return FileId{ info.st_ino, static_cast<uint64_t>( info.st_dev ) };
#endif
}
