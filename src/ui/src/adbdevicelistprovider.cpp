/*
 * Copyright (C) 2026 ZEACENT and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "adbdevicelistprovider.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

#include "commandargumenttokenizer.h"

namespace {

using klogg::livecapture::ErrorCategory;
using klogg::livecapture::ErrorScope;
using klogg::livecapture::Generation;
using klogg::livecapture::LiveSourceError;
using klogg::livecapture::RetryPolicy;
using ui::internal::expandTildePath;

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
    const auto appendEnvDir = [ &env, &appendDir ]( const char* envVar, const QString& suffix ) {
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

bool waitForFinishedOrKill( QProcess& process, int timeoutMs )
{
    if ( process.waitForFinished( timeoutMs ) ) {
        return true;
    }

    process.kill();
    process.waitForFinished( 1500 );
    return false;
}

AdbDeviceState deviceStateFromText( const QString& state )
{
    if ( state == QStringLiteral( "device" ) ) {
        return AdbDeviceState::Online;
    }
    if ( state == QStringLiteral( "unauthorized" ) ) {
        return AdbDeviceState::Unauthorized;
    }
    if ( state == QStringLiteral( "offline" ) ) {
        return AdbDeviceState::Offline;
    }
    return AdbDeviceState::Other;
}

std::string utf8String( const QString& value )
{
    const auto utf8 = value.toUtf8();
    return { utf8.constData(), static_cast<std::size_t>( utf8.size() ) };
}

LiveSourceError discoveryError( ErrorCategory category, RetryPolicy retryPolicy, const char* code,
                                const char* message, const QString& nativeDetail )
{
    return LiveSourceError{ category,    code,    ErrorScope::Service,
                            retryPolicy, message, utf8String( nativeDetail ) };
}

DeviceDiscoveryResult<AdbDeviceInfo> enumerateAdbDevices( Generation generation,
                                                          const QString& adbExecutable )
{
    QProcess process;
    process.start( AdbDeviceListProvider::normalizedExecutable( adbExecutable ),
                   { QStringLiteral( "devices" ), QStringLiteral( "-l" ) } );
    if ( !process.waitForStarted( 3000 ) ) {
        return { generation,
                 {},
                 discoveryError(
                     ErrorCategory::Configuration, RetryPolicy::Never, "adb-executable-not-found",
                     "adb could not be started; configure the ADB executable and retry.",
                     process.errorString() ) };
    }

    if ( !waitForFinishedOrKill( process, 5000 ) ) {
        return { generation,
                 {},
                 discoveryError( ErrorCategory::Backend, RetryPolicy::Immediate,
                                 "adb-device-list-timeout",
                                 "Timed out waiting for adb devices output; retry discovery.",
                                 process.errorString() ) };
    }

    if ( process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ) {
        const auto stdErr = QString::fromUtf8( process.readAllStandardError() ).trimmed();
        return { generation,
                 {},
                 discoveryError( ErrorCategory::Backend, RetryPolicy::Immediate,
                                 "adb-device-list-command-failed",
                                 "adb device discovery failed; verify the ADB service and retry.",
                                 stdErr.isEmpty() ? process.errorString() : stdErr ) };
    }

    return parseAdbDeviceDiscovery( generation, process.readAllStandardOutput() );
}

QString errorText( const LiveSourceError& error )
{
    if ( !error.nativeDetail.empty() ) {
        return QString::fromUtf8( error.nativeDetail.data(),
                                  static_cast<int>( error.nativeDetail.size() ) );
    }
    return QString::fromUtf8( error.message.data(), static_cast<int>( error.message.size() ) );
}

} // namespace

QList<AdbDeviceInfo> parseAdbDeviceListOutput( const QByteArray& output )
{
    QList<AdbDeviceInfo> devices;
    bool headerSeen = false;
    const auto lines = QString::fromUtf8( output ).split( '\n' );
    for ( auto line : lines ) {
        line = line.trimmed();
        if ( line == QStringLiteral( "List of devices attached" ) ) {
            headerSeen = true;
            continue;
        }
        if ( !headerSeen || line.isEmpty() ) {
            continue;
        }

        const auto parts = line.simplified().split( ' ' );
        if ( parts.size() < 2 ) {
            continue;
        }

        const auto& serial = parts.front();
        const auto& state = parts.at( 1 );
        QString model;
        QString device;
        QString product;
        for ( decltype( parts.size() ) i = 2; i < parts.size(); ++i ) {
            const auto& part = parts.at( i );
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

        auto description = model;
        if ( description.isEmpty() ) {
            description = device;
        }
        if ( description.isEmpty() ) {
            description = product;
        }
        if ( description.isEmpty() ) {
            description = serial;
        }

        const auto displayName = state == QStringLiteral( "device" )
                                     ? QStringLiteral( "%1 (%2)" ).arg( description, serial )
                                     : QStringLiteral( "%1 [%2]" ).arg( serial, state );
        devices.push_back(
            AdbDeviceInfo{ serial, displayName, line, deviceStateFromText( state ), state } );
    }

    return devices;
}

DeviceDiscoveryResult<AdbDeviceInfo> parseAdbDeviceDiscovery( Generation generation,
                                                              const QByteArray& output )
{
    const auto lines = QString::fromUtf8( output ).split( '\n' );
    const auto hasHeader = std::any_of( lines.cbegin(), lines.cend(), []( const QString& line ) {
        return line.trimmed() == QStringLiteral( "List of devices attached" );
    } );
    if ( !hasHeader ) {
        return { generation,
                 {},
                 discoveryError(
                     ErrorCategory::Backend, RetryPolicy::Immediate,
                     "adb-device-list-protocol-error",
                     "The ADB discovery service returned an invalid response; retry discovery.",
                     QStringLiteral( "Expected the adb device-list header." ) ) };
    }

    return { generation, parseAdbDeviceListOutput( output ), std::nullopt };
}

AdbDeviceListProvider::AdbDeviceListProvider( QString adbExecutable, QObject* parent )
    : DeviceListProviderBase(
          [ adbExecutable ]( Generation generation ) {
              return enumerateAdbDevices( generation, adbExecutable );
          },
          parent )
    , adbExecutable_( std::move( adbExecutable ) )
{
}

QString AdbDeviceListProvider::detectAdbExecutable()
{
    return findAdbAtKnownLocation();
}

QString AdbDeviceListProvider::normalizedExecutable( const QString& adbExecutable )
{
    auto expanded = expandTildePath( adbExecutable.trimmed() );
    if ( !expanded.isEmpty() ) {
        return expanded;
    }

    auto resolved = findAdbAtKnownLocation();
    if ( !resolved.isEmpty() ) {
        return resolved;
    }

    return QStringLiteral( "adb" );
}

bool AdbDeviceListProvider::deviceMatches( const AdbDeviceInfo& device,
                                           const QString& deviceId ) const
{
    return device.serial == deviceId && device.isOnline();
}

QList<AdbDeviceInfo> AdbDeviceListProvider::doListDevices( QString* error ) const
{
    auto result = enumerateAdbDevices( Generation{ 0 }, adbExecutable_ );
    if ( error ) {
        *error = result.error ? errorText( *result.error ) : QString{};
    }
    return std::move( result.devices );
}
