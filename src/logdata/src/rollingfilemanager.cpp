#include "rollingfilemanager.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "log.h"

namespace {
// Sanity ceiling for backupCount. The GUI spinbox tops out at 999, but the
// value also arrives from session-restore JSON / Configuration without range
// validation; clamping here keeps every `backupCount_ + N` expression safely
// within int range (and the rolling-window byte product within qint64).
constexpr int kMaxBackupCount = 100000;
} // namespace

RollingFileManager::RollingFileManager( QString basePath, qint64 maxFileSize, int backupCount )
    : basePath_( std::move( basePath ) )
    , maxFileSize_( maxFileSize )
    , backupCount_( std::clamp( backupCount, 0, kMaxBackupCount ) )
{
}

RollingFileManager::RollingFileManager( RollingFileManager&& other ) noexcept
    : basePath_( std::move( other.basePath_ ) )
    , maxFileSize_( other.maxFileSize_ )
    , backupCount_( other.backupCount_ )
    , currentBytes_( other.currentBytes_ )
    , openedNewFile_( other.openedNewFile_ )
{
    // Move the file handle by closing the old one and reopening in the new instance
    if ( other.currentFile_.isOpen() ) {
        other.currentFile_.close();
        currentFile_.setFileName( basePath_ );
        if ( currentFile_.open( QIODevice::WriteOnly | QIODevice::Append ) ) {
            currentBytes_ = currentFile_.size();
        }
        else {
            LOG_WARNING << "RollingFileManager: reopen failed during move: " << basePath_;
            currentBytes_ = 0;
        }
    }
}

RollingFileManager& RollingFileManager::operator=( RollingFileManager&& other ) noexcept
{
    if ( this != &other ) {
        close();
        basePath_ = std::move( other.basePath_ );
        maxFileSize_ = other.maxFileSize_;
        backupCount_ = other.backupCount_;
        currentBytes_ = other.currentBytes_;
        openedNewFile_ = other.openedNewFile_;
        if ( other.currentFile_.isOpen() ) {
            other.currentFile_.close();
            currentFile_.setFileName( basePath_ );
            if ( currentFile_.open( QIODevice::WriteOnly | QIODevice::Append ) ) {
                currentBytes_ = currentFile_.size();
            }
            else {
                LOG_WARNING << "RollingFileManager: reopen failed during move assign: "
                            << basePath_;
                currentBytes_ = 0;
            }
        }
    }
    return *this;
}

bool RollingFileManager::isValid() const
{
    // Valid if we have a path. maxFileSize=0 means no rolling (single unlimited file).
    return !basePath_.isEmpty();
}

bool RollingFileManager::open( bool truncate )
{
    if ( !isValid() ) {
        return false;
    }

    QDir().mkpath( QFileInfo( basePath_ ).absolutePath() );
    return openNewFile( truncate );
}

bool RollingFileManager::openExisting()
{
    if ( !isValid() ) {
        return false;
    }
    currentFile_.setFileName( basePath_ );
    if ( !currentFile_.open( QIODevice::WriteOnly | QIODevice::ExistingOnly
                             | QIODevice::Append ) ) {
        return false;
    }
    currentBytes_ = currentFile_.size();
    openedNewFile_ = false;
    return true;
}

void RollingFileManager::close()
{
    if ( currentFile_.isOpen() ) {
        currentFile_.flush();
        currentFile_.close();
    }
    currentBytes_ = 0;
}

bool RollingFileManager::flush()
{
    if ( currentFile_.isOpen() ) {
        return currentFile_.flush();
    }
    return false;
}

bool RollingFileManager::openedNewFile() const
{
    return openedNewFile_;
}

