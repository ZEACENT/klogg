#ifndef FILEHOLDER_H
#define FILEHOLDER_H

#include <QFile>
#include <memory>

#include "synchronization.h"

struct FileId {
    uint64_t fileIndex = 0;
    uint64_t volumeIndex = 0;

    bool operator!=( const FileId& other ) const
    {
        return fileIndex != other.fileIndex || volumeIndex != other.volumeIndex;
    }

    static FileId getFileId( const QString& filename );
};

template <typename T> class ScopedFileHolder {
  public:
    explicit ScopedFileHolder( T* file )
        : file_holder_( file )
    {
        file_holder_->lock();
        file_holder_->attachReader();
    }

    ~ScopedFileHolder()
    {
        file_holder_->unlock();
        file_holder_->detachReader();
    }

    QFile* getFile()
    {
        return file_holder_->getFile();
    }

  private:
    Q_DISABLE_COPY( ScopedFileHolder<T> )

    T* file_holder_;
};

class FileHolder {
    friend class ScopedFileHolder<FileHolder>;

  public:
    explicit FileHolder( bool keepClosed );
    ~FileHolder();
    FileId getFileId();
    qint64 size();

    bool isOpen();

    void open( const QString& fileName );

    // Adopt an acquired handle into a fresh holder without reopening its path.
    // The file must belong to this thread or have no thread affinity/Qt parent.
    // Success consumes file and owns one reader, released with detachReader().
    bool adoptForReader( const QString& path, std::unique_ptr<QFile>& file,
                         const FileId& identity );

    void lock();
    void unlock();

    void attachReader();
    void detachReader();

    void reOpenFile();

  private:
    Q_DISABLE_COPY( FileHolder )

    QFile* getFile();

  private:
    RecursiveMutex file_mutex_;

    QString file_name_;
    std::unique_ptr<QFile> attached_file_;
    FileId attached_file_id_;

    uint32_t counter_ = 0;
    bool keep_closed_ = false;
};

#endif // FILEHOLDER_H
