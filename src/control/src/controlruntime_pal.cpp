#include "controlruntime_pal.h"
#include "controlfilesystemsecurity_p.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <deque>
#include <QStandardPaths>
#include <QSocketNotifier>
#include <QUuid>

#include <utility>
#include <vector>
#include <exception>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace klogg::control::pal {
namespace {
bool safeName( const QString& name )
{
    return !name.isEmpty() && name != QStringLiteral( "." ) && name != QStringLiteral( ".." )
           && !name.contains( '/' ) && !name.contains( '\\' ) && !name.contains( QChar( 0 ) );
}
bool same( FileIdentity first, FileIdentity second )
{
    return first.valid && second.valid && first.device == second.device && first.file == second.file;
}
} // namespace

QString defaultDescriptorRoot()
{
    // Canonicalize the OS-selected parent (e.g. macOS /var -> /private/var).
    const auto cache = QStandardPaths::writableLocation( QStandardPaths::GenericCacheLocation );
    const auto canonical = QFileInfo( cache ).canonicalFilePath();
    return ( canonical.isEmpty() ? cache : canonical ) + QStringLiteral( "/klogg-control" );
}
QString defaultSocketRoot()
{
#ifdef Q_OS_WIN
    return QFileInfo( QDir::tempPath() ).canonicalFilePath();
#else
    // macOS per-user TMPDIR is often too long for sockaddr_un::sun_path.
    return QFileInfo( QStringLiteral( "/tmp" ) ).canonicalFilePath();
#endif
}

#ifdef Q_OS_WIN
namespace {
class Handle {
  public:
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle( HANDLE handle ) : value( handle ) {}
    ~Handle() { if ( value && value != INVALID_HANDLE_VALUE ) CloseHandle( value ); }
    Handle( const Handle& ) = delete;
    Handle& operator=( const Handle& ) = delete;
};

struct UserSecurity {
    QByteArray tokenInfo;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{ sizeof( SECURITY_ATTRIBUTES ), nullptr, FALSE };
    UserSecurity()
    {
        HANDLE raw = INVALID_HANDLE_VALUE;
        if ( !OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &raw ) ) return;
        Handle token( raw );
        DWORD size = 0;
        GetTokenInformation( token.value, TokenUser, nullptr, 0, &size );
        if ( size == 0 || size > 65536 ) return;
        tokenInfo.resize( static_cast<int>( size ) );
        if ( !GetTokenInformation( token.value, TokenUser, tokenInfo.data(), size, &size ) ) return;
        LPWSTR sidText = nullptr;
        if ( !ConvertSidToStringSidW( sid(), &sidText ) ) return;
        const QString sddl = QStringLiteral( "O:" ) + QString::fromWCharArray( sidText )
                             + QStringLiteral( "D:P(A;OICI;FA;;;" )
                             + QString::fromWCharArray( sidText ) + QStringLiteral( ")" );
        LocalFree( sidText );
        if ( ConvertStringSecurityDescriptorToSecurityDescriptorW(
                 reinterpret_cast<LPCWSTR>( sddl.utf16() ), SDDL_REVISION_1, &descriptor, nullptr ) ) {
            attributes.lpSecurityDescriptor = descriptor;
        }
    }
    ~UserSecurity() { if ( descriptor ) LocalFree( descriptor ); }
    PSID sid() const
    {
        return tokenInfo.isEmpty() ? nullptr
            : reinterpret_cast<const TOKEN_USER*>( tokenInfo.constData() )->User.Sid;
    }
};

