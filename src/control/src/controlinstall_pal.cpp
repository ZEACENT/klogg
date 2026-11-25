#include "controlinstall_pal.h"
#include "controlfilesystemsecurity_p.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <utility>
#include <vector>

#include "qtcontainerindex.h"

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace klogg::control::installpal {
#ifndef Q_OS_WIN
class FileDescriptor {
  public:
    explicit FileDescriptor( int value = -1 ) : value_( value ) {}
    ~FileDescriptor() { reset(); }
    FileDescriptor( const FileDescriptor& ) = delete;
    FileDescriptor& operator=( const FileDescriptor& ) = delete;
    FileDescriptor( FileDescriptor&& other ) noexcept : value_( std::exchange( other.value_, -1 ) ) {}
    FileDescriptor& operator=( FileDescriptor&& ) = delete;
    int get() const { return value_; }
    bool sameObject( const FileDescriptor& other ) const
    {
        struct stat first {};
        struct stat second {};
        return ::fstat( value_, &first ) == 0 && ::fstat( other.value_, &second ) == 0
               && first.st_dev == second.st_dev && first.st_ino == second.st_ino;
    }
    void reset( int value = -1 )
    {
        if ( value_ >= 0 ) ::close( value_ );
        value_ = value;
    }
  private:
    int value_;
};
#endif

namespace {
constexpr qint64 MaxConfigBytes = 4LL * 1024 * 1024;
#ifndef Q_OS_WIN
using filesystemsecurity::ReadFlags;

bool trustedAncestor( int descriptor )
{
    struct stat info {};
    return ::fstat( descriptor, &info ) == 0 && filesystemsecurity::safeAncestor( descriptor, info );
}
bool ownedDirectory( int descriptor )
{
    struct stat info {};
    return ::fstat( descriptor, &info ) == 0 && info.st_uid == ::geteuid()
           && filesystemsecurity::safeAncestor( descriptor, info );
}
InstallStatus openParent( const QString& path, FileDescriptor& parent, bool create, bool& touched )
{
    if ( !QDir::isAbsolutePath( path ) || QDir::cleanPath( path ) != path ) return InstallStatus::UnsafePath;
    auto parts = path.split( '/' );
    parts.removeAll( QString{} );
    if ( parts.isEmpty() ) return InstallStatus::UnsafePath;
    parent.reset( filesystemsecurity::openRootDirectory() );
    if ( parent.get() < 0 ) return InstallStatus::IoError;
    if ( !trustedAncestor( parent.get() ) ) return InstallStatus::UnsafePath;
    for ( ContainerIndex i = 0; i + 1 < parts.size(); ++i ) {
        const auto name = QFile::encodeName( parts.at( i ) );
        int next = filesystemsecurity::openReadOnlyAt( parent.get(), name.constData(), ReadFlags::Directory );
        if ( next < 0 && errno == ENOENT ) {
            if ( !ownedDirectory( parent.get() ) ) return InstallStatus::UnsafePath;
            if ( !create ) { parent.reset(); return InstallStatus::Available; }
            if ( ::mkdirat( parent.get(), name.constData(), 0700 ) != 0 ) return InstallStatus::ConcurrentChange;
            touched = true;
            next = filesystemsecurity::openReadOnlyAt( parent.get(), name.constData(), ReadFlags::Directory );
        }
        if ( next < 0 ) return ( errno == ELOOP || errno == ENOTDIR ) ? InstallStatus::UnsafePath : InstallStatus::IoError;
        parent.reset( next );
        if ( !trustedAncestor( parent.get() ) ) return InstallStatus::UnsafePath;
    }
    return ownedDirectory( parent.get() ) ? InstallStatus::Available : InstallStatus::UnsafePath;
}
qint64 nanos( const struct stat& info )
{
#ifdef Q_OS_DARWIN
    return info.st_mtimespec.tv_nsec;
#else
    return info.st_mtim.tv_nsec;
#endif
}
InstallStatus readAt( int parent, const QByteArray& name, FileState& state )
{
    state = {};
    FileDescriptor file( filesystemsecurity::openReadOnlyAt( parent, name.constData(), ReadFlags::NonBlocking ) );
    if ( file.get() < 0 ) {
        if ( errno == ENOENT ) return InstallStatus::Available;
        return errno == ELOOP ? InstallStatus::UnsafePath : InstallStatus::IoError;
    }
    struct stat before {};
    if ( ::fstat( file.get(), &before ) != 0 ) return InstallStatus::IoError;
    if ( !S_ISREG( before.st_mode ) || before.st_uid != ::geteuid() || before.st_nlink != 1
         || !filesystemsecurity::accessIsSafe( file.get(), ( before.st_mode & ( S_IWGRP | S_IWOTH ) ) == 0 )
         || before.st_size < 0 || before.st_size > MaxConfigBytes ) return InstallStatus::UnsafePath;
    state.exists = true;
    state.mode = static_cast<unsigned>( before.st_mode & 0777 );
    state.modifiedSeconds = before.st_mtime;
    state.modifiedNanoseconds = nanos( before );
    char buffer[8192];
    while ( true ) {
        const auto size = ::read( file.get(), buffer, sizeof( buffer ) );
        if ( size < 0 && errno == EINTR ) continue;
        if ( size < 0 ) return InstallStatus::IoError;
        if ( size == 0 ) break;
        state.bytes.append( buffer, static_cast<ContainerIndex>( size ) );
        if ( state.bytes.size() > MaxConfigBytes ) return InstallStatus::UnsafePath;
    }
    struct stat after {};
    if ( ::fstat( file.get(), &after ) != 0 ) return InstallStatus::IoError;
    if ( after.st_size != before.st_size || state.bytes.size() != before.st_size
         || after.st_mtime != before.st_mtime || nanos( after ) != nanos( before ) ) return InstallStatus::ConcurrentChange;
    state.observed = std::make_shared<FileDescriptor>( std::move( file ) );
    return InstallStatus::Available;
}
bool sameFile( const FileState& first, const FileState& second )
{
    return first.exists == second.exists && ( !first.exists
        || ( first.observed && second.observed && first.observed->sameObject( *second.observed )
             && first.mode == second.mode && first.modifiedSeconds == second.modifiedSeconds
             && first.modifiedNanoseconds == second.modifiedNanoseconds && first.bytes == second.bytes ) );
}
bool createFile( int parent, const QByteArray& name, const QByteArray& bytes, unsigned mode )
{
    FileDescriptor file( filesystemsecurity::createPrivateFileAt( parent, name.constData() ) );
    if ( file.get() < 0 ) return false;
    ContainerIndex offset = 0;
    while ( offset < bytes.size() ) {
        const auto written = ::write( file.get(), bytes.constData() + offset, static_cast<size_t>( bytes.size() - offset ) );
        if ( written < 0 && errno == EINTR ) continue;
        if ( written <= 0 ) { ::unlinkat( parent, name.constData(), 0 ); return false; }
        offset += static_cast<ContainerIndex>( written );
    }
    if ( ::fchmod( file.get(), static_cast<mode_t>( mode & 0777U ) ) != 0 || ::fsync( file.get() ) != 0 ) {
        ::unlinkat( parent, name.constData(), 0 );
        return false;
    }
    return true;
}
#else
class Key {
  public:
    HKEY value = nullptr;
    ~Key() { if ( value ) ::RegCloseKey( value ); }
};
bool readValue( const wchar_t* keyPath, const wchar_t* name, bool& exists, DWORD& type, QString& value )
{
    Key key;
    const auto opened = ::RegOpenKeyExW( HKEY_CURRENT_USER, keyPath, 0, KEY_QUERY_VALUE, &key.value );
    if ( opened == ERROR_FILE_NOT_FOUND ) { exists = false; return true; }
    if ( opened != ERROR_SUCCESS ) return false;
    DWORD size = 0;
    auto status = ::RegQueryValueExW( key.value, name, nullptr, &type, nullptr, &size );
    if ( status == ERROR_FILE_NOT_FOUND ) { exists = false; return true; }
    if ( status != ERROR_SUCCESS || ( type != REG_SZ && type != REG_EXPAND_SZ )
         || size < sizeof( wchar_t ) || size > MaxConfigBytes || size % sizeof( wchar_t ) != 0 ) return false;
    std::vector<wchar_t> buffer( static_cast<size_t>( size ) / sizeof( wchar_t ) );
    status = ::RegQueryValueExW( key.value, name, nullptr, &type,
                               reinterpret_cast<BYTE*>( buffer.data() ), &size );
    if ( status != ERROR_SUCCESS || size / sizeof( wchar_t ) != buffer.size()
         || buffer.back() != L'\0' || ( type != REG_SZ && type != REG_EXPAND_SZ ) ) return false;
    value = QString::fromWCharArray( buffer.data(), static_cast<int>( buffer.size() - 1 ) );
    if ( value.contains( QChar( 0 ) ) ) return false;
    exists = true;
    return true;
}
std::optional<UserPathState> readUserPath()
{
    UserPathState state;
    DWORD type = REG_SZ;
    if ( !readValue( L"Environment", L"Path", state.exists, type, state.value ) ) return {};
    state.expandable = type == REG_EXPAND_SZ;
    bool ownershipExists = false;
    type = REG_SZ;
    if ( !readValue( L"Software\\klogg\\ShellIntegration", L"OwnedPath", ownershipExists, type, state.ownedDirectory )
         || ( ownershipExists && type != REG_SZ ) ) return {};
    return state;
}
bool writeValue( const wchar_t* keyPath, const wchar_t* name, bool exists, DWORD type, const QString& value )
{
    Key key;
    if ( ::RegCreateKeyExW( HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_SET_VALUE,
                            nullptr, &key.value, nullptr ) != ERROR_SUCCESS ) return false;
    if ( !exists ) {
        const auto status = ::RegDeleteValueW( key.value, name );
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    if ( value.size() > MaxConfigBytes / 2 - 1 ) return false;
    return ::RegSetValueExW( key.value, name, 0, type, reinterpret_cast<const BYTE*>( value.utf16() ),
                            static_cast<DWORD>( ( static_cast<quint64>( value.size() ) + 1U ) * sizeof( wchar_t ) ) ) == ERROR_SUCCESS;
}
UserPathMutation replaceUserPath( const UserPathState& expected, const UserPathState& desired )
{
    const auto matches = [&expected]( const std::optional<UserPathState>& value ) {
        return value && value->exists == expected.exists && value->expandable == expected.expandable
               && value->value == expected.value && value->ownedDirectory == expected.ownedDirectory;
    };
    if ( !matches( readUserPath() ) ) return { InstallStatus::ConcurrentChange, {} };
    UserPathMutation result;
    const auto backup = QStringLiteral( R"(Software\klogg\ShellIntegration\Backups\)" )
                        + QUuid::createUuid().toString( QUuid::WithoutBraces );
    const auto* backupKey = reinterpret_cast<LPCWSTR>( backup.utf16() );
    Key created;
    DWORD disposition = 0;
    if ( ::RegCreateKeyExW( HKEY_CURRENT_USER, backupKey, 0, nullptr, 0, KEY_SET_VALUE,
                            nullptr, &created.value, &disposition ) != ERROR_SUCCESS
         || disposition != REG_CREATED_NEW_KEY ) return result;
    result.backupPath = QStringLiteral( "HKCU\\" ) + backup;
    if ( !writeValue( backupKey, L"Path", true, expected.expandable ? REG_EXPAND_SZ : REG_SZ, expected.value )
         || !writeValue( backupKey, L"PathExisted", true, REG_SZ, expected.exists ? "1" : "0" )
         || !writeValue( backupKey, L"OwnedPath", true, REG_SZ, expected.ownedDirectory ) ) {
        result.status = InstallStatus::PartialFailure;
        return result;
    }
    if ( !matches( readUserPath() ) ) { result.status = InstallStatus::ConcurrentChange; return result; }
    const bool changePath = desired.exists != expected.exists || desired.value != expected.value
                            || desired.expandable != expected.expandable;
    if ( changePath && !writeValue( L"Environment", L"Path", desired.exists,
                                   desired.expandable ? REG_EXPAND_SZ : REG_SZ, desired.value ) ) return result;
    if ( desired.ownedDirectory != expected.ownedDirectory
         && !writeValue( L"Software\\klogg\\ShellIntegration", L"OwnedPath", !desired.ownedDirectory.isEmpty(),
                         REG_SZ, desired.ownedDirectory ) ) {
        result.status = changePath ? InstallStatus::PartialFailure : InstallStatus::IoError;
        return result;
    }
    result.status = InstallStatus::Installed;
    return result;
}
#endif
} // namespace

InstallStatus readFile( const QString& path, FileState& state )
{
#ifndef Q_OS_WIN
    FileDescriptor parent;
    bool touched = false;
    const auto status = openParent( path, parent, false, touched );
    if ( status != InstallStatus::Available ) return status;
    if ( parent.get() < 0 ) { state = {}; return InstallStatus::Available; }
    return readAt( parent.get(), QFile::encodeName( QFileInfo( path ).fileName() ), state );
#else
    Q_UNUSED( path );
    Q_UNUSED( state );
    return InstallStatus::Unsupported;
#endif
}

Mutation replaceFile( const QString& path, const FileState& expected,
                      const std::optional<QByteArray>& bytes, unsigned mode )
{
    Mutation result;
#ifndef Q_OS_WIN
    FileDescriptor parent;
    result.status = openParent( path, parent, true, result.touched );
    if ( result.status != InstallStatus::Available ) return result;
    const auto name = QFile::encodeName( QFileInfo( path ).fileName() );
    FileState current;
    result.status = readAt( parent.get(), name, current );
    if ( result.status != InstallStatus::Available ) return result;
    if ( !sameFile( expected, current ) ) { result.status = InstallStatus::ConcurrentChange; return result; }
    result.status = InstallStatus::IoError;
    if ( expected.exists ) {
        const auto backup = name + ".kloggctl-backup-" + QUuid::createUuid().toByteArray( QUuid::WithoutBraces );
        if ( !createFile( parent.get(), backup, expected.bytes, 0600 ) ) return result;
        result.backup = QFileInfo( path ).absolutePath() + '/' + QFile::decodeName( backup );
        result.touched = true;
    }
    const auto temporary = QByteArray( ".kloggctl-write-" ) + QUuid::createUuid().toByteArray( QUuid::WithoutBraces );
    if ( bytes && !createFile( parent.get(), temporary, *bytes, mode ) ) return result;
    const auto check = readAt( parent.get(), name, current );
    if ( check != InstallStatus::Available || !sameFile( expected, current ) ) {
        if ( bytes ) ::unlinkat( parent.get(), temporary.constData(), 0 );
        result.status = check == InstallStatus::Available ? InstallStatus::ConcurrentChange : check;
        return result;
    }
    int updated = 0;
    if ( !bytes ) updated = ::unlinkat( parent.get(), name.constData(), 0 );
    else if ( expected.exists ) updated = ::renameat( parent.get(), temporary.constData(), parent.get(), name.constData() );
    else {
        // Atomic no-clobber publication for a previously absent destination.
        updated = ::linkat( parent.get(), temporary.constData(), parent.get(), name.constData(), 0 );
    }
    if ( bytes ) ::unlinkat( parent.get(), temporary.constData(), 0 );
    if ( updated != 0 ) return result;
    result.touched = true;
    result.status = InstallStatus::Installed;
#else
    Q_UNUSED( path ); Q_UNUSED( expected ); Q_UNUSED( bytes ); Q_UNUSED( mode );
    result.status = InstallStatus::Unsupported;
#endif
    return result;
}

bool executableIsSafe( const QString& path )
{
    if ( !QFileInfo( path ).isFile() || !QFileInfo( path ).isExecutable() ) return false;
#ifndef Q_OS_WIN
    // Validate every component without following a symlink. Executables may be
    // system-owned; only writable destinations require current-user ownership.
    auto parts = path.split( '/' );
    parts.removeAll( QString{} );
    if ( parts.isEmpty() || !QDir::isAbsolutePath( path ) ) return false;
    FileDescriptor parent( filesystemsecurity::openRootDirectory() );
    if ( parent.get() < 0 ) return false;
    for ( ContainerIndex i = 0; i < parts.size(); ++i ) {
        const auto name = QFile::encodeName( parts.at( i ) );
        const bool last = i + 1 == parts.size();
        const auto next = filesystemsecurity::openReadOnlyAt( parent.get(), name.constData(),
            last ? ReadFlags::NonBlocking : ReadFlags::NonBlockingDirectory );
        if ( next < 0 ) return false;
        parent.reset( next );
    }
    struct stat info {};
    return ::fstat( parent.get(), &info ) == 0 && S_ISREG( info.st_mode );
#else
    auto current = path;
    while ( true ) {
        const auto native = QDir::toNativeSeparators( current );
        const auto attributes = ::GetFileAttributesW( reinterpret_cast<LPCWSTR>( native.utf16() ) );
        if ( attributes == INVALID_FILE_ATTRIBUTES || ( attributes & FILE_ATTRIBUTE_REPARSE_POINT ) ) return false;
        const auto parent = QFileInfo( current ).absolutePath();
        if ( parent == current ) break;
        current = parent;
    }
    DWORD type = 0;
    return ::GetBinaryTypeW( reinterpret_cast<LPCWSTR>( QDir::toNativeSeparators( path ).utf16() ), &type ) != 0;
#endif
}

UserPathAccess nativeUserPath()
{
#ifdef Q_OS_WIN
    return { readUserPath, replaceUserPath };
#else
    return {};
#endif
}
std::optional<InstallShell> defaultShell( const QProcessEnvironment& environment )
{
#ifdef Q_OS_WIN
    Q_UNUSED( environment );
    return InstallShell::Windows;
#else
    const auto shell = QFileInfo( environment.value( "SHELL" ) ).fileName();
    if ( shell == "zsh" ) return InstallShell::Zsh;
    if ( shell == "bash" ) return InstallShell::Bash;
    if ( shell == "fish" ) return InstallShell::Fish;
    return {};
#endif
}
} // namespace klogg::control::installpal
