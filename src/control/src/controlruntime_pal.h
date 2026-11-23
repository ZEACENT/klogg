#ifndef KLOGG_CONTROLRUNTIME_PAL_H
#define KLOGG_CONTROLRUNTIME_PAL_H

#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QStringList>
#include <QLocalServer>

namespace klogg::control::pal {
struct FileIdentity {
    quint64 device = 0;
    quint64 file = 0;
    bool valid = false;
};

struct DirectoryEntries {
    QStringList names;
    bool complete = false;
};
struct RegisteredFile {
    QByteArray bytes;
    FileIdentity identity;
    bool ownerPresent;
};

// Native no-follow opens, owner/mode/ACL checks, and anchored descriptor I/O.
class PrivateDirectory {
  public:
    PrivateDirectory();
    ~PrivateDirectory();
    bool open( const QString& directoryPath, bool create );
    QString path() const;
    DirectoryEntries entries( int limit ) const;
    std::optional<RegisteredFile> read( const QString& name, int limit ) const;
    // The publisher holds an exclusive kernel lease from before publication
    // through teardown. Shared nonblocking probes cannot impersonate an owner.
    using RecordWriter = std::function<QByteArray( FileIdentity )>;
    bool publish( const QString& name, const RecordWriter& serialize, FileIdentity* identityOut );
    FileIdentity identity( const QString& name ) const;
    void removeOwned( const QString& name, FileIdentity expected );
    void removeDirectory();
    bool unchanged() const;

  private:
    struct Private;
    std::unique_ptr<Private> d_;
};

QString defaultDescriptorRoot();
QString defaultSocketRoot();
QString socketDirectory( const QString& socketRoot, const QString& instance );
QString endpoint( const QString& socketRoot, const QString& instance );
bool validEndpoint( const QString& value, const QString& socketRoot, const QString& instance );
bool privateEndpoint( const QString& name, const QString& instance );
// Client: verify before sending credentials. Server: verify after reading the
// first frame, before acknowledging hello or dispatching (Windows pipe token).
bool sameUserPeer( qintptr descriptor, bool serverSide );
// QLocalServer unlinks the bound pathname on close even for listen(fd).
// Unix acceptance therefore owns its native fd here and delivers QLocalSockets
// through the pending-connections API; only runtime removes pathnames.
class LocalListener : public QLocalServer {
  public:
    LocalListener();
    ~LocalListener() override;
    bool open( const QString& name, qintptr listener );
    void close();
    bool hasPendingConnections() const override;
    QLocalSocket* nextPendingConnection() override;

  protected:
    void incomingConnection( quintptr descriptor ) override;

  private:
    struct Native;
    std::unique_ptr<Native> native_;
};
qintptr createListener( const QString& name, FileIdentity* ownedSocket );
void closeListener( qintptr descriptor );
} // namespace klogg::control::pal
#endif