bool privateHandle( HANDLE handle, bool directory )
{
    BY_HANDLE_FILE_INFORMATION info{};
    if ( !GetFileInformationByHandle( handle, &info )
         || ( info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ) != 0
         || ( ( info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0 ) != directory
         || ( !directory && info.nNumberOfLinks != 1 ) ) return false;
    UserSecurity user;
    if ( !user.descriptor ) return false;
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR security = nullptr;
    if ( GetSecurityInfo( handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                          &owner, nullptr, &acl, nullptr, &security ) != ERROR_SUCCESS ) return false;
    bool valid = owner && EqualSid( owner, user.sid() ) && acl;
    if ( valid ) {
        for ( DWORD i = 0; i < acl->AceCount; ++i ) {
            void* ace = nullptr;
            if ( !GetAce( acl, i, &ace ) ) { valid = false; break; }
            const auto* header = static_cast<const ACE_HEADER*>( ace );
            if ( header->AceType == ACCESS_ALLOWED_ACE_TYPE ) {
                auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>( ace );
                if ( !EqualSid( &allowed->SidStart, user.sid() ) ) { valid = false; break; }
            }
            else if ( header->AceType != ACCESS_DENIED_ACE_TYPE ) { valid = false; break; }
        }
    }
    LocalFree( security );
    return valid;
}

HANDLE openNative( const QString& path, DWORD access, bool directory )
{
    // No FILE_SHARE_DELETE: hold the verified object against replacement.
    return CreateFileW( reinterpret_cast<LPCWSTR>( path.utf16() ), access,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT
                            | ( directory ? FILE_FLAG_BACKUP_SEMANTICS : 0 ), nullptr );
}
FileIdentity fileIdentity( HANDLE handle )
{
    BY_HANDLE_FILE_INFORMATION info{};
    if ( !GetFileInformationByHandle( handle, &info ) ) return {};
    return { info.dwVolumeSerialNumber,
             ( static_cast<quint64>( info.nFileIndexHigh ) << 32U ) | info.nFileIndexLow, true };
}
} // namespace

