#ifndef KLOGG_CONTROLFILESYSTEMSECURITY_P_H
#define KLOGG_CONTROLFILESYSTEMSECURITY_P_H

#include <QtGlobal>

#ifndef Q_OS_WIN
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef Q_OS_MACOS
#include <sys/acl.h>
#endif

namespace klogg::control::filesystemsecurity {

// Keep POSIX's variadic open/openat behind fixed-arity, no-follow operations.
inline int openRootDirectory()
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return ::open( "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC );
}

enum class ReadFlags : int {
    Directory = O_DIRECTORY,
    NonBlocking = O_NONBLOCK,
    NonBlockingDirectory = O_NONBLOCK | O_DIRECTORY
};
inline int openReadOnlyAt( int parent, const char* name, ReadFlags flags )
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return ::openat( parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | static_cast<int>( flags ) );
}

inline int createPrivateFileAt( int parent, const char* name )
{
    // O_CREAT requires the mode argument; no existing file may be opened.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return ::openat( parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600 );
}

inline bool noExtendedGrants( int descriptor )
{
#ifdef Q_OS_MACOS
    // Darwin ACL allows bypass mode bits. Refuse existing grants; never repair
    // security by removing an ACL that belongs to the user.
    acl_t acl = acl_get_fd_np( descriptor, ACL_TYPE_EXTENDED );
    if ( !acl ) return errno == ENOENT;
    acl_entry_t entry = nullptr;
    bool safe = true;
    for ( int position = ACL_FIRST_ENTRY; acl_get_entry( acl, position, &entry ) == 0;
          position = ACL_NEXT_ENTRY ) {
        acl_tag_t tag{};
        if ( acl_get_tag_type( entry, &tag ) != 0 || tag != ACL_EXTENDED_DENY ) {
            safe = false;
            break;
        }
    }
    acl_free( acl );
    return safe;
#else
    // POSIX ACL effective permissions are masked by group mode bits. Callers
    // reject group/other writes; private transport objects reject all such access.
    Q_UNUSED( descriptor );
    return true;
#endif
}

inline bool safeAncestor( const struct stat& info )
{
    return S_ISDIR( info.st_mode ) && ( info.st_uid == geteuid() || info.st_uid == 0 )
           && ( ( info.st_mode & 0022 ) == 0
                || ( info.st_uid == 0 && ( info.st_mode & S_ISVTX ) != 0 ) );
}

} // namespace klogg::control::filesystemsecurity
#endif
#endif
