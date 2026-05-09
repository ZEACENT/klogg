#include "adbprocesstransport.h"
#include "commandargumenttokenizer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

namespace {
// See AdbProcessTransport::detectAdbExecutable for rationale and probe order.
QString findAdbAtKnownLocation()
{
    const auto env = QProcessEnvironment::systemEnvironment();

#if defined( Q_OS_WIN )
    const QString exe = QStringLiteral( "adb.exe" );
#else
    const QString exe = QStringLiteral( "adb" );
#endif

    QStringList candidates;
    const auto appendDir = [ &candidates, &exe ]( const QString& dir ) {
        if ( !dir.isEmpty() ) {
            candidates.append( QDir::cleanPath( dir + QLatin1Char( '/' ) + exe ) );
        }
    };
    const auto appendEnvDir
        = [ &env, &appendDir ]( const char* envVar, const QString& suffix ) {
              const auto value = env.value( QString::fromLatin1( envVar ) );
              if ( !value.isEmpty() ) {
                  appendDir( value + suffix );
              }
          };

    appendEnvDir( "ANDROID_SDK_ROOT", QStringLiteral( "/platform-tools" ) );
    appendEnvDir( "ANDROID_HOME", QStringLiteral( "/platform-tools" ) );

#if defined( Q_OS_WIN )
    appendEnvDir( "LOCALAPPDATA", QStringLiteral( "/Android/Sdk/platform-tools" ) );
    appendEnvDir( "ProgramFiles", QStringLiteral( "/Android/android-sdk/platform-tools" ) );
#elif defined( Q_OS_MAC )
    candidates.append( QStringLiteral( "/usr/local/bin/adb" ) );
    candidates.append( QStringLiteral( "/opt/homebrew/bin/adb" ) );
    appendDir( QDir::homePath() + QStringLiteral( "/Library/Android/sdk/platform-tools" ) );
#else
    candidates.append( QStringLiteral( "/usr/local/bin/adb" ) );
    candidates.append( QStringLiteral( "/usr/bin/adb" ) );
    appendDir( QDir::homePath() + QStringLiteral( "/Android/Sdk/platform-tools" ) );
#endif

    for ( const auto& candidate : candidates ) {
        const QFileInfo info( candidate );
        if ( info.exists() && info.isFile() && info.isExecutable() ) {
            return info.absoluteFilePath();
        }
    }
    return {};
}

QString normalizedExecutable( const QString& adbExecutable )
{
    const auto trimmed = adbExecutable.trimmed();
    if ( !trimmed.isEmpty() ) {
        return trimmed;
    }

    const auto resolved = findAdbAtKnownLocation();
    if ( !resolved.isEmpty() ) {
        return resolved;
    }

    // Last resort: bare "adb" lets QProcess fall back to PATH lookup.  This
    // matches the historical behavior on hosts where the user's shell PATH
    // (Linux from terminal) or System PATH (Windows with Android SDK) is
    // already correctly configured.
    return QStringLiteral( "adb" );
}

bool waitForFinishedOrKill( QProcess& process, int timeoutMs )
{
    if ( process.waitForFinished( timeoutMs ) ) {
        return true;
    }

    process.kill();
    process.waitForFinished( 1500 );
    return false;
}

} // namespace

using ui::internal::splitCommandArguments;

AdbProcessTransport::AdbProcessTransport( QString adbExecutable, QString deviceSerial,
                                          QString extraArgs, bool ansiOutputEnabled,
                                          QObject* parent )
    : ProcessLiveSourceTransport( parent )
    , adbExecutable_( std::move( adbExecutable ) )
    , deviceSerial_( std::move( deviceSerial ) )
    , extraArgs_( std::move( extraArgs ) )
    , ansiOutputEnabled_( ansiOutputEnabled )
{
}

QList<AdbDeviceInfo> AdbProcessTransport::listDevices( const QString& adbExecutable, QString* error )
{
    QProcess process;
    process.start( normalizedExecutable( adbExecutable ),
                   { QStringLiteral( "devices" ), QStringLiteral( "-l" ) } );
    if ( !process.waitForStarted( 3000 ) ) {
        if ( error ) {
            *error = process.errorString();
        }
        return {};
    }

    if ( !waitForFinishedOrKill( process, 5000 ) ) {
        if ( error ) {
            *error = QObject::tr( "Timed out waiting for adb devices output" );
        }
        return {};
    }

    if ( process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ) {
        if ( error ) {
            const auto stdErr = QString::fromUtf8( process.readAllStandardError() ).trimmed();
            *error = stdErr.isEmpty() ? process.errorString() : stdErr;
        }
        return {};
    }

    QList<AdbDeviceInfo> devices;
    const auto lines = QString::fromUtf8( process.readAllStandardOutput() ).split( '\n' );
    for ( auto line : lines ) {
        line = line.trimmed();
        if ( line.isEmpty() || line.startsWith( QStringLiteral( "List of devices attached" ) ) ) {
            continue;
        }

        const auto parts = line.simplified().split( ' ' );
        if ( parts.size() < 2 || parts[ 1 ] != QStringLiteral( "device" ) ) {
            continue;
        }

        const auto serial = parts.front();
        QString model;
        QString device;
        QString product;
        for ( auto i = 2; i < parts.size(); ++i ) {
            const auto& part = parts[ i ];
            if ( part.startsWith( QStringLiteral( "model:" ) ) ) {
                model = part.mid( 6 ).replace( '_', ' ' );
            }
            else if ( part.startsWith( QStringLiteral( "device:" ) ) ) {
                device = part.mid( 7 ).replace( '_', ' ' );
            }
            else if ( part.startsWith( QStringLiteral( "product:" ) ) ) {
                product = part.mid( 8 ).replace( '_', ' ' );
            }
        }

        QString description = model;
        if ( description.isEmpty() ) {
            description = device;
        }
        if ( description.isEmpty() ) {
            description = product;
        }
        if ( description.isEmpty() ) {
            description = serial;
        }

        devices.push_back( AdbDeviceInfo{ serial,
                                          QStringLiteral( "%1 (%2)" ).arg( description, serial ),
                                          line } );
    }

    return devices;
}

ProcessLiveSourceTransport::Command AdbProcessTransport::streamingCommand() const
{
    return Command{ normalizedAdbExecutable(), logcatArguments() };
}

ProcessLiveSourceTransport::Command AdbProcessTransport::clearCommand() const
{
    return Command{ normalizedAdbExecutable(),
                    { QStringLiteral( "-s" ), deviceSerial_, QStringLiteral( "logcat" ),
                      QStringLiteral( "-c" ) } };
}

QString AdbProcessTransport::normalizedAdbExecutable() const
{
    return normalizedExecutable( adbExecutable_ );
}

QString AdbProcessTransport::detectAdbExecutable()
{
    return findAdbAtKnownLocation();
}

QStringList AdbProcessTransport::logcatArguments() const
{
    QStringList arguments{ QStringLiteral( "-s" ), deviceSerial_, QStringLiteral( "logcat" ) };
    if ( ansiOutputEnabled_ ) {
        arguments.append( { QStringLiteral( "-v" ), QStringLiteral( "color" ) } );
    }
    const auto trimmedExtraArgs = extraArgs_.trimmed();
    if ( !trimmedExtraArgs.isEmpty() ) {
        arguments.append( splitCommandArguments( trimmedExtraArgs ) );
    }
    return arguments;
}
