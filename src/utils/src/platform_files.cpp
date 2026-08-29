#include "platform/platform_files.h"

#include <cstdint>
#include <vector>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QtGlobal>

#if defined( Q_OS_WIN )
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace klogg::platform {
namespace {

#if defined( Q_OS_WIN )

class NativeHandle final {
public:
    explicit NativeHandle( HANDLE handle )
        : handle_( handle )
    {
    }

    ~NativeHandle()
    {
        if ( handle_ != INVALID_HANDLE_VALUE ) {
            CloseHandle( handle_ );
        }
    }

    NativeHandle( const NativeHandle& ) = delete;
    NativeHandle& operator=( const NativeHandle& ) = delete;

    NativeHandle( NativeHandle&& other ) noexcept
        : handle_( other.handle_ )
    {
        other.handle_ = INVALID_HANDLE_VALUE;
    }

    NativeHandle& operator=( NativeHandle&& other ) noexcept
    {
        if ( this != &other ) {
            if ( handle_ != INVALID_HANDLE_VALUE ) {
                CloseHandle( handle_ );
            }
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HANDLE get() const
    {
        return handle_;
    }

    explicit operator bool() const
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_{ INVALID_HANDLE_VALUE };
};

std::vector<std::uint8_t> currentUserToken()
{
    HANDLE rawToken = nullptr;
    if ( !OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &rawToken ) ) {
        return {};
    }
    NativeHandle token( rawToken );

    DWORD requiredBytes = 0;
    GetTokenInformation( token.get(), TokenUser, nullptr, 0, &requiredBytes );
    if ( requiredBytes == 0 ) {
        return {};
    }

    std::vector<std::uint8_t> storage( requiredBytes );
    if ( !GetTokenInformation( token.get(), TokenUser, storage.data(), requiredBytes,
                               &requiredBytes ) ) {
        return {};
    }
    return storage;
}

PSID tokenUserSid( std::vector<std::uint8_t>& token )
{
    if ( token.empty() ) {
        return nullptr;
    }
    return reinterpret_cast<TOKEN_USER*>( token.data() )->User.Sid;
}

PACL ownerOnlyAcl( PSID owner, bool directory )
{
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>( owner );

    PACL acl = nullptr;
    if ( SetEntriesInAclW( 1, &access, nullptr, &acl ) != ERROR_SUCCESS ) {
        return nullptr;
    }
    return acl;
}

NativeHandle openObject( const QString& path, bool directory, DWORD access )
{
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT
                        | ( directory ? FILE_FLAG_BACKUP_SEMANTICS : 0 );
    return NativeHandle( CreateFileW(
        reinterpret_cast<LPCWSTR>( path.utf16() ), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags,
        nullptr ) );
}

bool objectMatchesKind( HANDLE handle, bool directory )
{
    BY_HANDLE_FILE_INFORMATION info{};
    if ( !GetFileInformationByHandle( handle, &info ) ) {
        return false;
    }
    if ( ( info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ) != 0 ) {
        return false;
    }
    const bool isDirectory = ( info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0;
    return isDirectory == directory;
}

bool ownerMatchesCurrentUser( HANDLE handle, PSID currentUser )
{
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto result = GetSecurityInfo( handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                         &owner, nullptr, nullptr, nullptr, &descriptor );
    const bool matches
        = result == ERROR_SUCCESS && owner != nullptr && EqualSid( owner, currentUser );
    if ( descriptor != nullptr ) {
        LocalFree( descriptor );
    }
    return matches;
}

bool handleHasOwnerOnlyAccess( HANDLE handle, PSID currentUser, bool directory )
{
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto result = GetSecurityInfo(
        handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor );
    if ( result != ERROR_SUCCESS || descriptor == nullptr || owner == nullptr || dacl == nullptr
         || !EqualSid( owner, currentUser ) ) {
        if ( descriptor != nullptr ) {
            LocalFree( descriptor );
        }
        return false;
    }

    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    const bool protectedDacl
        = GetSecurityDescriptorControl( descriptor, &control, &revision )
          && ( control & SE_DACL_PROTECTED ) != 0;
    bool ownerOnly = protectedDacl && dacl->AceCount == 1;
    if ( ownerOnly ) {
        void* rawAce = nullptr;
        ownerOnly = GetAce( dacl, 0, &rawAce ) != FALSE && rawAce != nullptr;
        if ( ownerOnly ) {
            const auto* header = static_cast<ACE_HEADER*>( rawAce );
            ownerOnly = header->AceType == ACCESS_ALLOWED_ACE_TYPE;
            if ( ownerOnly ) {
                const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>( rawAce );
                ownerOnly = EqualSid( const_cast<DWORD*>( &ace->SidStart ), currentUser );
                if ( directory ) {
                    const auto inheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
                    ownerOnly = ownerOnly && ( header->AceFlags & inheritance ) == inheritance;
                }
            }
        }
    }

    LocalFree( descriptor );
    return ownerOnly;
}

bool secureHandle( HANDLE handle, PSID currentUser, bool directory )
{
    if ( !objectMatchesKind( handle, directory )
         || !ownerMatchesCurrentUser( handle, currentUser ) ) {
        return false;
    }

    auto* acl = ownerOnlyAcl( currentUser, directory );
    if ( acl == nullptr ) {
        return false;
    }
    const auto result = SetSecurityInfo(
        handle, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl,
        nullptr );
    LocalFree( acl );
    return result == ERROR_SUCCESS
           && handleHasOwnerOnlyAccess( handle, currentUser, directory );
}

#else

bool securePosixObject( const QString& path, bool directory )
{
    const auto encodedPath = QFile::encodeName( path );
    const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | ( directory ? O_DIRECTORY : 0 );
    // POSIX open() is variadic only for O_CREAT; this call never creates files.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const int descriptor = ::open( encodedPath.constData(), flags );
    if ( descriptor < 0 ) {
        return false;
    }

    struct stat status{};
    const mode_t mode = directory ? 0700 : 0600;
    const bool secured = ::fstat( descriptor, &status ) == 0
                         && ( directory ? S_ISDIR( status.st_mode ) : S_ISREG( status.st_mode ) )
                         && status.st_uid == ::geteuid() && ::fchmod( descriptor, mode ) == 0;
    ::close( descriptor );
    return secured;
}

#endif

} // namespace

bool ensureOwnerOnlyDirectory( const QString& path )
{
#if defined( Q_OS_WIN )
    auto token = currentUserToken();
    auto* currentUser = tokenUserSid( token );
    if ( currentUser == nullptr ) {
        return false;
    }

    auto directory = openObject( path, true, READ_CONTROL | WRITE_DAC );
    if ( !directory ) {
        auto* acl = ownerOnlyAcl( currentUser, true );
        if ( acl == nullptr ) {
            return false;
        }
        SECURITY_DESCRIPTOR descriptor{};
        const bool descriptorReady
            = InitializeSecurityDescriptor( &descriptor, SECURITY_DESCRIPTOR_REVISION )
              && SetSecurityDescriptorDacl( &descriptor, TRUE, acl, FALSE )
              && SetSecurityDescriptorControl( &descriptor, SE_DACL_PROTECTED,
                                                SE_DACL_PROTECTED );
        SECURITY_ATTRIBUTES attributes{ sizeof( SECURITY_ATTRIBUTES ), &descriptor, FALSE };
        const bool created = descriptorReady
                             && CreateDirectoryW(
                                 reinterpret_cast<LPCWSTR>( path.utf16() ), &attributes );
        const auto createError = GetLastError();
        LocalFree( acl );
        if ( !created && createError != ERROR_ALREADY_EXISTS ) {
            return false;
        }
        directory = openObject( path, true, READ_CONTROL | WRITE_DAC );
    }
    return directory && secureHandle( directory.get(), currentUser, true );
#else
    const auto encodedPath = QFile::encodeName( path );
    if ( ::mkdir( encodedPath.constData(), 0700 ) != 0 && errno != EEXIST ) {
        return false;
    }
    return securePosixObject( path, true );
#endif
}

bool restrictRegularFileToOwner( const QString& path )
{
#if defined( Q_OS_WIN )
    auto token = currentUserToken();
    auto* currentUser = tokenUserSid( token );
    if ( currentUser == nullptr ) {
        return false;
    }
    auto file = openObject( path, false, READ_CONTROL | WRITE_DAC );
    return file && secureHandle( file.get(), currentUser, false );
#else
    return securePosixObject( path, false );
#endif
}

bool ownerOnlyAccessIsEnforced( const QString& path )
{
#if defined( Q_OS_WIN )
    const QFileInfo info( path );
    if ( !info.exists() ) {
        return false;
    }
    const bool directory = info.isDir();
    auto token = currentUserToken();
    auto* currentUser = tokenUserSid( token );
    if ( currentUser == nullptr ) {
        return false;
    }
    auto object = openObject( path, directory, READ_CONTROL );
    return object && objectMatchesKind( object.get(), directory )
           && handleHasOwnerOnlyAccess( object.get(), currentUser, directory );
#else
    const auto encodedPath = QFile::encodeName( path );
    struct stat status{};
    if ( ::lstat( encodedPath.constData(), &status ) != 0 || status.st_uid != ::geteuid() ) {
        return false;
    }
    if ( S_ISDIR( status.st_mode ) ) {
        return ( status.st_mode & 0777 ) == 0700;
    }
    if ( S_ISREG( status.st_mode ) ) {
        return ( status.st_mode & 0777 ) == 0600;
    }
    return false;
#endif
}

} // namespace klogg::platform
