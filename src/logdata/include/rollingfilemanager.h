#ifndef ROLLINGFILEMANAGER_H
#define ROLLINGFILEMANAGER_H

#include <QFile>
#include <QString>
#include <QStringList>

// Manages a rolling output file that rotates when it reaches a size limit.
// When the current file exceeds maxFileSize, it is renamed as a numbered backup
// and a new file is opened with the original name. Old backups beyond backupCount
// are automatically deleted. When backupCount is 0, all rotated files are kept
// indefinitely (no cleanup).
//
// The rename is done atomically. To avoid data loss during the gap between closing
// the old file and opening the new one, the rotation opens the new file BEFORE
// closing the old one.
class RollingFileManager {
  public:
    RollingFileManager() = default;
    RollingFileManager( QString basePath, qint64 maxFileSize, int backupCount );
    RollingFileManager( RollingFileManager&& other ) noexcept;
    RollingFileManager& operator=( RollingFileManager&& other ) noexcept;
    RollingFileManager( const RollingFileManager& ) = delete;
    RollingFileManager& operator=( const RollingFileManager& ) = delete;

    bool isValid() const;
    bool open( bool truncate = false );
    void close();
    bool flush();

    // Write data to the current file. Automatically rotates if the file exceeds
    // maxFileSize. Returns the number of bytes written (may be less than data.size()
    // if rotation is needed; call again with the remaining data).
    qint64 write( const QByteArray& data );

    // Force a rotation (e.g., during CaptureStore trim).
    void rotate();

    bool needsRotation() const;
    // True if the most recent write() call (or a force-rotate) rotated the file.
    // More reliable than comparing currentFileSize() before/after a write, which
    // misses rotations that leave the new file at least as large as the old one.
    bool rotated() const;
    qint64 currentFileSize() const;
    qint64 maxFileSize() const;
    int backupCount() const;
    QFile* currentFile();

    // List backup files in order (oldest first).
    QStringList backupFiles() const;

    // Re-read the current file's size from disk.  Call this after writing
    // directly to currentFile() (bypassing write()) so that needsRotation()
    // and currentFileSize() reflect the true on-disk size.
    void resyncSize();

    // Delete all files (current + backups).
    void deleteAll();

  private:
    QString backupPath( int index ) const;
    void cleanupOldBackups();
    bool openNewFile( bool truncate = false );
    bool rotateInternal();

    QString basePath_;
    qint64 maxFileSize_ = 0;
    int backupCount_ = 0;
    QFile currentFile_;
    qint64 currentBytes_ = 0;
    bool rotated_ = false;
};

#endif
