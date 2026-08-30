#ifndef ROLLINGFILEMANAGER_H
#define ROLLINGFILEMANAGER_H

#include <QFile>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

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
    bool openExisting();
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
    // True if the most recent open() started a brand-new file: either a
    // truncating open, or an append that had to create a previously-missing
    // path. Callers use this instead of their own pre-open existence check to
    // decide whether a capture must be replayed into the file, avoiding the
    // TOCTOU window between that check and the actual open().
    bool openedNewFile() const;
    bool refersToPath( const QString& path ) const;
    // Truncate through the active handle only when it still owns basePath_.
    // Returns false without modifying any pathname after external replacement.
    bool clearIfCurrent();
    bool removeCurrentFile();
    qint64 currentFileSize() const;
    qint64 maxFileSize() const;
    int backupCount() const;
    QFile* currentFile();

    // List backup files in numeric suffix order.
    QStringList backupFiles() const;

    // Re-read the current file's size from disk.  Call this after writing
    // directly to currentFile() (bypassing write()) so that needsRotation()
    // and currentFileSize() reflect the true on-disk size.
    void resyncSize();

    // Delete all files (current + backups).
    void deleteAll();

  private:
    QString backupPath( qint64 index ) const;
    bool openNewFile( bool truncate = false );
    bool rotateInternal();
    bool removeManagedBackups();

    QString basePath_;
    qint64 maxFileSize_ = 0;
    int backupCount_ = 0;
    std::unique_ptr<QFile> currentFile_ = std::make_unique<QFile>();
    qint64 currentBytes_ = 0;
    bool rotated_ = false;
    bool openedNewFile_ = false;
    std::optional<qint64> nextKeepAllBackupIndex_;
    std::vector<qint64> keepAllBackupIndices_;
    bool keepAllBackupIndicesLoaded_ = false;
    bool keepAllManifestNeedsRewrite_ = false;
    bool finiteBackupsCleaned_ = false;
};

#endif
