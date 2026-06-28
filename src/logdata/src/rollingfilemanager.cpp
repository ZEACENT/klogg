#include "rollingfilemanager.h"

#include <QDir>
#include <QFileInfo>

#include "log.h"

RollingFileManager::RollingFileManager( QString basePath, qint64 maxFileSize, int backupCount )
    : basePath_( std::move( basePath ) )
    , maxFileSize_( maxFileSize )
    , backupCount_( backupCount )
{
}

RollingFileManager::RollingFileManager( RollingFileManager&& other ) noexcept
    : basePath_( std::move( other.basePath_ ) )
    , maxFileSize_( other.maxFileSize_ )
    , backupCount_( other.backupCount_ )
    , currentBytes_( other.currentBytes_ )
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

qint64 RollingFileManager::write( const QByteArray& data )
{
    if ( data.isEmpty() || !currentFile_.isOpen() ) {
        return 0;
    }

    // Auto-rotate if current file is full (from previous writes)
    if ( needsRotation() ) {
        if ( !rotateInternal() ) {
            return 0;
        }
    }

    // maxFileSize_ = 0 means no rolling: write everything without size checks
    if ( maxFileSize_ <= 0 ) {
        const auto written = currentFile_.write( data );
        if ( written > 0 ) {
            currentBytes_ += written;
        }
        return written;
    }

    const auto remainingCapacity = maxFileSize_ - currentBytes_;

    // If the entire data fits, write it all
    if ( data.size() <= remainingCapacity ) {
        const auto written = currentFile_.write( data );
        if ( written > 0 ) {
            currentBytes_ += written;
        }
        // Rotate if we've hit the limit
        if ( needsRotation() ) {
            rotateInternal();
        }
        return written;
    }

    // Data doesn't fit entirely. Find the last complete line that fits.
    // Write only complete lines to avoid splitting across files.
    auto bytesToWrite = static_cast<qint64>( data.size() );
    if ( bytesToWrite > remainingCapacity ) {
        bytesToWrite = remainingCapacity;
    }

    // Search backwards for a newline within the write range
    const auto lastNewline = data.lastIndexOf( '\n', static_cast<int>( bytesToWrite ) - 1 );
    if ( lastNewline >= 0 ) {
        // Write up to and including the last complete line
        bytesToWrite = lastNewline + 1;
    }
    else {
        // No complete line fits in remaining space. Rotate first, then retry.
        if ( !rotateInternal() ) {
            return 0;
        }
        // After rotation, the file is fresh. Write the entire data.
        const auto written = currentFile_.write( data );
        if ( written > 0 ) {
            currentBytes_ += written;
        }
        return written;
    }

    if ( bytesToWrite <= 0 ) {
        return 0;
    }

    const auto written = currentFile_.write( data.constData(), bytesToWrite );
    if ( written > 0 ) {
        currentBytes_ += written;
    }

    // Rotate if we've hit the limit, then write remaining data to the new file
    if ( needsRotation() ) {
        rotateInternal();
        const auto remaining = data.mid( static_cast<int>( written ) );
        if ( !remaining.isEmpty() && currentFile_.isOpen() ) {
            const auto extraWritten = currentFile_.write( remaining );
            if ( extraWritten > 0 ) {
                currentBytes_ += extraWritten;
            }
            return written + extraWritten;
        }
    }

    return written;
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
    for ( int i = backupCount_; i < backupCount_ + 100; ++i ) {
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
    const auto mode = truncate ? ( QIODevice::WriteOnly | QIODevice::Truncate )
                               : ( QIODevice::WriteOnly | QIODevice::Append );
    if ( !currentFile_.open( mode ) ) {
        LOG_WARNING << "RollingFileManager: failed to open " << basePath_;
        return false;
    }
    currentBytes_ = truncate ? 0 : currentFile_.size();
    return true;
}

bool RollingFileManager::rotateInternal()
{
    if ( !currentFile_.isOpen() ) {
        return openNewFile();
    }

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
