#include "ioslogprocesstransport.h"
#include "commandargumenttokenizer.h"
#include "iosdeviceparser.h"
#include "iosdevicelistprovider.h"
#include "log.h"

#include <utility>

namespace {

using ui::internal::splitCommandArguments;

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
    , deviceProvider_( std::make_unique<IosDeviceListProvider>( executable_, this ) )
{
}

QList<IosDeviceInfo> IosLogProcessTransport::listDevices( const QString& executable,
                                                          QString* error )
{
    IosDeviceListProvider provider( executable );
    return provider.listDevices( error );
}

QString IosLogProcessTransport::detectIosSyslogExecutable()
{
    return IosDeviceListProvider::detectIosSyslogExecutable();
}

IosDeviceListProvider* IosLogProcessTransport::deviceListProvider() const
{
    return deviceProvider_.get();
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
    //
    // A PTY merges the child's stderr into stdout, which would let
    // pymobiledevice3's diagnostic output (ERROR logs, urllib3 warnings) leak
    // into the log view.  Run the inner command via a shell that redirects its
    // stderr to the transport's temp file *outside* the PTY:
    //   sh -c 'exec "$@" 2>"$0"' <stderrFile> <program> <args...>
    // "$0" is the stderr file, "$@" is the program + args (passed through
    // literally, with no shell-quoting hazard).  stderr is still captured for
    // error detection (read from the temp file in the finished handler) but
    // never reaches the log view.  macOS script(1) runs the command "with an
    // optional argument vector" (execvp directly), so the sh -c string and the
    // following args pass through unmodified.
    if ( ansiOutputEnabled_ ) {
        auto args = QStringList{
            QStringLiteral( "-q" ),
            QStringLiteral( "/dev/null" ),
            QStringLiteral( "/bin/sh" ),
            QStringLiteral( "-c" ),
            QStringLiteral( "exec \"$@\" 2>\"$0\"" ),
            stderrFilePath(),
            innerCommand.program,
        } + innerCommand.arguments;
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
    return IosDeviceListProvider::normalizedExecutable( executable_ );
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
