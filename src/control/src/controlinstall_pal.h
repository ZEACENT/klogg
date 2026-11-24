#ifndef KLOGG_CONTROLINSTALL_PAL_H
#define KLOGG_CONTROLINSTALL_PAL_H

#include <QByteArray>
#include <memory>

#include "controlinstall.h"

namespace klogg::control::installpal {

class FileDescriptor;
struct FileState {
    bool exists = false;
    QByteArray bytes;
    // Keep the observed object alive through the last identity recheck. An inode
    // number alone can be recycled between unlink and an identical recreation.
    std::shared_ptr<const FileDescriptor> observed;
    unsigned mode = 0600;
    qint64 modifiedSeconds = 0;
    qint64 modifiedNanoseconds = 0;
};
struct Mutation {
    InstallStatus status = InstallStatus::IoError;
    QString backup;
    bool touched = false;
};

// Paths must be absolute. Refuse symlinks/reparse points, foreign-owned leaves,
// and oversized/non-regular files. Missing leaves/parents are an empty state.
InstallStatus readFile( const QString& path, FileState& state );
Mutation replaceFile( const QString& path, const FileState& expected,
                      const std::optional<QByteArray>& bytes, unsigned mode );
bool executableIsSafe( const QString& path );
UserPathAccess nativeUserPath();
std::optional<InstallShell> defaultShell( const QProcessEnvironment& environment );

} // namespace klogg::control::installpal
#endif
