#ifndef SECURECAPTUREDIRECTORY_H
#define SECURECAPTUREDIRECTORY_H

#include <cstdint>
#include <functional>
#include <memory>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>

class SecureCaptureDirectory {
  public:
    enum class PublishResult : std::uint8_t {
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
    QString fileIdentity( const QString& filePath ) const;
    QString path() const;
    bool hasEntries() const;

    QStringList entryList( const QStringList& nameFilters, QDir::Filters filters,
                           QDir::SortFlags sort = QDir::NoSort ) const;
    QStringList entryList( QDir::Filters filters,
                           QDir::SortFlags sort = QDir::NoSort ) const;

    std::unique_ptr<QFile> openReadFile(
        const QString& filePath, const QString& expectedIdentity = {} ) const;
    std::unique_ptr<QFile> createTemporaryFile( QString& filePath ) const;
    PublishResult publishTemporaryFile( const QString& temporaryPath,
                                        const QString& targetPath ) const;
    bool removeFile( const QString& filePath,
                     const QString& expectedIdentity = {} ) const;
    bool removeIfEmpty();
    bool removeRecursively();
    void failNextRecursiveRemovalForTesting();
    void setAfterRecursiveRemovalQuarantineCallbackForTesting(
        std::function<void()> callback );
    QDateTime latestModificationTime() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
