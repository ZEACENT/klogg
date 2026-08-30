#include "rollingfilemanager.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <vector>

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

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

struct BackupEntry {
    qint64 index;
    QString path;
};

std::vector<BackupEntry> numericBackupEntries( const QString& basePath )
{
    const QFileInfo baseInfo( basePath );
    const auto prefix = baseInfo.fileName() + QLatin1Char( '.' );
    const QDir directory = baseInfo.absoluteDir();
    const auto names = directory.entryList(
        QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::NoSort );

    std::vector<BackupEntry> entries;
    for ( const auto& name : names ) {
        if ( !name.startsWith( prefix ) ) {
            continue;
        }
        const auto suffix = name.mid( prefix.size() );
        bool parsed = false;
        const auto index = suffix.toLongLong( &parsed );
        if ( !parsed || index < 0 || index == std::numeric_limits<qint64>::max()
             || suffix != QString::number( index ) ) {
            continue;
        }
        entries.push_back( BackupEntry{ index, directory.filePath( name ) } );
    }
    std::sort( entries.begin(), entries.end(), []( const BackupEntry& left,
                                                   const BackupEntry& right ) {
        return left.index < right.index;
    } );
    return entries;
}

std::vector<BackupEntry> backupEntries( const QString& basePath )
{
    auto entries = numericBackupEntries( basePath );

    // Without a keep-all marker, manager-owned backups must form a canonical
    // sequence from .0. Stop at the first gap so unrelated numeric siblings
    // (for example a dated export) are never treated as owned output history.
    const auto firstGap = std::find_if(
        entries.cbegin(), entries.cend(), [ expected = qint64{ 0 } ]( const BackupEntry& entry ) mutable {
            return entry.index != expected++;
        } );
    entries.erase( firstGap, entries.cend() );
    return entries;
}

QString keepAllManifestPath( const QString& basePath )
{
    return basePath + QStringLiteral( ".klogg-rolling-backups" );
}

QString keepAllPendingPath( const QString& basePath )
{
    return basePath + QStringLiteral( ".klogg-rolling-pending" );
}

std::optional<qint64> readKeepAllPendingIndex( const QString& basePath )
{
    QFile pending( keepAllPendingPath( basePath ) );
    if ( !pending.open( QIODevice::ReadOnly ) ) {
        return std::nullopt;
    }
    const auto record = pending.readAll();
    if ( record.isEmpty() || !record.endsWith( '\n' ) ) {
        return std::nullopt;
    }
    const auto line = record.left( record.size() - 1 );
    bool parsed = false;
    const auto index = QString::fromLatin1( line ).toLongLong( &parsed );
    return parsed && index >= 0 && index != std::numeric_limits<qint64>::max()
                   && line == QByteArray::number( index )
               ? std::optional<qint64>{ index }
               : std::nullopt;
}

bool writeKeepAllPendingIndex( const QString& basePath, qint64 index )
{
    QSaveFile pending( keepAllPendingPath( basePath ) );
    if ( !pending.open( QIODevice::WriteOnly ) ) {
        return false;
    }
    const auto record = QByteArray::number( index ) + '\n';
    return pending.write( record ) == record.size() && pending.commit();
}

std::optional<std::vector<qint64>> readKeepAllBackupIndices(
    const QString& basePath, const std::optional<qint64>& pendingIndex )
{
    QFile manifest( keepAllManifestPath( basePath ) );
    if ( !manifest.open( QIODevice::ReadOnly ) ) {
        return std::nullopt;
    }

    auto contents = manifest.readAll();
    if ( !contents.isEmpty() && !contents.endsWith( '\n' ) ) {
        const auto finalSeparator = contents.lastIndexOf( '\n' );
        const auto fragment = contents.mid( finalSeparator + 1 );
        if ( !pendingIndex.has_value()
             || !QByteArray::number( pendingIndex.value() ).startsWith( fragment ) ) {
            return std::nullopt;
        }
        contents.truncate( finalSeparator + 1 );
    }

    std::vector<qint64> indices;
    for ( const auto& rawLine : contents.split( '\n' ) ) {
        const auto line = rawLine.trimmed();
        if ( line.isEmpty() ) {
            continue;
        }
        bool parsed = false;
        const auto index = QString::fromLatin1( line ).toLongLong( &parsed );
        if ( !parsed || index < 0 || index == std::numeric_limits<qint64>::max()
             || line != QByteArray::number( index )
             || ( !indices.empty() && index <= indices.back() ) ) {
            return std::nullopt;
        }
        indices.push_back( index );
    }
    return indices;
}

