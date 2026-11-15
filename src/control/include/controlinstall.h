#ifndef KLOGG_CONTROLINSTALL_H
#define KLOGG_CONTROLINSTALL_H

#include <functional>
#include <optional>

#include <QProcessEnvironment>
#include <QStringList>

namespace klogg::control {

enum class InstallShell { Zsh, Bash, Fish, Windows };
enum class InstallAction { Check, Install, Remove, CheckRemoval };
enum class InstallStatus {
    Available, Installed, NotInstalled, NeedsRepair, Removed,
    InvalidInput, UnsafePath, Collision, ConcurrentChange, IoError,
    Unsupported, PartialFailure
};

struct InstallOptions {
    QString executable;
    QString home;
    InstallShell shell = InstallShell::Bash;
    QProcessEnvironment environment;
    // Empty selects home/.local/bin. Unix only.
    QString binDirectory;
    // Explicit custom startup files override shell-specific defaults. Unix only.
    QStringList configFiles;
};

struct InstallResult {
    InstallStatus status = InstallStatus::InvalidInput;
    QString message;
    QString executable;
    QString commandPath;
    QString pathDirectory;
    QStringList configFiles;
    QStringList plannedPaths;
    QStringList changedPaths;
    QStringList backupPaths;
    bool restartRequired = false;
};

// The Windows store is a narrow test seam. Production uses only HKCU and keeps
// the unexpanded PATH text and its native string type. Tests must inject a fake.
struct UserPathState {
    bool exists = false;
    bool expandable = false;
    QString value;
    QString ownedDirectory;
};
struct UserPathMutation {
    InstallStatus status = InstallStatus::IoError;
    QString backupPath;
};
struct UserPathAccess {
    std::function<std::optional<UserPathState>()> read;
    // Compare before writing. Return Installed on success, or a typed failure;
    // native registry writes are not a cross-value transaction.
    std::function<UserPathMutation( const UserPathState& expected, const UserPathState& desired )> replace;
};

// Resolves defaults only; never writes or executes a shell. The caller supplies
// the bundled console executable, not the GUI executable or an AppImage mount.
std::optional<InstallOptions> defaultInstallOptions( const QString& executable );

// Check and CheckRemoval are read-only previews. CheckRemoval reports Installed
// when owned content is removable, NotInstalled when nothing is owned.
// Install also repairs owned content. Remove touches only managed entries.
// Call only after an explicit user action for Install/Remove. If approvedPaths
// is supplied for mutation, reject a changed target list before any writes.
// File replacements are atomic individually, backed up, and optimistically
// checked for concurrent edits; there is no cross-file ACID or exclusion of
// noncooperating editors racing the final native replacement.
InstallResult configureControlInstallation( const InstallOptions& options, InstallAction action,
                                            const UserPathAccess* userPath = nullptr,
                                            const QStringList* approvedPaths = nullptr );

} // namespace klogg::control
#endif