bool RollingFileManager::refersToPath( const QString& path ) const
{
    if ( !currentFile_.isOpen() || path.isEmpty() ) {
        return false;
    }
#ifdef Q_OS_WIN
    const auto nativeHandle = _get_osfhandle( static_cast<int>( currentFile_.handle() ) );
    if ( nativeHandle == -1 ) {
        return false;
    }
    const auto currentHandle = reinterpret_cast<HANDLE>( nativeHandle );
    if ( currentHandle == INVALID_HANDLE_VALUE ) {
        return false;
    }
    const auto pathHandle
        = CreateFileW( reinterpret_cast<LPCWSTR>( path.utf16() ), FILE_READ_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr );
    if ( pathHandle == INVALID_HANDLE_VALUE ) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION currentInfo{};
    BY_HANDLE_FILE_INFORMATION pathInfo{};
    const auto currentInfoRead = GetFileInformationByHandle( currentHandle, &currentInfo );
    const auto pathInfoRead = GetFileInformationByHandle( pathHandle, &pathInfo );
    CloseHandle( pathHandle );
    return currentInfoRead && pathInfoRead
           && currentInfo.dwVolumeSerialNumber == pathInfo.dwVolumeSerialNumber
           && currentInfo.nFileIndexHigh == pathInfo.nFileIndexHigh
           && currentInfo.nFileIndexLow == pathInfo.nFileIndexLow;
#else
    struct stat currentInfo{};
    struct stat pathInfo{};
    const auto encodedPath = QFile::encodeName( path );
    return ::fstat( currentFile_.handle(), &currentInfo ) == 0
           && ::stat( encodedPath.constData(), &pathInfo ) == 0
           && currentInfo.st_dev == pathInfo.st_dev && currentInfo.st_ino == pathInfo.st_ino;
#endif
}

bool RollingFileManager::removeCurrentFile()
{
    if ( !refersToPath( basePath_ ) ) {
        return false;
    }
    close();
    return QFile::remove( basePath_ );
}

qint64 RollingFileManager::write( const QByteArray& data )
{
    if ( data.isEmpty() || !currentFile_.isOpen() ) {
        return 0;
    }

    rotated_ = false;

    qint64 totalWritten = 0;
    int offset = 0;
    const int size = static_cast<int>( data.size() );

    while ( offset < size ) {
        // Auto-rotate if the current file is full from previous writes.
        if ( needsRotation() ) {
            if ( !rotateInternal() ) {
                break; // unable to rotate; preserve what was already written
            }
        }

        // maxFileSize_ = 0 means no rolling: write everything left in one go.
        if ( maxFileSize_ <= 0 ) {
            const auto written = currentFile_.write( data.constData() + offset, size - offset );
            if ( written > 0 ) {
                currentBytes_ += written;
                totalWritten += written;
            }
            break;
        }

        const qint64 remainingCapacity = maxFileSize_ - currentBytes_;
        const qint64 chunkLeft = static_cast<qint64>( size ) - offset;

        // The entire remainder fits in the current file.
        if ( chunkLeft <= remainingCapacity ) {
            const auto written = currentFile_.write( data.constData() + offset,
                                                     static_cast<qint64>( chunkLeft ) );
            if ( written > 0 ) {
                currentBytes_ += written;
                totalWritten += written;
            }
            if ( needsRotation() ) {
                rotateInternal();
            }
            break;
        }

        // Find the last newline within the capacity window so only complete
        // lines are written to this file (a line is never split across files).
        const auto searchFrom = static_cast<int>(
            std::min<qint64>( offset + remainingCapacity - 1, size - 1 ) );
        const auto lastNewline = data.lastIndexOf( '\n', searchFrom );

        if ( lastNewline >= offset ) {
            const auto bytesToWrite = lastNewline - offset + 1;
            const auto written = currentFile_.write( data.constData() + offset, bytesToWrite );
            if ( written <= 0 ) {
                break;
            }
            currentBytes_ += written;
            totalWritten += written;
            offset += static_cast<int>( written );
            // Loop: the file is now (near) full, so the next iteration rotates
            // and continues writing the remaining complete lines.
        }
        else {
            // No complete line fits in the remaining capacity: the next line is
            // longer than the space left. Rotate to a fresh file and write that
            // one (possibly oversized) line whole so nothing is dropped.
            if ( !rotateInternal() ) {
                break;
            }
            const auto nextNewline = data.indexOf( '\n', offset );
            const int lineEnd = ( nextNewline >= 0 ) ? static_cast<int>( nextNewline ) + 1 : size;
            const auto written = currentFile_.write( data.constData() + offset,
                                                     lineEnd - offset );
            if ( written <= 0 ) {
                break;
            }
            currentBytes_ += written;
            totalWritten += written;
            offset = lineEnd;
        }
    }

    return totalWritten;
}