bool writeKeepAllBackupIndices( const QString& basePath,
                                const std::vector<qint64>& indices )
{
    QSaveFile manifest( keepAllManifestPath( basePath ) );
    if ( !manifest.open( QIODevice::WriteOnly ) ) {
        return false;
    }
    for ( const auto index : indices ) {
        const auto record = QByteArray::number( index ) + '\n';
        if ( manifest.write( record ) != record.size() ) {
            return false;
        }
    }
    return manifest.commit();
}

bool appendKeepAllBackupIndex( const QString& basePath, qint64 index )
{
    QFile manifest( keepAllManifestPath( basePath ) );
    if ( !manifest.open( QIODevice::WriteOnly | QIODevice::Append ) ) {
        return false;
    }
    const auto record = QByteArray::number( index ) + '\n';
    return manifest.write( record ) == record.size() && manifest.flush();
}

bool clearKeepAllOwnershipMetadata( const QString& basePath )
{
    const auto manifestPath = keepAllManifestPath( basePath );
    const auto pendingPath = keepAllPendingPath( basePath );
    if ( !QFile::exists( manifestPath ) && !QFile::exists( pendingPath ) ) {
        return true;
    }
    const std::vector<qint64> noBackups;
    if ( !writeKeepAllBackupIndices( basePath, noBackups ) ) {
        return false;
    }
    if ( QFile::exists( pendingPath ) && !QFile::remove( pendingPath )
         && QFile::exists( pendingPath ) ) {
        return false;
    }
    // Failure to remove the now-empty manifest is safe: future appends still
    // start from an exact empty ownership set.
    QFile::remove( manifestPath );
    return true;
}

std::optional<std::vector<qint64>> loadKeepAllBackupIndices( const QString& basePath )
{
    const auto pendingIndex = readKeepAllPendingIndex( basePath );
    auto indices = readKeepAllBackupIndices( basePath, pendingIndex );
    if ( !pendingIndex.has_value() ) {
        return indices;
    }

    const auto pendingPath = basePath + QStringLiteral( ".%1" ).arg( pendingIndex.value() );
    if ( !QFile::exists( pendingPath ) ) {
        QFile::remove( keepAllPendingPath( basePath ) );
        return indices;
    }
    if ( !indices.has_value() ) {
        return std::nullopt;
    }

    const auto found = std::lower_bound( indices->cbegin(), indices->cend(),
                                         pendingIndex.value() );
    if ( found == indices->cend() || *found != pendingIndex.value() ) {
        if ( found != indices->cend() ) {
            return std::nullopt;
        }
        indices->push_back( pendingIndex.value() );
    }
    if ( writeKeepAllBackupIndices( basePath, indices.value() ) ) {
        QFile::remove( keepAllPendingPath( basePath ) );
    }
    return indices;
}

