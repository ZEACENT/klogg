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

#include "iosdevicelistprovider.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include "commandargumenttokenizer.h"

namespace {

using klogg::livecapture::ErrorCategory;
using klogg::livecapture::ErrorScope;
using klogg::livecapture::Generation;
using klogg::livecapture::LiveSourceError;
using klogg::livecapture::RetryPolicy;
using ui::internal::expandTildePath;

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
            candidates.append( QDir::cleanPath(
                pythonRoot.absoluteFilePath( version + QStringLiteral( "/bin/" ) + executable ) ) );
        }
    }

    return candidates;
}

QString findExecutableAtKnownLocation( const QString& executable )
{
    const auto candidates = knownExecutableCandidatePaths( executable );
    for ( const auto& candidate : candidates ) {
        const QFileInfo info( candidate );
        if ( info.exists() && info.isFile() && info.isExecutable() ) {
            return info.absoluteFilePath();
        }
    }

    return QStandardPaths::findExecutable( executable );
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

struct CommandResult {
    QByteArray output;
    std::optional<LiveSourceError> error;
};

CommandResult runPymobiledeviceListCommand( const QString& executable,
                                            const QStringList& arguments )
{
    QProcess process;
    process.start( executable, arguments );
    if ( !process.waitForStarted( 3000 ) ) {
        return { {},
                 discoveryError(
                     ErrorCategory::Configuration, RetryPolicy::Never, "ios-executable-not-found",
                     "pymobiledevice3 executable was not found; configure it and retry.",
                     process.errorString() ) };
    }

    if ( !waitForFinishedOrKill( process, 10000 ) ) {
        return { {},
                 discoveryError( ErrorCategory::Backend, RetryPolicy::Immediate,
                                 "ios-device-list-timeout",
                                 "Timed out waiting for iOS device discovery; retry discovery.",
                                 process.errorString() ) };
    }

    const auto stdOut = process.readAllStandardOutput();
    if ( process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ) {
        const auto stdErr = QString::fromUtf8( process.readAllStandardError() ).trimmed();
        return { {},
                 discoveryError( ErrorCategory::Backend, RetryPolicy::Immediate,
                                 "ios-device-list-command-failed",
                                 "pymobiledevice3 device discovery failed; retry discovery.",
                                 stdErr.isEmpty() ? process.errorString() : stdErr ) };
    }

    return { stdOut, std::nullopt };
}

QStringList pymobiledeviceSimpleListArguments()
{
    return { QStringLiteral( "usbmux" ), QStringLiteral( "list" ), QStringLiteral( "--simple" ) };
}

QStringList pymobiledeviceLegacyListArguments()
{
    return { QStringLiteral( "usbmux" ), QStringLiteral( "list" ) };
}
#endif // Q_OS_MAC

DeviceDiscoveryResult<IosDeviceInfo> enumerateIosDevices( Generation generation,
                                                          const QString& executable )
{
#ifndef Q_OS_MAC
    Q_UNUSED( executable );
    return { generation,
             {},
             discoveryError( ErrorCategory::Configuration, RetryPolicy::Never,
                             "ios-platform-unsupported",
                             "iOS log streaming is supported only on macOS.", QString{} ) };
#else
    const auto pymobiledeviceExecutable = IosDeviceListProvider::normalizedExecutable( executable );
    auto legacy = runPymobiledeviceListCommand( pymobiledeviceExecutable,
                                                pymobiledeviceLegacyListArguments() );
    if ( !legacy.error ) {
        return parsePymobiledeviceDeviceDiscovery( generation, legacy.output );
    }

    if ( legacy.error->code == "ios-executable-not-found" ) {
        return { generation, {}, std::move( legacy.error ) };
    }

    // Older pymobiledevice3 versions may reject the rich JSON command. Their
    // --simple output is an intentionally provider-specific text protocol.
    auto simple = runPymobiledeviceListCommand( pymobiledeviceExecutable,
                                                pymobiledeviceSimpleListArguments() );
    if ( simple.error ) {
        return { generation, {}, std::move( simple.error ) };
    }

    return { generation, parsePymobiledeviceSimpleDeviceList( simple.output ), std::nullopt };
#endif
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

IosDeviceListProvider::IosDeviceListProvider( QString executable, QObject* parent )
    : DeviceListProviderBase(
          [ executable ]( Generation generation ) {
              return enumerateIosDevices( generation, executable );
          },
          parent )
    , executable_( std::move( executable ) )
{
}

QString IosDeviceListProvider::detectIosSyslogExecutable()
{
#ifdef Q_OS_MAC
    return findExecutableAtKnownLocation( QStringLiteral( "pymobiledevice3" ) );
#else
    return {};
#endif
}

QString IosDeviceListProvider::normalizedExecutable( const QString& executable )
{
    auto expanded = expandTildePath( executable.trimmed() );
    if ( !expanded.isEmpty() ) {
        return expanded;
    }

#ifdef Q_OS_MAC
    auto detected = findExecutableAtKnownLocation( QStringLiteral( "pymobiledevice3" ) );
    if ( !detected.isEmpty() ) {
        return detected;
    }
#endif

    return QStringLiteral( "pymobiledevice3" );
}

bool IosDeviceListProvider::deviceMatches( const IosDeviceInfo& device,
                                           const QString& deviceId ) const
{
    return device.udid == deviceId;
}

QList<IosDeviceInfo> IosDeviceListProvider::doListDevices( QString* error ) const
{
    auto result = enumerateIosDevices( Generation{ 0 }, executable_ );
    if ( error ) {
        *error = result.error ? errorText( *result.error ) : QString{};
    }
    return std::move( result.devices );
}
