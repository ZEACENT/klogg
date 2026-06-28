#include "adbprocesstransport.h"
#include "adbdevicelistprovider.h"
#include "commandargumenttokenizer.h"
#include "log.h"

using ui::internal::splitCommandArguments;

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

QString AdbProcessTransport::normalizedAdbExecutable() const
{
    return AdbDeviceListProvider::normalizedExecutable( adbExecutable_ );
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