std::vector<QString> managedBackupPaths(
    const QString& basePath, int backupCount,
    const std::vector<qint64>* cachedKeepAllBackupIndices )
{
    std::vector<QString> paths;
    std::optional<std::vector<qint64>> persistedIndices;
    const std::vector<qint64>* ownedIndices = nullptr;
    if ( backupCount == 0 ) {
        ownedIndices = cachedKeepAllBackupIndices;
        if ( ownedIndices == nullptr ) {
            persistedIndices = loadKeepAllBackupIndices( basePath );
            if ( persistedIndices.has_value() ) {
                ownedIndices = &persistedIndices.value();
            }
        }
    }
    if ( ownedIndices != nullptr ) {
        paths.reserve( ownedIndices->size() );
        for ( const auto index : *ownedIndices ) {
            const auto path = basePath + QStringLiteral( ".%1" ).arg( index );
            if ( QFile::exists( path ) ) {
                paths.push_back( path );
            }
        }
        return paths;
    }

    const auto entries = backupEntries( basePath );
    paths.reserve( entries.size() );
    std::transform( entries.cbegin(), entries.cend(), std::back_inserter( paths ),
                    []( const BackupEntry& entry ) { return entry.path; } );
    return paths;
}
} // namespace

RollingFileManager::RollingFileManager( QString basePath, qint64 maxFileSize, int backupCount )
    : basePath_( std::move( basePath ) )
    , maxFileSize_( maxFileSize )
    , backupCount_( std::clamp( backupCount, 0, kMaxBackupCount ) )
{
}

RollingFileManager::RollingFileManager( RollingFileManager&& other ) noexcept = default;

RollingFileManager& RollingFileManager::operator=( RollingFileManager&& other ) noexcept = default;

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
    if ( currentFile_ == nullptr ) {
        currentFile_ = std::make_unique<QFile>();
    }
    currentFile_->setFileName( basePath_ );
    if ( !currentFile_->open( QIODevice::WriteOnly | QIODevice::ExistingOnly
                             | QIODevice::Append ) ) {
        return false;
    }
    currentBytes_ = currentFile_->size();
    openedNewFile_ = false;
    return true;
}

void RollingFileManager::close()
{
    if ( currentFile_ == nullptr ) {
        currentBytes_ = 0;
        return;
    }
    if ( currentFile_->isOpen() ) {
        currentFile_->flush();
        currentFile_->close();
    }
    currentBytes_ = 0;
}

bool RollingFileManager::flush()
{
    if ( currentFile_ != nullptr && currentFile_->isOpen() ) {
        return currentFile_->flush();
    }
    return false;
}

bool RollingFileManager::openedNewFile() const
{
    return openedNewFile_;
}

bool RollingFileManager::refersToPath( const QString& path ) const
{
    if ( currentFile_ == nullptr || !currentFile_->isOpen() || path.isEmpty() ) {
        return false;
    }
#ifdef Q_OS_WIN
    const auto nativeHandle = _get_osfhandle( static_cast<int>( currentFile_->handle() ) );
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
    return ::fstat( currentFile_->handle(), &currentInfo ) == 0
           && ::stat( encodedPath.constData(), &pathInfo ) == 0
           && currentInfo.st_dev == pathInfo.st_dev && currentInfo.st_ino == pathInfo.st_ino;
#endif
}

