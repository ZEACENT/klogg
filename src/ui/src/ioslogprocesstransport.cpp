#include "ioslogprocesstransport.h"
#include "commandargumenttokenizer.h"
#include "iosdeviceparser.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <utility>

namespace {

using ui::internal::splitCommandArguments;
using ui::internal::expandTildePath;

#ifdef Q_OS_MAC
QStringList knownExecutableCandidatePaths( const QString& executable )
{
    QStringList candidates{
        QDir::cleanPath( QStringLiteral( "/opt/homebrew/bin/" ) + executable ),
        QDir::cleanPath( QStringLiteral( "/usr/local/bin/" ) + executable ),
    };

    const auto homeDir = QStandardPaths::writableLocation( QStandardPaths::HomeLocation );
    if ( !homeDir.isEmpty() ) {
        const auto pythonRoot = QDir( homeDir + QStringLiteral( "/Library/Python" ) );
        const auto versionDirs = pythonRoot.entryList( QDir::Dirs | QDir::NoDotAndDotDot );
        for ( const auto& version : versionDirs ) {
            candidates.append(
                QDir::cleanPath( pythonRoot.absoluteFilePath( version + QStringLiteral( "/bin/" ) + executable ) ) );
        }
    }

    return candidates;
}
#endif

QString findExecutableAtKnownLocation( const QString& executable )
{
#ifdef Q_OS_MAC
    const auto candidates = knownExecutableCandidatePaths( executable );
    for ( const auto& candidate : candidates ) {
        const QFileInfo info( candidate );
        if ( info.exists() && info.isFile() && info.isExecutable() ) {
            return info.absoluteFilePath();
        }
    }

    const auto fromPath = QStandardPaths::findExecutable( executable );
    if ( !fromPath.isEmpty() ) {
        return fromPath;
    }
#else
    Q_UNUSED( executable );
#endif

    return {};
}

QString normalizedIosSyslogExecutable( const QString& executable )
{
    const auto expanded = expandTildePath( executable.trimmed() );
    if ( !expanded.isEmpty() ) {
        return expanded;
    }

    const auto detected = findExecutableAtKnownLocation( QStringLiteral( "pymobiledevice3" ) );
    if ( !detected.isEmpty() ) {
        return detected;
    }

    return QStringLiteral( "pymobiledevice3" );
}

#ifdef Q_OS_MAC
bool waitForFinishedOrKill( QProcess& process, int timeoutMs )
{
    if ( process.waitForFinished( timeoutMs ) ) {
        return true;
    }

    process.kill();
    process.waitForFinished( 1500 );
    return false;
}

bool runPymobiledeviceListCommand( const QString& executable, const QStringList& arguments,
                                   QList<IosDeviceInfo>* devices, QString* error )
{
    QProcess process;
    process.start( executable, arguments );
    if ( !process.waitForStarted( 3000 ) ) {
        if ( error ) {
            *error = process.errorString();
        }
        return false;
    }

    if ( !waitForFinishedOrKill( process, 5000 ) ) {
        if ( error ) {
            *error = QObject::tr( "Timed out waiting for iOS device list output" );
        }
        return false;
    }

    const auto stdOut = process.readAllStandardOutput();
    if ( process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ) {
        if ( error ) {
            const auto stdErr = QString::fromUtf8( process.readAllStandardError() ).trimmed();
            *error = stdErr.isEmpty() ? process.errorString() : stdErr;
        }
        return false;
    }

    *devices = parsePymobiledeviceSimpleDeviceList( stdOut );
    return true;
}

QStringList pymobiledeviceSimpleListArguments()
{
    return { QStringLiteral( "usbmux" ), QStringLiteral( "list" ), QStringLiteral( "--simple" ) };
}

QStringList pymobiledeviceLegacyListArguments()
{
    return { QStringLiteral( "usbmux" ), QStringLiteral( "list" ) };
}
#endif

QStringList pymobiledeviceStreamingArguments( const QString& deviceUdid )
{
    QStringList arguments;
    arguments.append( QStringLiteral( "syslog" ) );
    arguments.append( QStringLiteral( "live" ) );

    if ( deviceUdid.isEmpty() ) {
        return arguments;
    }

    arguments.append( { QStringLiteral( "--udid" ), deviceUdid } );
    return arguments;
}

} // namespace

IosLogProcessTransport::IosLogProcessTransport( QString executable, QString deviceUdid,
                                                QString extraArgs, bool ansiOutputEnabled,
                                                QObject* parent )
    : ProcessLiveSourceTransport( parent )
    , executable_( std::move( executable ) )
    , deviceUdid_( std::move( deviceUdid ) )
    , extraArgs_( std::move( extraArgs ) )
    , ansiOutputEnabled_( ansiOutputEnabled )
{
}