struct PrivateDirectory::Private {
    Handle handle;
    Handle published;
    QString path;
    FileIdentity identity;
    // Pin ancestors too, preventing rename/reparse substitution of a parent.
    std::vector<std::unique_ptr<Handle>> ancestors;
};
PrivateDirectory::PrivateDirectory() : d_( std::make_unique<Private>() ) {}
PrivateDirectory::~PrivateDirectory() = default;
bool PrivateDirectory::open( const QString& directoryPath, bool create )
{
    if ( !QDir::isAbsolutePath( directoryPath ) || directoryPath.contains( QChar( 0 ) ) ) return false;
    const QString clean = QDir::cleanPath( QDir::fromNativeSeparators( directoryPath ) );
    if ( clean.startsWith( "//" ) || clean.size() < 3 || clean.at( 1 ) != ':' ) return false;
    QString current = clean.left( 3 );
    auto parts = clean.mid( 3 ).split( '/' );
    parts.removeAll( QString{} );
    UserSecurity security;
    if ( !security.descriptor || parts.isEmpty() ) return false;
    for ( const auto& part : parts ) {
        if ( !safeName( part ) || part.contains( ':' ) ) return false;
        current = QDir( current ).filePath( part );
        if ( create && !CreateDirectoryW( reinterpret_cast<LPCWSTR>( current.utf16() ),
                                         &security.attributes )
             && GetLastError() != ERROR_ALREADY_EXISTS ) return false;
        auto handle = std::make_unique<Handle>( openNative( current, READ_CONTROL | FILE_READ_ATTRIBUTES, true ) );
        BY_HANDLE_FILE_INFORMATION info{};
        if ( handle->value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle( handle->value, &info )
             || ( info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ) != 0
             || ( info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) == 0 ) return false;
        d_->ancestors.push_back( std::move( handle ) );
    }
    d_->handle.value = openNative( clean, READ_CONTROL | FILE_READ_ATTRIBUTES, true );
    if ( d_->handle.value == INVALID_HANDLE_VALUE || !privateHandle( d_->handle.value, true ) ) return false;
    d_->path = clean;
    d_->identity = fileIdentity( d_->handle.value );
    return true;
}
QString PrivateDirectory::path() const { return d_->path; }
bool PrivateDirectory::unchanged() const
{
    Handle handle( openNative( d_->path, READ_CONTROL | FILE_READ_ATTRIBUTES, true ) );
    return handle.value != INVALID_HANDLE_VALUE && privateHandle( handle.value, true )
           && same( d_->identity, fileIdentity( handle.value ) );
}
DirectoryEntries PrivateDirectory::entries( int limit ) const
{
    DirectoryEntries result;
    if ( !unchanged() ) return result;
    WIN32_FIND_DATAW data{};
    const auto pattern = QDir( d_->path ).filePath( QStringLiteral( "*" ) );
    const HANDLE search = FindFirstFileW( reinterpret_cast<LPCWSTR>( pattern.utf16() ), &data );
    if ( search == INVALID_HANDLE_VALUE ) {
        result.complete = GetLastError() == ERROR_FILE_NOT_FOUND;
        return result;
    }
    int inspected = 0;
    do {
        if ( ++inspected > limit ) { FindClose( search ); return result; }
        const auto name = QString::fromWCharArray( data.cFileName );
        if ( name.endsWith( QStringLiteral( ".instance" ) ) ) result.names.push_back( name );
    } while ( FindNextFileW( search, &data ) );
    result.complete = GetLastError() == ERROR_NO_MORE_FILES;
    FindClose( search );
    return result;
}
std::optional<RegisteredFile> PrivateDirectory::read( const QString& name, int limit ) const
{
    if ( !safeName( name ) || !unchanged() ) return {};
    Handle handle( openNative( QDir( d_->path ).filePath( name ), GENERIC_READ | READ_CONTROL, false ) );
    if ( handle.value == INVALID_HANDLE_VALUE || !privateHandle( handle.value, false ) ) return {};
    LARGE_INTEGER size{};
    if ( !GetFileSizeEx( handle.value, &size ) || size.QuadPart <= 0 || size.QuadPart > limit ) return {};
    QByteArray bytes( static_cast<int>( size.QuadPart ), '\0' );
    DWORD readCount = 0;
    if ( !ReadFile( handle.value, bytes.data(), static_cast<DWORD>( bytes.size() ), &readCount, nullptr )
         || readCount != static_cast<DWORD>( bytes.size() ) ) return {};
    // Use a byte beyond all descriptor contents: the lease cannot block ReadFile.
    OVERLAPPED lease{};
    lease.OffsetHigh = 1;
    const bool shared = LockFileEx( handle.value, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &lease ) != FALSE;
    if ( !shared && GetLastError() != ERROR_LOCK_VIOLATION ) return {};
    return RegisteredFile{ std::move( bytes ), fileIdentity( handle.value ), !shared };
}
FileIdentity PrivateDirectory::identity( const QString& name ) const
{
    if ( !safeName( name ) || !unchanged() ) return {};
    Handle handle( openNative( QDir( d_->path ).filePath( name ), FILE_READ_ATTRIBUTES, false ) );
    return handle.value == INVALID_HANDLE_VALUE ? FileIdentity{} : fileIdentity( handle.value );
}
bool PrivateDirectory::publish( const QString& name, const RecordWriter& serialize, FileIdentity* identityOut )
{
    if ( !safeName( name ) || !unchanged() ) return false;
    UserSecurity security;
    if ( !security.descriptor ) return false;
    const auto finalPath = QDir( d_->path ).filePath( name );
    const auto temporary = finalPath + QStringLiteral( "." ) + QUuid::createUuid().toString( QUuid::Id128 );
    Handle handle( CreateFileW( reinterpret_cast<LPCWSTR>( temporary.utf16() ), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security.attributes,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr ) );
    if ( handle.value == INVALID_HANDLE_VALUE ) return false;
    OVERLAPPED lease{};
    lease.OffsetHigh = 1;
    const auto publishedIdentity = fileIdentity( handle.value );
    const auto bytes = serialize( publishedIdentity );
    DWORD count = 0;
    const bool written = publishedIdentity.valid
        && LockFileEx( handle.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &lease )
        && WriteFile( handle.value, bytes.constData(), static_cast<DWORD>( bytes.size() ), &count, nullptr )
        && count == static_cast<DWORD>( bytes.size() ) && FlushFileBuffers( handle.value );
    const bool moved = written && MoveFileExW( reinterpret_cast<LPCWSTR>( temporary.utf16() ),
                                              reinterpret_cast<LPCWSTR>( finalPath.utf16() ), MOVEFILE_WRITE_THROUGH );
    if ( moved ) {
        d_->published.value = std::exchange( handle.value, INVALID_HANDLE_VALUE );
        *identityOut = publishedIdentity;
    } else {
        DeleteFileW( reinterpret_cast<LPCWSTR>( temporary.utf16() ) );
    }
    return moved;
}
void PrivateDirectory::removeOwned( const QString& name, FileIdentity expected )
{
    if ( !safeName( name ) || !unchanged() ) return;
    const auto file = QDir( d_->path ).filePath( name );
    Handle handle( openNative( file, DELETE | FILE_READ_ATTRIBUTES, false ) );
    if ( handle.value == INVALID_HANDLE_VALUE || !same( expected, fileIdentity( handle.value ) ) ) return;
    FILE_DISPOSITION_INFO disposition{ TRUE };
    SetFileInformationByHandle( handle.value, FileDispositionInfo, &disposition, sizeof( disposition ) );
}
void PrivateDirectory::removeDirectory()
{
    // Windows uses no per-epoch filesystem socket directory.
}
QString socketDirectory( const QString&, const QString& ) { return {}; }
QString endpoint( const QString&, const QString& instance )
{
    return QStringLiteral( "klogg-control-" ) + instance;
}
bool privateEndpoint( const QString& name, const QString& instance )
{
    // In particular, never accept \\server\\pipe remote named-pipe paths.
    return name == endpoint( {}, instance );
}
bool sameUserPeer( qintptr descriptor, bool serverSide )
{
    const auto pipe = reinterpret_cast<HANDLE>( descriptor );
    HANDLE raw = INVALID_HANDLE_VALUE;
    if ( serverSide ) {
        // Call only after reading client bytes: impersonation identifies the
        // last pipe writer. A remote client's PID is NOT a local process ID.
        HANDLE originalRaw = INVALID_HANDLE_VALUE;
        const bool hadToken = OpenThreadToken( GetCurrentThread(), TOKEN_QUERY | TOKEN_IMPERSONATE,
                                               TRUE, &originalRaw ) != FALSE;
        if ( !hadToken && GetLastError() != ERROR_NO_TOKEN ) return false;
        Handle original( originalRaw );
        if ( !ImpersonateNamedPipeClient( pipe ) ) return false;
        const bool obtained = OpenThreadToken( GetCurrentThread(), TOKEN_QUERY, TRUE, &raw ) != FALSE;
        const bool restored = hadToken ? SetThreadToken( nullptr, original.value ) != FALSE
                                       : RevertToSelf() != FALSE;
        // Continuing under a peer's impersonation token is never safe.
        if ( !restored ) std::terminate();
        if ( !obtained ) return false;
    }
    else {
        // The client accepts only local pipe names; verify the actual server
        // process before revealing the descriptor credential to a stale pipe.
        DWORD pid = 0;
        if ( !GetNamedPipeServerProcessId( pipe, &pid ) ) return false;
        Handle process( OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid ) );
        if ( !process.value || !OpenProcessToken( process.value, TOKEN_QUERY, &raw ) ) return false;
    }
    Handle token( raw );
    if ( serverSide ) {
        // Same-account SMB authentication still carries the kernel NETWORK SID.
        // Reject its presence, even if disabled/deny-only, before hello reply.
        // This does not reject an ordinary local process running in an RDP session.
        DWORD groupBytes = 0;
        GetTokenInformation( token.value, TokenGroups, nullptr, 0, &groupBytes );
        if ( groupBytes == 0 || groupBytes > 65536 ) return false;
        QByteArray groups( static_cast<int>( groupBytes ), '\0' );
        if ( !GetTokenInformation( token.value, TokenGroups, groups.data(), groupBytes, &groupBytes ) ) return false;
        alignas( SID ) BYTE networkSid[SECURITY_MAX_SID_SIZE]{};
        DWORD sidBytes = sizeof( networkSid );
        if ( !CreateWellKnownSid( WinNetworkSid, nullptr, networkSid, &sidBytes ) ) return false;
        const auto* tokenGroups = reinterpret_cast<const TOKEN_GROUPS*>( groups.constData() );
        for ( DWORD i = 0; i < tokenGroups->GroupCount; ++i ) {
            if ( EqualSid( tokenGroups->Groups[i].Sid, networkSid ) ) return false;
        }
    }
    DWORD size = 0;
    GetTokenInformation( token.value, TokenUser, nullptr, 0, &size );
    if ( size == 0 || size > 65536 ) return false;
    QByteArray info( static_cast<int>( size ), '\0' );
    if ( !GetTokenInformation( token.value, TokenUser, info.data(), size, &size ) ) return false;
    UserSecurity user;
    return user.descriptor
           && EqualSid( reinterpret_cast<const TOKEN_USER*>( info.constData() )->User.Sid, user.sid() );
}
qintptr createListener( const QString&, FileIdentity* ) { return -1; }
void closeListener( qintptr ) {}
struct LocalListener::Native {};
LocalListener::LocalListener() : native_( std::make_unique<Native>() ) {}
LocalListener::~LocalListener() { close(); }
bool LocalListener::open( const QString& name, qintptr )
{
    setSocketOptions( QLocalServer::UserAccessOption );
    return QLocalServer::listen( name );
}
void LocalListener::close() { QLocalServer::close(); }
bool LocalListener::hasPendingConnections() const { return QLocalServer::hasPendingConnections(); }
QLocalSocket* LocalListener::nextPendingConnection() { return QLocalServer::nextPendingConnection(); }
void LocalListener::incomingConnection( quintptr descriptor ) { QLocalServer::incomingConnection( descriptor ); }
#else
namespace {
class Fd {
  public:
    int value = -1;
    explicit Fd( int descriptor = -1 ) : value( descriptor ) {}
    ~Fd() { if ( value >= 0 ) ::close( value ); }
    Fd( const Fd& ) = delete;
    Fd& operator=( const Fd& ) = delete;
};
FileIdentity fileIdentity( const struct stat& info )
{
    return { static_cast<quint64>( info.st_dev ), static_cast<quint64>( info.st_ino ), true };
}
using filesystemsecurity::ReadFlags;
using filesystemsecurity::accessIsSafe;
using filesystemsecurity::safeAncestor;

// fcntl's third argument depends on its command. Only expose the two socket
// operations used by the listener, preserving status flags for F_SETFL.
bool setCloseOnExec( int descriptor )
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return ::fcntl( descriptor, F_SETFD, FD_CLOEXEC ) == 0;
}
bool setNonBlocking( int descriptor )
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const auto flags = ::fcntl( descriptor, F_GETFL );
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return flags >= 0 && ::fcntl( descriptor, F_SETFL, flags | O_NONBLOCK ) == 0;
}
bool privateDirectory( const struct stat& info )
{
    return S_ISDIR( info.st_mode ) && info.st_uid == geteuid() && ( info.st_mode & 0077 ) == 0;
}
} // namespace
struct PrivateDirectory::Private {
    Fd fd;
    Fd published;
    QString path;
    FileIdentity identity;
};
PrivateDirectory::PrivateDirectory() : d_( std::make_unique<Private>() ) {}
PrivateDirectory::~PrivateDirectory() = default;
bool PrivateDirectory::open( const QString& directoryPath, bool create )
{
    if ( !QDir::isAbsolutePath( directoryPath ) || directoryPath.contains( QChar( 0 ) ) ) return false;
    auto parts = directoryPath.split( '/' );
    parts.removeAll( QString{} );
    if ( parts.isEmpty() ) return false;
    int current = filesystemsecurity::openRootDirectory();
    if ( current < 0 ) return false;
    Fd anchor( current );
    for ( const auto& part : parts ) {
        if ( !safeName( part ) ) return false;
        const auto name = QFile::encodeName( part );
        if ( create && mkdirat( anchor.value, name.constData(), 0700 ) != 0 && errno != EEXIST ) return false;
        const int next = filesystemsecurity::openReadOnlyAt( anchor.value, name.constData(), ReadFlags::Directory );
        if ( next < 0 ) return false;
        ::close( anchor.value );
        anchor.value = next;
        struct stat info{};
        if ( fstat( anchor.value, &info ) != 0 || !safeAncestor( anchor.value, info ) ) return false;
    }
    struct stat info{};
    if ( fstat( anchor.value, &info ) != 0 || !privateDirectory( info ) ) return false;
    d_->fd.value = std::exchange( anchor.value, -1 );
    d_->path = QDir::cleanPath( directoryPath );
    d_->identity = fileIdentity( info );
    return true;
}
QString PrivateDirectory::path() const { return d_->path; }
bool PrivateDirectory::unchanged() const
{
    struct stat info{};
    struct stat opened{};
    return d_->fd.value >= 0 && lstat( QFile::encodeName( d_->path ).constData(), &info ) == 0
           && fstat( d_->fd.value, &opened ) == 0 && privateDirectory( info )
           && accessIsSafe( d_->fd.value, privateDirectory( opened ) )
           && same( d_->identity, fileIdentity( info ) );
}
DirectoryEntries PrivateDirectory::entries( int limit ) const
{
    DirectoryEntries result;
    if ( !unchanged() ) return result;
    const int copy = dup( d_->fd.value );
    if ( copy < 0 ) return result;
    DIR* directory = fdopendir( copy );
    if ( !directory ) { ::close( copy ); return result; }
    rewinddir( directory );
    for ( int inspected = 0; inspected <= limit; ++inspected ) {
        errno = 0;
        const auto* entry = readdir( directory );
        if ( !entry ) { result.complete = errno == 0; break; }
        if ( inspected == limit ) break;
        const auto name = QFile::decodeName( entry->d_name );
        if ( name.endsWith( QStringLiteral( ".instance" ) ) ) result.names.push_back( name );
    }
    closedir( directory );
    return result;
}
std::optional<RegisteredFile> PrivateDirectory::read( const QString& name, int limit ) const
{
    if ( !safeName( name ) || !unchanged() ) return {};
    Fd file( filesystemsecurity::openReadOnlyAt( d_->fd.value, QFile::encodeName( name ).constData(),
                                                ReadFlags::NonBlocking ) );
    struct stat info{};
    if ( file.value < 0 || fstat( file.value, &info ) != 0 || !S_ISREG( info.st_mode )
         || info.st_uid != geteuid() || info.st_nlink != 1 || info.st_size <= 0 || info.st_size > limit
         || !accessIsSafe( file.value, ( info.st_mode & 0077 ) == 0 ) ) return {};
    QByteArray bytes( static_cast<int>( info.st_size ), '\0' );
    qint64 offset = 0;
    while ( offset < bytes.size() ) {
        const auto count = ::read( file.value, bytes.data() + offset, static_cast<size_t>( bytes.size() - offset ) );
        if ( count < 0 && errno == EINTR ) continue;
        if ( count <= 0 ) return {};
        offset += count;
    }
    char extra = 0;
    if ( ::read( file.value, &extra, 1 ) != 0 ) return {};
    const bool shared = ::flock( file.value, LOCK_SH | LOCK_NB ) == 0;
    if ( !shared && errno != EWOULDBLOCK ) return {};
    return RegisteredFile{ std::move( bytes ), fileIdentity( info ), !shared };
}
FileIdentity PrivateDirectory::identity( const QString& name ) const
{
    struct stat info{};
    if ( !safeName( name ) || !unchanged()
         || fstatat( d_->fd.value, QFile::encodeName( name ).constData(), &info, AT_SYMLINK_NOFOLLOW ) != 0 ) return {};
    return fileIdentity( info );
}
bool PrivateDirectory::publish( const QString& name, const RecordWriter& serialize, FileIdentity* identityOut )
{
    if ( !safeName( name ) || !unchanged() ) return false;
    const auto temporary = QFile::encodeName( QStringLiteral( "." ) + QUuid::createUuid().toString( QUuid::Id128 ) );
    Fd file( filesystemsecurity::createPrivateFileAt( d_->fd.value, temporary.constData() ) );
    if ( file.value < 0 ) return false;
    struct stat info{};
    bool okay = ::flock( file.value, LOCK_EX | LOCK_NB ) == 0 && fstat( file.value, &info ) == 0;
    const auto bytes = serialize( fileIdentity( info ) );
    qint64 offset = 0;
    while ( okay && offset < bytes.size() ) {
        const auto count = ::write( file.value, bytes.constData() + offset, static_cast<size_t>( bytes.size() - offset ) );
        if ( count < 0 && errno == EINTR ) continue;
        if ( count <= 0 ) { okay = false; break; }
        offset += count;
    }
    okay = okay && fsync( file.value ) == 0;
    // linkat is an atomic publish without overwriting an existing descriptor.
    okay = okay && linkat( d_->fd.value, temporary.constData(), d_->fd.value,
                          QFile::encodeName( name ).constData(), 0 ) == 0;
    unlinkat( d_->fd.value, temporary.constData(), 0 );
    if ( okay ) {
        d_->published.value = std::exchange( file.value, -1 );
        *identityOut = fileIdentity( info );
    }
    return okay;
}
void PrivateDirectory::removeOwned( const QString& name, FileIdentity expected )
{
    if ( same( expected, identity( name ) ) )
        unlinkat( d_->fd.value, QFile::encodeName( name ).constData(), 0 );
}
void PrivateDirectory::removeDirectory()
{
    if ( unchanged() ) rmdir( QFile::encodeName( d_->path ).constData() );
}
QString socketDirectory( const QString& socketRoot, const QString& instance )
{
    return QDir( socketRoot ).filePath( QStringLiteral( "kg-" ) + instance );
}
QString endpoint( const QString& socketRoot, const QString& instance )
{
    return socketDirectory( socketRoot, instance ) + QStringLiteral( "/s" );
}
bool privateEndpoint( const QString& name, const QString& instance ) // lint-allow: platform-fragile (external PAL; controltransport.cpp calls this)
{
    const QFileInfo file( name );
    if ( !QDir::isAbsolutePath( name ) || QDir::cleanPath( name ) != name
         || file.fileName() != QStringLiteral( "s" )
         || QFileInfo( file.path() ).fileName() != QStringLiteral( "kg-" ) + instance ) return false;
    PrivateDirectory directory;
    struct stat info{};
    return directory.open( file.path(), false )
           && lstat( QFile::encodeName( name ).constData(), &info ) == 0
           && S_ISSOCK( info.st_mode ) && info.st_uid == geteuid();
}
bool sameUserPeer( qintptr descriptor, bool serverSide ) // lint-allow: platform-fragile (external PAL; controltransport.cpp calls this)
{
    Q_UNUSED( serverSide );
#ifdef Q_OS_LINUX
    struct ucred credentials{};
    socklen_t length = sizeof( credentials );
    return getsockopt( static_cast<int>( descriptor ), SOL_SOCKET, SO_PEERCRED, &credentials, &length ) == 0
           && length == sizeof( credentials ) && credentials.uid == geteuid();
#else
    uid_t user = 0;
    gid_t group = 0;
    return getpeereid( static_cast<int>( descriptor ), &user, &group ) == 0 && user == geteuid();
#endif
}
qintptr createListener( const QString& name, FileIdentity* ownedSocket ) // lint-allow: platform-fragile (external PAL; controlruntime.cpp calls this)
{
    const auto path = QFile::encodeName( name );
    sockaddr_un address{};
    if ( path.size() >= static_cast<qint64>( sizeof( address.sun_path ) ) ) return -1;
    Fd listener( socket( AF_UNIX, SOCK_STREAM, 0 ) );
    if ( listener.value < 0 || !setCloseOnExec( listener.value ) ) return -1;
    address.sun_family = AF_UNIX;
    std::memcpy( address.sun_path, path.constData(), static_cast<size_t>( path.size() ) + 1 );
    if ( bind( listener.value, reinterpret_cast<sockaddr*>( &address ), sizeof( address ) ) != 0 ) return -1;
    struct stat info{};
    if ( lstat( path.constData(), &info ) != 0 || !S_ISSOCK( info.st_mode ) || info.st_uid != geteuid() ) return -1;
    // Claim cleanup ownership only after our bind succeeded, never on EADDRINUSE.
    *ownedSocket = fileIdentity( info );
    if ( ::listen( listener.value, 16 ) != 0 ) return -1;
    return std::exchange( listener.value, -1 );
}
void closeListener( qintptr descriptor ) // lint-allow: platform-fragile (external PAL; controlruntime.cpp calls this)
{
    if ( descriptor >= 0 ) ::close( static_cast<int>( descriptor ) );
}
struct LocalListener::Native {
    int fd = -1;
    std::unique_ptr<QSocketNotifier> notifier;
    std::deque<QLocalSocket*> pending;