bool RollingFileManager::clearIfCurrent()
{
    if ( !refersToPath( basePath_ ) || !currentFile_->flush() || !currentFile_->resize( 0 ) ) {
        return false;
    }

    currentBytes_ = 0;
    rotated_ = false;
    openedNewFile_ = false;

    const auto backupsRemoved = removeManagedBackups();
    finiteBackupsCleaned_ = true;
    QFile::remove( basePath_ + QStringLiteral( ".tmp_rotate" ) );
    return backupsRemoved;
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
    if ( currentFile_ == nullptr || data.isEmpty() || !currentFile_->isOpen() ) {
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
            const auto written = currentFile_->write( data.constData() + offset, size - offset );
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
            const auto written = currentFile_->write( data.constData() + offset,
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
            const auto written = currentFile_->write( data.constData() + offset, bytesToWrite );
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
            const auto written = currentFile_->write( data.constData() + offset,
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
    return currentFile_ != nullptr && currentFile_->isOpen() ? currentFile_.get() : nullptr;
}

QStringList RollingFileManager::backupFiles() const
{
    QStringList files;
    const auto paths = managedBackupPaths(
        basePath_, backupCount_,
        keepAllBackupIndicesLoaded_ ? &keepAllBackupIndices_ : nullptr );
    std::copy( paths.cbegin(), paths.cend(), std::back_inserter( files ) );
    return files;
}

void RollingFileManager::resyncSize()
{
    if ( currentFile_ != nullptr && currentFile_->isOpen() ) {
        currentBytes_ = currentFile_->size();
    }
}

void RollingFileManager::deleteAll()
{
    close();
    QFile::remove( basePath_ );
    (void) removeManagedBackups();
    finiteBackupsCleaned_ = true;
    QFile::remove( basePath_ + QStringLiteral( ".tmp_rotate" ) );
}

bool RollingFileManager::removeManagedBackups()
{
    if ( backupCount_ != 0 ) {
        bool allRemoved = true;
        for ( const auto& backup : managedBackupPaths( basePath_, backupCount_, nullptr ) ) {
            if ( QFile::exists( backup ) && !QFile::remove( backup )
                 && QFile::exists( backup ) ) {
                allRemoved = false;
            }
        }
        return allRemoved && clearKeepAllOwnershipMetadata( basePath_ );
    }

    if ( !keepAllBackupIndicesLoaded_ ) {
        auto persistedIndices = loadKeepAllBackupIndices( basePath_ );
        if ( persistedIndices.has_value() ) {
            keepAllBackupIndices_ = std::move( persistedIndices ).value();
            keepAllManifestNeedsRewrite_ = false;
        }
        else {
            const auto backups = backupEntries( basePath_ );
            keepAllBackupIndices_.clear();
            keepAllBackupIndices_.reserve( backups.size() );
            std::transform( backups.cbegin(), backups.cend(),
                            std::back_inserter( keepAllBackupIndices_ ),
                            []( const BackupEntry& entry ) { return entry.index; } );
            keepAllManifestNeedsRewrite_ = true;
        }
        keepAllBackupIndicesLoaded_ = true;
    }

    std::vector<qint64> retainedIndices;
    for ( const auto index : keepAllBackupIndices_ ) {
        const auto path = backupPath( index );
        if ( QFile::exists( path ) && !QFile::remove( path ) && QFile::exists( path ) ) {
            retainedIndices.push_back( index );
        }
    }
    keepAllBackupIndices_ = std::move( retainedIndices );
    nextKeepAllBackupIndex_.reset();

    if ( keepAllBackupIndices_.empty() ) {
        const auto metadataCleared = clearKeepAllOwnershipMetadata( basePath_ );
        keepAllManifestNeedsRewrite_ = !metadataCleared;
        return metadataCleared;
    }

    const auto persisted = writeKeepAllBackupIndices( basePath_, keepAllBackupIndices_ );
    keepAllManifestNeedsRewrite_ = !persisted;
    if ( persisted ) {
        QFile::remove( keepAllPendingPath( basePath_ ) );
    }
    return false;
}

QString RollingFileManager::backupPath( qint64 index ) const
{
    return basePath_ + QStringLiteral( ".%1" ).arg( index );
}

bool RollingFileManager::openNewFile( bool truncate )
{
    if ( currentFile_ == nullptr ) {
        currentFile_ = std::make_unique<QFile>();
    }
    currentFile_->setFileName( basePath_ );

    // A truncating open always starts a fresh file, regardless of whether the
    // path already exists (FreshSave semantics).
    if ( truncate ) {
        if ( !currentFile_->open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
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
    if ( currentFile_->open( QIODevice::WriteOnly | QIODevice::NewOnly ) ) {
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
    if ( currentFile_->open( QIODevice::WriteOnly | QIODevice::ExistingOnly | QIODevice::Append ) ) {
        currentBytes_ = currentFile_->size();
        openedNewFile_ = false;
        return true;
    }

    // The ExistingOnly append failed, most likely because the path disappeared
    // after NewOnly's EEXIST (deleted by another process). Recreate it
    // atomically; the result now correctly reports a brand-new file. A second
    // NewOnly failure means the path reappeared in the meantime (or a genuine
    // open error) — surface it.
    if ( currentFile_->open( QIODevice::WriteOnly | QIODevice::NewOnly ) ) {
        currentBytes_ = 0;
        openedNewFile_ = true;
        return true;
    }

    LOG_WARNING << "RollingFileManager: failed to open " << basePath_;
    return false;
}

bool RollingFileManager::rotateInternal()
{
    if ( currentFile_ == nullptr || !currentFile_->isOpen() ) {
        return openNewFile();
    }

    rotated_ = true;

    // 1. Flush pending data
    currentFile_->flush();

    // 2. Open temp file BEFORE closing old one (no data loss gap)
    const auto tmpPath = basePath_ + QStringLiteral( ".tmp_rotate" );
    QFile tmpFile( tmpPath );
    if ( !tmpFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        LOG_WARNING << "RollingFileManager: failed to open temp file: " << tmpPath;
        return false;
    }
    tmpFile.close();

    // 3. Close old current file
    currentFile_->close();

    // 4. Finite retention shifts only its configured bounded window. Keep-all
    //    retention uses monotonically increasing backup numbers, so each rotation
    //    performs one rename rather than rewriting the complete history.
    QString rotatedPath;
    if ( backupCount_ > 0 ) {
        if ( !finiteBackupsCleaned_ ) {
            for ( const auto& backup : backupEntries( basePath_ ) ) {
                if ( backup.index >= backupCount_ ) {
                    QFile::remove( backup.path );
                }
            }
            finiteBackupsCleaned_ = true;
        }
        QFile::remove( backupPath( backupCount_ - 1 ) );
        for ( int index = backupCount_ - 2; index >= 0; --index ) {
            const auto source = backupPath( index );
            const auto destination = backupPath( index + 1 );
            QFile::remove( destination );
            if ( QFile::exists( source ) ) {
                QFile::rename( source, destination );
            }
        }
        rotatedPath = backupPath( 0 );
        QFile::remove( rotatedPath );
    }
    else {
        if ( !keepAllBackupIndicesLoaded_ ) {
            auto persistedIndices = loadKeepAllBackupIndices( basePath_ );
            if ( persistedIndices.has_value() ) {
                keepAllBackupIndices_ = std::move( persistedIndices ).value();
                keepAllManifestNeedsRewrite_ = false;
            }
            else {
                const auto backups = backupEntries( basePath_ );
                keepAllBackupIndices_.clear();
                keepAllBackupIndices_.reserve( backups.size() );
                std::transform( backups.cbegin(), backups.cend(),
                                std::back_inserter( keepAllBackupIndices_ ),
                                []( const BackupEntry& entry ) { return entry.index; } );
                keepAllManifestNeedsRewrite_ = true;
            }
            keepAllBackupIndicesLoaded_ = true;
        }
        if ( !nextKeepAllBackupIndex_.has_value() ) {
            nextKeepAllBackupIndex_ = keepAllBackupIndices_.empty()
                                          ? qint64{ 0 }
                                          : keepAllBackupIndices_.back() + 1;
        }
        auto nextIndex = nextKeepAllBackupIndex_.value_or( 0 );
        if ( nextIndex == std::numeric_limits<qint64>::max() ) {
            LOG_WARNING << "RollingFileManager: exhausted keep-all backup indices for "
                        << basePath_;
            QFile::remove( tmpPath );
            return false;
        }
        while ( QFile::exists( backupPath( nextIndex ) ) ) {
            if ( nextIndex == std::numeric_limits<qint64>::max() - 1 ) {
                LOG_WARNING << "RollingFileManager: exhausted keep-all backup indices for "
                            << basePath_;
                QFile::remove( tmpPath );
                return false;
            }
            ++nextIndex;
        }
        nextKeepAllBackupIndex_ = nextIndex;
        rotatedPath = backupPath( nextIndex );
    }

    if ( backupCount_ == 0 ) {
        if ( keepAllManifestNeedsRewrite_
             || !QFile::exists( keepAllManifestPath( basePath_ ) ) ) {
            if ( !writeKeepAllBackupIndices( basePath_, keepAllBackupIndices_ ) ) {
                LOG_WARNING << "RollingFileManager: failed to prepare keep-all backup manifest for "
                            << basePath_;
                QFile::remove( tmpPath );
                return false;
            }
            keepAllManifestNeedsRewrite_ = false;
            QFile::remove( keepAllPendingPath( basePath_ ) );
        }
        if ( !writeKeepAllPendingIndex( basePath_, nextKeepAllBackupIndex_.value_or( 0 ) ) ) {
            LOG_WARNING << "RollingFileManager: failed to prepare keep-all rotation journal for "
                        << basePath_;
            QFile::remove( tmpPath );
            return false;
        }
    }

    // 5. Rename the old current into the prepared backup slot.
    if ( !QFile::rename( basePath_, rotatedPath ) ) {
        LOG_WARNING << "RollingFileManager: rename failed: " << basePath_ << " → "
                    << rotatedPath;
        // Restore: reopen old file as current
        currentFile_->setFileName( basePath_ );
        (void) currentFile_->open( QIODevice::WriteOnly | QIODevice::Append );
        currentBytes_ = currentFile_->size();
        QFile::remove( tmpPath );
        if ( backupCount_ == 0 ) {
            QFile::remove( keepAllPendingPath( basePath_ ) );
        }
        return false;
    }

    // 6. Rename temp → new current
    if ( !QFile::rename( tmpPath, basePath_ ) ) {
        LOG_WARNING << "RollingFileManager: rename failed: " << tmpPath << " → " << basePath_;
        // Restore. If the rollback rename also fails, retain the pending
        // journal so startup recovery can adopt the published backup.
        const auto restoredCurrent = QFile::rename( rotatedPath, basePath_ );
        if ( restoredCurrent ) {
            currentFile_->setFileName( basePath_ );
            (void) currentFile_->open( QIODevice::WriteOnly | QIODevice::Append );
            currentBytes_ = currentFile_->size();
            if ( backupCount_ == 0 ) {
                QFile::remove( keepAllPendingPath( basePath_ ) );
            }
        }
        else {
            currentBytes_ = 0;
            if ( backupCount_ == 0 ) {
                const auto publishedIndex = nextKeepAllBackupIndex_.value_or( 0 );
                if ( keepAllBackupIndices_.empty()
                     || keepAllBackupIndices_.back() != publishedIndex ) {
                    keepAllBackupIndices_.push_back( publishedIndex );
                }
                nextKeepAllBackupIndex_ = publishedIndex + 1;
                keepAllManifestNeedsRewrite_ = true;
            }
        }
        return false;
    }
    if ( backupCount_ == 0 ) {
        const auto rotatedIndex = nextKeepAllBackupIndex_.value_or( 0 );
        keepAllBackupIndices_.push_back( rotatedIndex );
        nextKeepAllBackupIndex_ = rotatedIndex + 1;

        bool persisted = false;
        if ( !keepAllManifestNeedsRewrite_
             && QFile::exists( keepAllManifestPath( basePath_ ) ) ) {
            persisted = appendKeepAllBackupIndex( basePath_, rotatedIndex );
        }
        if ( !persisted ) {
            persisted = writeKeepAllBackupIndices( basePath_, keepAllBackupIndices_ );
        }
        keepAllManifestNeedsRewrite_ = !persisted;
        if ( persisted ) {
            QFile::remove( keepAllPendingPath( basePath_ ) );
        }
        else {
            LOG_WARNING << "RollingFileManager: failed to persist keep-all backup manifest for "
                        << basePath_;
        }
    }

    // 7. Open new current file
    return openNewFile();
}
