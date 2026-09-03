#include "adbprocesstransport.h"
#include "adbdevicelistprovider.h"
#include "adbprotocol.h"
#include "commandargumenttokenizer.h"
#include "log.h"

using ui::internal::splitCommandArguments;

namespace adb = klogg::livecapture::adb;

namespace {

QStringList removeLogcatFormatArguments( const QStringList& arguments )
{
    const auto shortFormatOption = QStringLiteral( "-v" );
    const auto longFormatOption = QStringLiteral( "--format" );
    const auto longFormatAssignment = QStringLiteral( "--format=" );

    QStringList filtered;
    filtered.reserve( arguments.size() );

    using ArgumentIndex = decltype( arguments.size() );
    for ( ArgumentIndex index = 0; index < arguments.size(); ++index ) {
        const auto& argument = arguments.at( index );
        if ( argument == QStringLiteral( "--" ) ) {
            filtered.append( arguments.mid( index ) );
            break;
        }

        if ( argument == shortFormatOption || argument == longFormatOption ) {
            const auto valueIndex = index + 1;
            if ( valueIndex < arguments.size() ) {
                const auto& value = arguments.at( valueIndex );
                if ( !value.isEmpty() && !value.startsWith( QLatin1Char( '-' ) ) ) {
                    index = valueIndex;
                }
            }
            continue;
        }

        const auto isAttachedShortFormat
            = argument.startsWith( shortFormatOption ) && argument.size() > shortFormatOption.size();
        const auto isAssignedLongFormat = argument.startsWith( longFormatAssignment );
        if ( isAttachedShortFormat || isAssignedLongFormat ) {
            continue;
        }

        filtered.append( argument );
    }

    return filtered;
}

void appendLogcatFormatArguments( QStringList& arguments, bool ansiOutputEnabled )
{
    for ( const auto& argument : adb::buildLogcatFormatArguments( ansiOutputEnabled ) ) {
        arguments.append( QString::fromStdString( argument ) );
    }
}

} // namespace

AdbProcessTransport::AdbProcessTransport( QString adbExecutable, QString deviceSerial,
                                          QString extraArgs, bool ansiOutputEnabled,
                                          QObject* parent )
    : ProcessLiveSourceTransport( parent )
    , adbExecutable_( std::move( adbExecutable ) )
    , deviceSerial_( std::move( deviceSerial ) )
    , extraArgs_( std::move( extraArgs ) )
    , ansiOutputEnabled_( ansiOutputEnabled )
    , deviceProvider_( std::make_unique<AdbDeviceListProvider>( adbExecutable_, this ) )
{
}

QList<AdbDeviceInfo> AdbProcessTransport::listDevices( const QString& adbExecutable, QString* error )
{
    AdbDeviceListProvider provider( adbExecutable );
    return provider.listDevices( error );
}

QString AdbProcessTransport::detectAdbExecutable()
{
    return AdbDeviceListProvider::detectAdbExecutable();
}

AdbDeviceListProvider* AdbProcessTransport::deviceListProvider() const
{
    return deviceProvider_.get();
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

QString AdbProcessTransport::normalizeStreamingError( const QString& error ) const
{
    return QString::fromStdString( adb::normalizeLogcatStreamError( error.toStdString() ) );
}

QString AdbProcessTransport::normalizedAdbExecutable() const
{
    return AdbDeviceListProvider::normalizedExecutable( adbExecutable_ );
}

QStringList AdbProcessTransport::logcatArguments() const
{
    QStringList arguments{ QStringLiteral( "-s" ), deviceSerial_, QStringLiteral( "logcat" ) };
    appendLogcatFormatArguments( arguments, ansiOutputEnabled_ );

    const auto trimmedExtraArgs = extraArgs_.trimmed();
    if ( !trimmedExtraArgs.isEmpty() ) {
        const auto extraArguments = splitCommandArguments( trimmedExtraArgs );
        arguments.append( removeLogcatFormatArguments( extraArguments ) );
    }
    return arguments;
}