QList<IosDeviceInfo> IosLogProcessTransport::listDevices( const QString& executable,
                                                          QString* error )
{
#ifndef Q_OS_MAC
    Q_UNUSED( executable );
    if ( error ) {
        *error = QObject::tr( "iOS log streaming is supported only on macOS." );
    }
    return {};
#else
    QList<IosDeviceInfo> devices;
    const auto pymobiledeviceExecutable = normalizedIosSyslogExecutable( executable );
    // Try the full JSON output first — it includes DeviceName, ProductType,
    // ProductVersion, etc.  Fall back to --simple (UDID-only) only if the
    // full listing is not supported by the installed pymobiledevice3 version.
    if ( !runPymobiledeviceListCommand( pymobiledeviceExecutable, pymobiledeviceLegacyListArguments(),
                                        &devices, error ) ) {
        QString simpleError;
        if ( !runPymobiledeviceListCommand( pymobiledeviceExecutable,
                                            pymobiledeviceSimpleListArguments(), &devices,
                                            &simpleError ) ) {
            if ( error && !simpleError.isEmpty() ) {
                *error = simpleError;
            }
            return {};
        }

        if ( error ) {
            error->clear();
        }
    }

    if ( devices.isEmpty() && error ) {
        *error = QObject::tr( "No iOS devices reported by pymobiledevice3." );
    }
    return devices;
#endif
}

QString IosLogProcessTransport::detectIosSyslogExecutable()
{
    return findExecutableAtKnownLocation( QStringLiteral( "pymobiledevice3" ) );
}

bool IosLogProcessTransport::clearRemote( QString* error )
{
    if ( error ) {
        *error = tr( "iOS live logs cannot be cleared remotely." );
    }
    return false;
}

bool IosLogProcessTransport::connectTransport()
{
    ptyPrefixStripped_ = false;
    pendingPrefixProbe_.clear();
    return ProcessLiveSourceTransport::connectTransport();
}

ProcessLiveSourceTransport::Command IosLogProcessTransport::streamingCommand() const
{
    const auto innerCommand = Command{ normalizedExecutable(), streamArguments() };

#ifdef Q_OS_MAC
    // pymobiledevice3 checks isatty() in addition to the --color flag; when
    // QProcess pipes stdout, isatty() is false so ANSI escape codes are never
    // emitted.  Wrap with /usr/bin/script to allocate a PTY so that
    // pymobiledevice3 sees a terminal and actually produces colored output.
    if ( ansiOutputEnabled_ ) {
        auto args = QStringList{ QStringLiteral( "-q" ), QStringLiteral( "/dev/null" ),
                                 innerCommand.program }
                    + innerCommand.arguments;
        return Command{ QStringLiteral( "/usr/bin/script" ), std::move( args ) };
    }
#endif

    return innerCommand;
}

ProcessLiveSourceTransport::Command IosLogProcessTransport::clearCommand() const
{
#ifdef Q_OS_WIN
    return Command{ QStringLiteral( "cmd" ),
                    { QStringLiteral( "/c" ), QStringLiteral( "exit" ), QStringLiteral( "0" ) } };
#else
    return Command{ QStringLiteral( "true" ), {} };
#endif
}

QString IosLogProcessTransport::normalizedExecutable() const
{
    return normalizedIosSyslogExecutable( executable_ );
}

QStringList IosLogProcessTransport::streamArguments() const
{
    QStringList arguments{ ansiOutputEnabled_ ? QStringLiteral( "--color" )
                                              : QStringLiteral( "--no-color" ) };
    arguments.append( pymobiledeviceStreamingArguments( deviceUdid_ ) );
    const auto trimmedExtraArgs = extraArgs_.trimmed();
    if ( !trimmedExtraArgs.isEmpty() ) {
        arguments.append( splitCommandArguments( trimmedExtraArgs ) );
    }
    return arguments;
}

void IosLogProcessTransport::filterReceivedBytes( QByteArray& data )
{
#ifdef Q_OS_MAC
    if ( !ansiOutputEnabled_ || ptyPrefixStripped_ || data.isEmpty() ) {
        return;
    }
    // macOS script(1) emits a visual ^D\b\b prefix at the start of its output:
    //   0x5e ('^') 0x44 ('D') 0x08 (BS) 0x08 (BS)
    // Strip this garbage so it doesn't appear as a spurious first line.
    // The prefix may arrive split across chunks, so buffer until we can
    // decide definitively.
    static const QByteArray scriptPtyPrefix = QByteArrayLiteral( "^D" ) + QByteArray( 2, '\b' );

    if ( !pendingPrefixProbe_.isEmpty() ) {
        pendingPrefixProbe_.append( data );
        data = pendingPrefixProbe_;
        pendingPrefixProbe_.clear();
    }

    if ( data.startsWith( scriptPtyPrefix ) ) {
        data.remove( 0, scriptPtyPrefix.size() );
        ptyPrefixStripped_ = true;
        return;
    }

    // data might be a partial prefix (e.g. just "^D" without the two BS yet).
    if ( scriptPtyPrefix.startsWith( data ) ) {
        pendingPrefixProbe_ = data;
        data.clear();
        return;
    }

    // Not a prefix match at all — first chunk is real log data; done probing.
    ptyPrefixStripped_ = true;
#else
    Q_UNUSED( data );
#endif
}