    QLocalSocket* takePending()
    {
        if ( pending.empty() ) return nullptr;
        auto* socket = pending.front();
        pending.pop_front();
        return socket;
    }
};
LocalListener::LocalListener() : native_( std::make_unique<Native>() ) {}
LocalListener::~LocalListener() { close(); }
bool LocalListener::open( const QString&, qintptr listener )
{
    if ( listener < 0 ) return false;
    const auto descriptor = static_cast<int>( listener );
    if ( !setNonBlocking( descriptor ) ) return false;
    native_->fd = descriptor;
    native_->notifier = std::make_unique<QSocketNotifier>( listener, QSocketNotifier::Read );
    QObject::connect( native_->notifier.get(), &QSocketNotifier::activated, this, [this] {
        if ( native_->fd < 0 ) return;
        // One accept per readiness notification bounds work even under a flood.
        const int client = ::accept( native_->fd, nullptr, nullptr );
        if ( client < 0 ) return;
        if ( !setCloseOnExec( client ) ) { ::close( client ); return; }
        incomingConnection( static_cast<quintptr>( client ) );
    } );
    return true;
}
void LocalListener::close()
{
    if ( native_->notifier ) {
        native_->notifier->setEnabled( false );
        native_->notifier.reset();
    }
    if ( native_->fd >= 0 ) ::close( std::exchange( native_->fd, -1 ) );
    while ( auto* socket = native_->takePending() ) {
        socket->abort();
        socket->deleteLater();
    }
    QLocalServer::close();
}
bool LocalListener::hasPendingConnections() const { return !native_->pending.empty(); }
QLocalSocket* LocalListener::nextPendingConnection()
{
    return native_->takePending();
}
void LocalListener::incomingConnection( quintptr descriptor )
{
    if ( native_->pending.size() >= static_cast<size_t>( maxPendingConnections() ) ) {
        ::close( static_cast<int>( descriptor ) );
        return;
    }
    auto socket = std::make_unique<QLocalSocket>( this );
    if ( !socket->setSocketDescriptor( static_cast<qintptr>( descriptor ) ) ) {
        socket.reset();
        ::close( static_cast<int>( descriptor ) );
        return;
    }
    // The Qt parent owns the adopted socket; the pending queue only exposes it.
    native_->pending.push_back( socket.release() );
    Q_EMIT newConnection();
}
#endif

bool validEndpoint( const QString& value, const QString& socketRoot, const QString& instance )
{
    return !value.contains( QChar( 0 ) ) && value == endpoint( socketRoot, instance );
}
} // namespace klogg::control::pal