void RollingFileManager::rotate()
{
    rotateInternal();
}

bool RollingFileManager::needsRotation() const
{
    // maxFileSize_ = 0 means no rolling (single unlimited file)
    return maxFileSize_ > 0 && currentBytes_ >= maxFileSize_;
}

bool RollingFileManager::rotated() const
{
    return rotated_;
}

qint64 RollingFileManager::currentFileSize() const
{
    return currentBytes_;
}

qint64 RollingFileManager::maxFileSize() const
{
    return maxFileSize_;
}

int RollingFileManager::backupCount() const
{
    return backupCount_;
}

QFile* RollingFileManager::currentFile()
{
    return currentFile_.isOpen() ? &currentFile_ : nullptr;
}

QStringList RollingFileManager::backupFiles() const
{
    QStringList files;
    // When backupCount_ = 0 (keep all), search a reasonable upper bound;
    // otherwise search up to backupCount_ + 1 to catch any excess.
    const int searchLimit = ( backupCount_ > 0 ) ? backupCount_ + 1 : 100;
    for ( int i = 0; i <= searchLimit; ++i ) {
        const auto path = backupPath( i );
        if ( QFile::exists( path ) ) {
            files.append( path );
        }
        else if ( i > 0 ) {
            // Stop at the first gap — backups are numbered sequentially
            break;
        }
    }
    return files;
}

void RollingFileManager::resyncSize()
{
    if ( currentFile_.isOpen() ) {
        currentBytes_ = currentFile_.size();
    }
}

void RollingFileManager::deleteAll()
{
    close();
    QFile::remove( basePath_ );
    const int searchLimit = ( backupCount_ > 0 ) ? backupCount_ + 10 : 100;
    for ( int i = 0; i <= searchLimit; ++i ) {
        const auto path = backupPath( i );
        if ( QFile::exists( path ) ) {
            QFile::remove( path );
        }
        else if ( i > 0 ) {
            break;
        }
    }
    QFile::remove( basePath_ + QStringLiteral( ".tmp_rotate" ) );
}

QString RollingFileManager::backupPath( int index ) const
{
    return basePath_ + QStringLiteral( ".%1" ).arg( index );
}

void RollingFileManager::cleanupOldBackups()
{
    // backupCount_ is clamped to kMaxBackupCount at construction, so this upper
    // bound can never overflow int (the previous `backupCount_ + 100` was UB
    // when backupCount_ was within 100 of INT_MAX and skipped cleanup entirely).
    const int scanLimit = backupCount_ + 100;
    for ( int i = backupCount_; i < scanLimit; ++i ) {
        const auto path = backupPath( i );
        if ( QFile::exists( path ) ) {
            QFile::remove( path );
        }
        else {
            break;
        }
    }
}

