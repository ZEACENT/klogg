#ifndef KLOGG_CONTROLFILESYSTEMSECURITY_P_H
#define KLOGG_CONTROLFILESYSTEMSECURITY_P_H

#include <QtGlobal>

#ifndef Q_OS_WIN
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#ifdef Q_OS_MACOS
#include <sys/acl.h>
#endif

namespace klogg::control::filesystemsecurity {

inline bool noExtendedGrants( int fd )
{
#ifdef Q_OS_MACOS
    // Darwin ACL allows bypass mode bits. Refuse existing grants; never repair
    // security by removing an ACL that belongs to the user.
    acl_t acl = acl_get_fd_np( fd, ACL_TYPE_EXTENDED );
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
    Q_UNUSED( fd );
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
