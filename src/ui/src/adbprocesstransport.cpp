#include "adbprocesstransport.h"

#include <QProcess>

namespace {
QString normalizedExecutable( const QString& adbExecutable )
{
    return adbExecutable.trimmed().isEmpty() ? QStringLiteral( "adb" ) : adbExecutable.trimmed();
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

bool canEscapeArgumentCharacter( const QChar nextChar, const QChar quoteChar )
{
    if ( quoteChar == QLatin1Char( '"' ) ) {
        return nextChar == QLatin1Char( '"' ) || nextChar == QLatin1Char( '\\' );
    }

    if ( quoteChar == QLatin1Char( '\'' ) ) {
        return false;
    }

    return nextChar.isSpace() || nextChar == QLatin1Char( '"' )
           || nextChar == QLatin1Char( '\'' ) || nextChar == QLatin1Char( '\\' );
}

QStringList splitCommandArguments( const QString& arguments )
{
    QStringList tokens;
    QString currentToken;
    QChar quoteChar;

    for ( int i = 0; i < arguments.size(); ++i ) {
        const auto ch = arguments.at( i );
        if ( ch == QLatin1Char( '\\' ) ) {
            const auto nextIndex = i + 1;
            if ( nextIndex < arguments.size() ) {
                const auto nextChar = arguments.at( nextIndex );
                if ( canEscapeArgumentCharacter( nextChar, quoteChar ) ) {
                    currentToken.append( nextChar );
                    ++i;
                    continue;
                }
            }

            currentToken.append( ch );
            continue;
        }

        if ( !quoteChar.isNull() ) {
            if ( ch == quoteChar ) {
                quoteChar = QChar{};
            }
            else {
                currentToken.append( ch );
            }
            continue;
        }

        if ( ch == QLatin1Char( '"' ) || ch == QLatin1Char( '\'' ) ) {
            quoteChar = ch;
            continue;
        }

        if ( ch.isSpace() ) {
            if ( !currentToken.isEmpty() ) {
                tokens.push_back( currentToken );
                currentToken.clear();
            }
            continue;
        }

        currentToken.append( ch );
    }

    if ( !currentToken.isEmpty() ) {
        tokens.push_back( currentToken );
    }

    return tokens;
}
} // namespace

AdbProcessTransport::AdbProcessTransport( QString adbExecutable, QString deviceSerial,
                                          QString extraArgs, QObject* parent )
    : ProcessLiveSourceTransport( parent )
    , adbExecutable_( std::move( adbExecutable ) )
    , deviceSerial_( std::move( deviceSerial ) )
    , extraArgs_( std::move( extraArgs ) )
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

QStringList AdbProcessTransport::logcatArguments() const
{
    QStringList arguments{ QStringLiteral( "-s" ), deviceSerial_, QStringLiteral( "logcat" ) };
    const auto trimmedExtraArgs = extraArgs_.trimmed();
    if ( !trimmedExtraArgs.isEmpty() ) {
        arguments.append( splitCommandArguments( trimmedExtraArgs ) );
    }
    return arguments;
}
