#include "controlinstall.h"
#include "controlinstall_pal.h"

#include <algorithm>
#include <vector>

#include <QDir>
#include <QFileInfo>

#include "qtcontainerindex.h"

namespace klogg::control {
namespace {
const QByteArray Begin = "# >>> kloggctl shell integration v1 >>>";
const QByteArray End = "# <<< kloggctl shell integration v1 <<<";
const QByteArray ShimHeader = "#!/bin/sh\n# kloggctl managed launcher v1\n# target-base64: ";

bool safeText( const QString& text )
{
    return !text.isEmpty() && std::none_of( text.cbegin(), text.cend(), []( QChar character ) {
        return character.unicode() < 32 || character.unicode() == 127;
    } );
}
bool absolutePath( const QString& path )
{
    return safeText( path ) && QDir::isAbsolutePath( path );
}
QString quote( QString value, InstallShell shell )
{
    if ( shell == InstallShell::Fish ) {
        value.replace( '\\', "\\\\" );
        value.replace( '\'', "\\'" );
    }
    else value.replace( '\'', "'\\''" );
    return '\'' + value + '\'';
}
QByteArray launcher( const QString& executable )
{
    return ShimHeader + executable.toUtf8().toBase64() + "\nexec "
           + quote( executable, InstallShell::Bash ).toUtf8() + " \"$@\"\n";
}
std::optional<QString> ownedLauncher( const QByteArray& bytes )
{
    if ( !bytes.startsWith( ShimHeader ) ) return {};
    const auto end = bytes.indexOf( '\n', ShimHeader.size() );
    if ( end < 0 ) return {};
    const auto encoded = bytes.mid( ShimHeader.size(), end - ShimHeader.size() );
    const auto decoded = QByteArray::fromBase64( encoded );
    auto target = QString::fromUtf8( decoded );
    if ( decoded.toBase64() != encoded || target.toUtf8() != decoded || !absolutePath( target )
         || launcher( target ) != bytes ) return {};
    return target;
}

struct Block {
    bool present = false;
    bool separator = false;
    ContainerIndex start = 0;
    ContainerIndex end = 0;
    QByteArray newline = "\n";
};
std::optional<Block> findBlock( const QByteArray& bytes )
{
    if ( bytes.contains( '\0' ) ) return {};
    Block block;
    const auto firstNewline = bytes.indexOf( '\n' );
    if ( firstNewline > 0 && bytes.at( firstNewline - 1 ) == '\r' ) block.newline = "\r\n";
    const auto start = bytes.indexOf( Begin );
    if ( start < 0 ) {
        if ( bytes.contains( "kloggctl shell integration" ) ) return {};
        return block;
    }
    const auto finish = bytes.indexOf( End );
    if ( bytes.count( Begin ) != 1 || bytes.count( End ) != 1 || finish <= start
         || bytes.count( "# >>> kloggctl shell integration" ) != 1
         || bytes.count( "# <<< kloggctl shell integration" ) != 1
         || ( start > 0 && bytes.at( start - 1 ) != '\n' )
         || bytes.at( finish - 1 ) != '\n' ) return {};
    block.newline = bytes.mid( start + Begin.size(), 2 ) == "\r\n" ? "\r\n" : "\n";
    const auto contentStart = start + Begin.size() + block.newline.size();
    if ( bytes.mid( start + Begin.size(), block.newline.size() ) != block.newline ) return {};
    const auto metadataEnd = bytes.indexOf( '\n', contentStart );
    if ( metadataEnd < 0 ) return {};
    auto metadata = bytes.mid( contentStart, metadataEnd - contentStart );
    if ( metadata.endsWith( '\r' ) ) metadata.chop( 1 );
    if ( metadata != "# kloggctl separator: 0" && metadata != "# kloggctl separator: 1" ) return {};
    block.present = true;
    block.separator = metadata.endsWith( '1' );
    block.start = start;
    block.end = finish + End.size();
    if ( block.end != bytes.size() ) {
        if ( bytes.mid( block.end, block.newline.size() ) != block.newline ) return {};
        block.end += block.newline.size();
    }
    if ( block.separator ) {
        if ( start < block.newline.size()
             || bytes.mid( start - block.newline.size(), block.newline.size() ) != block.newline ) return {};
        block.start -= block.newline.size();
    }
    return block;
}
QByteArray renderBlock( const QString& directory, InstallShell shell, const Block& block )
{
    const auto path = quote( directory, shell ).toUtf8();
    QByteArray result = Begin + '\n' + "# kloggctl separator: " + ( block.separator ? "1\n" : "0\n" );
    if ( shell == InstallShell::Fish ) {
        result += "if not contains -- " + path + " $PATH\n    set -gx PATH " + path + " $PATH\nend\n";
    }
    else {
        result += "case \":${PATH-}:\" in\n    *:" + path + ":*) ;;\n    *) PATH="
                  + path + ":\"${PATH-}\"; export PATH ;;\nesac\n";
    }
    result += End + '\n';
    if ( block.newline == "\r\n" ) result.replace( "\n", "\r\n" );
    if ( block.separator ) result.prepend( block.newline );
    return result;
}

QString windowsKey( QString value )
{
    value.replace( '\\', '/' );
    while ( value.size() > 3 && value.endsWith( '/' ) ) value.chop( 1 );
    return value.toCaseFolded();
}
QString expandWindowsPath( QString value, const QProcessEnvironment& environment )
{
    for ( const auto& key : environment.keys() )
        value.replace( '%' + key + '%', environment.value( key ), Qt::CaseInsensitive );
    return value;
}
bool pathContains( const QString& path, const QString& directory, bool windows,
                   const QProcessEnvironment& environment )
{
    const auto parts = path.split( windows ? ';' : ':' );
    return std::any_of( parts.cbegin(), parts.cend(), [&]( const QString& part ) {
        return windows ? windowsKey( expandWindowsPath( part, environment ) ) == windowsKey( directory )
                       : QDir::cleanPath( part ) == directory;
    } );
}
InstallStatus previewStatus( bool removing, bool changed, bool managed )
{
    if ( removing ) return changed ? InstallStatus::Installed : InstallStatus::NotInstalled;
    if ( !changed ) return InstallStatus::Installed;
    return managed ? InstallStatus::NeedsRepair : InstallStatus::NotInstalled;
}
InstallResult fail( InstallResult result, InstallStatus status, const QString& message )
{
    result.status = status;
    result.message = message;
    return result;
}

std::optional<InstallResult> approvalError( const InstallResult& result, bool checking,
                                            const QStringList* approvedPaths )
{
    if ( !checking && approvedPaths && result.plannedPaths != *approvedPaths )
        return fail( result, InstallStatus::ConcurrentChange,
                     "The installation targets changed after preview; review and confirm the new plan" );
    return {};
}

InstallResult windowsInstall( const InstallOptions& options, InstallAction action,
                              InstallResult result, const UserPathAccess& access,
                              const QStringList* approvedPaths )
{
    const bool removing = action == InstallAction::Remove || action == InstallAction::CheckRemoval;
    const bool checking = action == InstallAction::Check || action == InstallAction::CheckRemoval;
    if ( !access.read || !access.replace )
        return fail( result, InstallStatus::Unsupported, "Native per-user PATH is unavailable on this platform" );
    const auto previous = access.read();
    if ( !previous ) return fail( result, InstallStatus::IoError, "Cannot safely read per-user PATH ownership" );
    auto desired = *previous;
    auto parts = desired.value.split( ';' );
    if ( !desired.ownedDirectory.isEmpty() ) {
        int equivalent = 0;
        for ( const auto& part : parts )
            if ( windowsKey( part ) == windowsKey( desired.ownedDirectory ) ) ++equivalent;
        const auto exact = parts.count( desired.ownedDirectory );
        if ( equivalent > 1 || ( equivalent == 1 && exact != 1 ) )
            return fail( result, InstallStatus::Collision, "The owned PATH entry was edited ambiguously; no changes made" );
        if ( removing || desired.ownedDirectory != result.pathDirectory ) {
            if ( exact == 1 ) parts.removeAt( parts.indexOf( desired.ownedDirectory ) );
            desired.value = parts.join( ';' );
            desired.ownedDirectory.clear();
        }
    }
    if ( !removing ) {
        const bool present = pathContains( desired.value, result.pathDirectory, true,
                                          desired.expandable ? options.environment : QProcessEnvironment{} );
        if ( present && desired.ownedDirectory.isEmpty() ) {
            // Existing entries belong to the user, even when they match exactly.
            if ( previous->ownedDirectory.isEmpty() ) {
                result.status = InstallStatus::Available;
                result.message = "The bundled CLI directory is already in per-user PATH. "
                                 "If it is not visible, sign out of Windows and sign in again; "
                                 "a new shell from an existing Explorer or harness may inherit the old PATH";
                result.restartRequired = !pathContains( options.environment.value( "PATH" ), result.pathDirectory,
                                                        true, QProcessEnvironment{} );
                if ( const auto error = approvalError( result, checking, approvedPaths ) ) return *error;
                return result;
            }
        }
        if ( !present ) {
            if ( !desired.value.isEmpty() ) desired.value += ';';
            desired.value += result.pathDirectory;
            desired.ownedDirectory = result.pathDirectory;
            desired.exists = true;
        }
    }
    const bool changed = desired.value != previous->value || desired.ownedDirectory != previous->ownedDirectory;
    if ( desired.value != previous->value ) result.plannedPaths.push_back( "HKCU\\Environment\\Path" );
    if ( desired.ownedDirectory != previous->ownedDirectory )
        result.plannedPaths.push_back( R"(HKCU\Software\klogg\ShellIntegration\OwnedPath)" );
    if ( checking ) {
        result.status = previewStatus( removing, changed, !previous->ownedDirectory.isEmpty() );
        result.message = removing ? "Owned per-user PATH removal preview; no changes made"
                                  : "Per-user PATH check completed; no changes made";
        return result;
    }
    if ( const auto error = approvalError( result, checking, approvedPaths ) ) return *error;
    if ( changed ) {
        const auto mutation = access.replace( *previous, desired );
        if ( !mutation.backupPath.isEmpty() ) result.backupPaths.push_back( mutation.backupPath );
        if ( mutation.status != InstallStatus::Installed )
            return fail( result, mutation.status, "Per-user PATH update did not complete; inspect PATH and backups before retrying" );
        result.changedPaths = result.plannedPaths;
        result.restartRequired = true;
    }
    result.status = action == InstallAction::Remove ? InstallStatus::Removed : InstallStatus::Installed;
    result.message = "Per-user PATH configured. Sign out of Windows and sign in again, or restart Windows, "
                     "before relying on command discovery. A new shell launched by an existing Explorer "
                     "or harness may still inherit the old PATH";
    return result;
}
} // namespace

std::optional<InstallOptions> defaultInstallOptions( const QString& executable )
{
    InstallOptions options;
    options.executable = executable;
    options.home = QDir::homePath();
    options.environment = QProcessEnvironment::systemEnvironment();
    const auto shell = installpal::defaultShell( options.environment );
    if ( !shell ) return {};
    options.shell = *shell;
    return options;
}

InstallResult configureControlInstallation( const InstallOptions& options, InstallAction action,
                                            const UserPathAccess* userPath, const QStringList* approvedPaths )
{
    const bool removing = action == InstallAction::Remove || action == InstallAction::CheckRemoval;
    const bool checking = action == InstallAction::Check || action == InstallAction::CheckRemoval;
    InstallResult result;
    result.executable = QDir::cleanPath( options.executable );
    const bool windows = options.shell == InstallShell::Windows;
    result.pathDirectory = windows ? QFileInfo( result.executable ).absolutePath()
        : QDir::cleanPath( options.binDirectory.isEmpty() ? options.home + "/.local/bin" : options.binDirectory );
    result.commandPath = windows ? result.executable : result.pathDirectory + "/kloggctl";
    if ( !absolutePath( options.executable ) || !absolutePath( options.home )
         || !QFileInfo( options.home ).isDir() || !absolutePath( result.pathDirectory )
         || result.pathDirectory.contains( windows ? ';' : ':' )
         || ( windows && result.pathDirectory.contains( '%' ) )
         || result.executable.contains( "/AppTranslocation/", Qt::CaseInsensitive )
         || result.executable.contains( "/.mount_" )
         || !installpal::executableIsSafe( result.executable ) )
        return fail( result, InstallStatus::InvalidInput, "Select a stable, existing executable and absolute user paths" );

    const auto inheritedPath = options.environment.value( "PATH" );
    if ( !removing ) {
        QStringList names{ "kloggctl" };
        if ( windows ) {
            names.clear();
            for ( const auto& extension : options.environment.value( "PATHEXT", ".COM;.EXE;.BAT;.CMD" ).split( ';' ) ) {
                if ( extension.startsWith( '.' ) && !extension.contains( '/' ) && !extension.contains( '\\' ) )
                    names.push_back( "kloggctl" + extension.toLower() );
            }
            if ( !names.contains( "kloggctl.exe" ) ) names.push_back( "kloggctl.exe" );
        }
        bool ownedPathFound = false;
        for ( auto directory : inheritedPath.split( windows ? ';' : ':' ) ) {
            if ( windows ) directory = expandWindowsPath( directory, options.environment );
            if ( directory.isEmpty() ) continue;
            for ( const auto& name : names ) {
                const auto candidate = QDir( directory ).filePath( name );
                if ( !QFileInfo( candidate ).isFile() || ( !windows && !QFileInfo( candidate ).isExecutable() ) ) continue;
                if ( windows ? windowsKey( candidate ) == windowsKey( result.executable )
                             : QDir::cleanPath( candidate ) == result.executable ) {
                    result.status = InstallStatus::Available;
                    result.commandPath = candidate;
                    result.message = "The intended CLI is already available on PATH; no changes made";
                    if ( const auto error = approvalError( result, checking, approvedPaths ) ) return *error;
                    return result;
                }
                installpal::FileState state;
                const auto read = installpal::readFile( candidate, state );
                const auto owned = read == InstallStatus::Available ? ownedLauncher( state.bytes ) : std::nullopt;
                if ( !windows && owned && QDir::cleanPath( candidate ) == result.commandPath ) {
                    ownedPathFound = true;
                    break;
                }
                return fail( result, InstallStatus::Collision, "Another executable on PATH shadows the intended CLI" );
            }
            if ( ownedPathFound ) break;
        }
    }
    if ( windows ) return windowsInstall( options, action, result,
                                          userPath ? *userPath : installpal::nativeUserPath(), approvedPaths );

    result.configFiles = options.configFiles;
    if ( result.configFiles.isEmpty() ) {
        if ( options.shell == InstallShell::Zsh ) {
            const auto root = options.environment.value( "ZDOTDIR", options.home );
            result.configFiles = QStringList{ root + "/.zshrc" };
        }
        else if ( options.shell == InstallShell::Fish ) {
            const auto root = options.environment.value( "XDG_CONFIG_HOME", options.home + "/.config" );
            result.configFiles = QStringList{ root + "/fish/config.fish" };
        }
        else {
            result.configFiles = QStringList{ options.home + "/.bashrc" };
            QString login = options.home + "/.bash_profile";
            for ( const auto* name : { "/.bash_profile", "/.bash_login", "/.profile" } ) {
                const auto path = options.home + QLatin1String( name );
                if ( removing ) result.configFiles.push_back( path );
                else if ( QFileInfo::exists( path ) || QFileInfo( path ).isSymLink() ) { login = path; break; }
            }
            if ( !removing ) result.configFiles.push_back( login );
        }
    }
    result.configFiles.removeDuplicates();
    for ( auto& path : result.configFiles ) {
        if ( !absolutePath( path ) ) return fail( result, InstallStatus::InvalidInput, "Shell configuration paths must be absolute" );
        path = QDir::cleanPath( path );
        if ( path == result.executable || path == result.commandPath )
            return fail( result, InstallStatus::InvalidInput, "Shell configuration cannot be an executable path" );
    }
    struct Change {
        QString path;
        installpal::FileState before;
        std::optional<QByteArray> after;
        unsigned mode = 0600;
    };
    std::vector<Change> changes;
    bool managed = false;
    installpal::FileState shim;
    auto status = installpal::readFile( result.commandPath, shim );
    if ( status != InstallStatus::Available ) return fail( result, status, "Cannot safely inspect the launcher destination" );
    if ( shim.exists && !ownedLauncher( shim.bytes ) )
        return fail( result, InstallStatus::Collision, "The launcher destination contains unowned or edited content" );
    managed = shim.exists;
    const auto desiredShim = launcher( result.executable );
    if ( removing ) {
        if ( shim.exists ) changes.push_back( { result.commandPath, shim, {}, 0700 } );
    }
    else if ( !shim.exists || shim.bytes != desiredShim || ( shim.mode & 0100U ) == 0 )
        changes.push_back( { result.commandPath, shim, desiredShim, 0700 } );

    const bool alreadyInPath = pathContains( inheritedPath, result.pathDirectory, false, options.environment );
    for ( const auto& path : result.configFiles ) {
        installpal::FileState before;
        status = installpal::readFile( path, before );
        if ( status != InstallStatus::Available ) return fail( result, status, "Cannot safely inspect a shell configuration file" );
        auto block = findBlock( before.bytes );
        if ( !block ) return fail( result, InstallStatus::Collision, "A shell integration block is malformed or ambiguous" );
        managed = managed || block->present;
        auto after = before.bytes;
        if ( removing ) {
            if ( block->present ) after.remove( block->start, block->end - block->start );
        }
        else if ( block->present ) {
            after.replace( block->start, block->end - block->start, renderBlock( result.pathDirectory, options.shell, *block ) );
        }
        else if ( !alreadyInPath ) {
            block->separator = !after.isEmpty() && !after.endsWith( '\n' );
            after += renderBlock( result.pathDirectory, options.shell, *block );
        }
        if ( after != before.bytes ) changes.push_back( { path, before, after, before.mode } );
    }
    result.restartRequired = removing ? !changes.empty() : !alreadyInPath;
    for ( const auto& change : changes ) result.plannedPaths.push_back( change.path );
    if ( checking ) {
        result.status = previewStatus( removing, !changes.empty(), managed );
        result.message = removing ? "Owned shell integration removal preview; no changes made"
                                  : "Shell integration check completed; no changes made";
        return result;
    }
    if ( const auto error = approvalError( result, checking, approvedPaths ) ) return *error;
    for ( const auto& change : changes ) {
        const auto mutation = installpal::replaceFile( change.path, change.before, change.after, change.mode );
        if ( !mutation.backup.isEmpty() ) result.backupPaths.push_back( mutation.backup );
        if ( mutation.status != InstallStatus::Installed )
            return fail( result, ( mutation.touched || !result.changedPaths.isEmpty() ) ? InstallStatus::PartialFailure : mutation.status,
                         "Shell integration did not complete; inspect changed paths and backups before retrying" );
        result.changedPaths.push_back( change.path );
    }
    result.status = action == InstallAction::Remove ? InstallStatus::Removed : InstallStatus::Installed;
    result.message = "Shell integration configured. Existing terminals and harnesses may need restart";
    return result;
}
} // namespace klogg::control
