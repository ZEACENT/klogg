/*
 * Copyright (C) 2026 ZEACENT and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "iosdevicelistprovider.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include "commandargumenttokenizer.h"
#include "iosdeviceparser.h"
#include "log.h"

namespace {

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

QString findExecutableAtKnownLocation( const QString& executable )
{
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

    // pymobiledevice3 can take ~8s on the slow path; use 10s to avoid
    // premature timeouts on healthy-but-slow probes.
    if ( !waitForFinishedOrKill( process, 10000 ) ) {
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
#endif // Q_OS_MAC

} // namespace

IosDeviceListProvider::IosDeviceListProvider( QString executable, QObject* parent )
    : DeviceListProviderBase( parent )
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
    const auto expanded = expandTildePath( executable.trimmed() );
    if ( !expanded.isEmpty() ) {
        return expanded;
    }

#ifdef Q_OS_MAC
    const auto detected = findExecutableAtKnownLocation( QStringLiteral( "pymobiledevice3" ) );
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
#ifndef Q_OS_MAC
    Q_UNUSED( error );
    if ( error ) {
        *error = QObject::tr( "iOS log streaming is supported only on macOS." );
    }
    return {};
#else
    QList<IosDeviceInfo> devices;
    const auto pymobiledeviceExecutable = normalizedExecutable( executable_ );
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