bool RollingFileManager::openNewFile( bool truncate )
{
    currentFile_.setFileName( basePath_ );

    // A truncating open always starts a fresh file, regardless of whether the
    // path already exists (FreshSave semantics).
    if ( truncate ) {
        if ( !currentFile_.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
            LOG_WARNING << "RollingFileManager: failed to open " << basePath_;
            return false;
        }
        currentBytes_ = 0;
        openedNewFile_ = true;
        return true;
    }

    // Append path: decide "is this a brand-new file" atomically with the open.
    // NewOnly (O_EXCL) succeeds only when the path did not exist, so the result
    // cannot race a pre-open QFileInfo::exists() probe. On success the file was
    // created by this open.
    if ( currentFile_.open( QIODevice::WriteOnly | QIODevice::NewOnly ) ) {
        currentBytes_ = 0;
        openedNewFile_ = true;
        return true;
    }

    // The path pre-existed (NewOnly failed with EEXIST). Append to it without
    // creation (ExistingOnly): a plain Append would silently recreate the file
    // if another process deleted it in the window since NewOnly failed, leaving
    // openedNewFile_ = false for a file this open just created. Restore-mode
    // callers gate capture replay on that flag, so the new file would be left
    // empty and the buffered content lost.
    if ( currentFile_.open( QIODevice::WriteOnly | QIODevice::ExistingOnly | QIODevice::Append ) ) {
        currentBytes_ = currentFile_.size();
        openedNewFile_ = false;
        return true;
    }

    // The ExistingOnly append failed, most likely because the path disappeared
    // after NewOnly's EEXIST (deleted by another process). Recreate it
    // atomically; the result now correctly reports a brand-new file. A second
    // NewOnly failure means the path reappeared in the meantime (or a genuine
    // open error) — surface it.
    if ( currentFile_.open( QIODevice::WriteOnly | QIODevice::NewOnly ) ) {
        currentBytes_ = 0;
        openedNewFile_ = true;
        return true;
    }

    LOG_WARNING << "RollingFileManager: failed to open " << basePath_;
    return false;
}

bool RollingFileManager::rotateInternal()
{
    if ( !currentFile_.isOpen() ) {
        return openNewFile();
    }

    rotated_ = true;

    // 1. Flush pending data
    currentFile_.flush();

    // 2. Open temp file BEFORE closing old one (no data loss gap)
    const auto tmpPath = basePath_ + QStringLiteral( ".tmp_rotate" );
    QFile tmpFile( tmpPath );
    if ( !tmpFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        LOG_WARNING << "RollingFileManager: failed to open temp file: " << tmpPath;
        return false;
    }
    tmpFile.close();

    // 3. Close old current file
    currentFile_.close();

    // 4. Shift backups: backup[i-1] → backup[i] (oldest first to avoid overwrite).
    //    This frees up backup[0] for the old current file.
    //    When backupCount_ = 0 (keep all), shift all existing backups up.
    int shiftUpper;
    if ( backupCount_ > 0 ) {
        shiftUpper = backupCount_ - 1;
    }
    else {
        // Find the highest existing backup index
        shiftUpper = 0;
        for ( int probe = 0; probe < 1000; ++probe ) {
            if ( QFile::exists( backupPath( probe ) ) ) {
                shiftUpper = probe;
            }
            else if ( probe > 0 ) {
                break;
            }
        }
    }
    for ( int i = shiftUpper; i >= 0; --i ) {
        const auto src = backupPath( i );
        const auto dst = backupPath( i + 1 );
        QFile::remove( dst );
        if ( QFile::exists( src ) ) {
            QFile::rename( src, dst );
        }
    }

    // 5. Rename old current → backup[0]
    const auto backup0 = backupPath( 0 );
    QFile::remove( backup0 );
    if ( !QFile::rename( basePath_, backup0 ) ) {
        LOG_WARNING << "RollingFileManager: rename failed: " << basePath_ << " → " << backup0;
        // Restore: reopen old file as current
        currentFile_.setFileName( basePath_ );
        (void) currentFile_.open( QIODevice::WriteOnly | QIODevice::Append );
        currentBytes_ = currentFile_.size();
        QFile::remove( tmpPath );
        return false;
    }

    // 6. Rename temp → new current
    if ( !QFile::rename( tmpPath, basePath_ ) ) {
        LOG_WARNING << "RollingFileManager: rename failed: " << tmpPath << " → " << basePath_;
        // Restore
        QFile::rename( backup0, basePath_ );
        currentFile_.setFileName( basePath_ );
        (void) currentFile_.open( QIODevice::WriteOnly | QIODevice::Append );
        currentBytes_ = currentFile_.size();
        return false;
    }

    // 7. Cleanup excess backups (skip when backupCount_ = 0: keep all rotated files)
    if ( backupCount_ > 0 ) {
        cleanupOldBackups();
    }

    // 8. Open new current file
    return openNewFile();
}
