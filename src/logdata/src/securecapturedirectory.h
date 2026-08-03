#ifndef SECURECAPTUREDIRECTORY_H
#define SECURECAPTUREDIRECTORY_H

#include <memory>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>

class SecureCaptureDirectory {
  public:
    enum class PublishResult {
        Success,
        AlreadyExists,
        Error,
    };

    explicit SecureCaptureDirectory( QString path );
    ~SecureCaptureDirectory();

    SecureCaptureDirectory( const SecureCaptureDirectory& ) = delete;
    SecureCaptureDirectory& operator=( const SecureCaptureDirectory& ) = delete;
    SecureCaptureDirectory( SecureCaptureDirectory&& ) noexcept;
    SecureCaptureDirectory& operator=( SecureCaptureDirectory&& ) noexcept;

    bool ensureExists();
    bool bindExisting();
    bool isCurrentPath() const;
    bool isRemoved() const;
    QString identityKey() const;
    QString path() const;

    QStringList entryList( const QStringList& nameFilters, QDir::Filters filters,
                           QDir::SortFlags sort = QDir::NoSort ) const;
    QStringList entryList( QDir::Filters filters,
                           QDir::SortFlags sort = QDir::NoSort ) const;

    bool openReadFile( const QString& filePath, QFile& file ) const;
    std::unique_ptr<QFile> createTemporaryFile( QString& filePath ) const;
    PublishResult publishTemporaryFile( const QString& temporaryPath,
                                        const QString& targetPath ) const;
    bool removeFile( const QString& filePath ) const;
    bool removeIfEmpty();
    bool removeRecursively();
    QDateTime latestModificationTime() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
