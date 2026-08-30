#include "iosdeviceparser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <utility>

#include "livestate.h"

namespace {

constexpr ushort Escape = 0x1B;
constexpr ushort Csi = '[';

bool isCsiFinalByte( const QChar character )
{
    const auto value = character.unicode();
    return value >= 0x40 && value <= 0x7e;
}

QString firstStringValue( const QJsonObject& object, std::initializer_list<const char*> keys )
{
    for ( const auto* key : keys ) {
        const auto value = object.value( QLatin1String( key ) );
        if ( value.isString() ) {
            const auto text = value.toString().trimmed();
            if ( !text.isEmpty() ) {
                return text;
            }
        }
    }

    return {};
}

QString buildDisplayName( const QString& name, const QString& udid, const QString& productType,
                          const QString& productVersion )
{
    QStringList parts;
    if ( !name.isEmpty() ) {
        parts.append( name );
    }
    if ( !udid.isEmpty() ) {
        parts.append( udid );
    }
    if ( !productType.isEmpty() ) {
        parts.append( productType );
    }
    if ( !productVersion.isEmpty() ) {
        parts.append( productVersion );
    }
    return parts.join( QStringLiteral( " " ) );
}

DeviceDiscoveryResult<IosDeviceInfo> protocolError( klogg::livecapture::Generation generation,
                                                    const QString& detail )
{
    const auto nativeDetail = detail.toUtf8();
    return { generation,
             {},
             klogg::livecapture::LiveSourceError{
                 klogg::livecapture::ErrorCategory::Backend, "ios-device-list-protocol-error",
                 klogg::livecapture::ErrorScope::Service,
                 klogg::livecapture::RetryPolicy::Immediate,
                 "The iOS discovery service returned an invalid response; retry discovery.",
                 std::string( nativeDetail.constData(),
                              static_cast<std::size_t>( nativeDetail.size() ) ) } };
}

} // namespace

QString stripAnsiSequences( const QString& text )
{
    if ( text.isEmpty() ) {
        return text;
    }

    QString result;
    result.reserve( text.size() );

    for ( int i = 0; i < text.size(); ) {
        const auto character = text[ i ];
        if ( character.unicode() != Escape || i + 1 >= text.size()
             || text[ i + 1 ].unicode() != Csi ) {
            result.append( character );
            ++i;
            continue;
        }

        int end = i + 2;
        while ( end < text.size() && !isCsiFinalByte( text[ end ] ) ) {
            ++end;
        }

        if ( end >= text.size() ) {
            result.append( character );
            ++i;
            continue;
        }

        i = end + 1;
    }

    return result;
}

QList<IosDeviceInfo> parsePymobiledeviceDeviceList( const QByteArray& output )
{
    // Strip ANSI escape sequences from the raw output before JSON parsing,
    // in case pymobiledevice3 emits colored output that corrupts the JSON
    // structure (e.g. \x1b[32m[{...}]\x1b[0m).
    const auto cleanedOutput = stripAnsiSequences( QString::fromUtf8( output ) ).toUtf8();
    const auto document = QJsonDocument::fromJson( cleanedOutput );
    if ( !document.isArray() ) {
        return {};
    }

    QList<IosDeviceInfo> devices;
    for ( const auto& value : document.array() ) {
        QString udid;
        QString name;
        QString productType;
        QString productVersion;

        if ( value.isString() ) {
            udid = stripAnsiSequences( value.toString().trimmed() );
        }
        else if ( value.isObject() ) {
            const auto object = value.toObject();
            udid = stripAnsiSequences(
                firstStringValue( object, { "Identifier", "UDID", "UniqueDeviceID", "SerialNumber",
                                            "serial", "udid" } ) );
            name = stripAnsiSequences(
                firstStringValue( object, { "DeviceName", "Name", "ProductName", "name" } ) );
            productType = stripAnsiSequences( firstStringValue( object, { "ProductType" } ) );
            productVersion = stripAnsiSequences( firstStringValue( object, { "ProductVersion" } ) );
        }

        if ( udid.isEmpty() ) {
            continue;
        }

        const auto displayName = buildDisplayName( name, udid, productType, productVersion );
        const auto description = name.isEmpty() ? udid : name;
        devices.push_back(
            IosDeviceInfo{ udid, displayName, description, productType, productVersion } );
    }

    return devices;
}

QList<IosDeviceInfo> parsePymobiledeviceSimpleDeviceList( const QByteArray& output )
{
    auto parsed = parsePymobiledeviceDeviceList( output );
    if ( !parsed.isEmpty() ) {
        return parsed;
    }

    QList<IosDeviceInfo> devices;
    const auto lines = QString::fromUtf8( output ).split( '\n' );
    for ( auto& line : lines ) {
        auto cleaned = stripAnsiSequences( line.trimmed() );
        if ( cleaned.isEmpty() || cleaned.startsWith( QLatin1Char( '[' ) )
             || cleaned.startsWith( QLatin1Char( ']' ) ) ) {
            continue;
        }

        devices.push_back( IosDeviceInfo{ cleaned, cleaned, cleaned, {}, {} } );
    }

    return devices;
}

DeviceDiscoveryResult<IosDeviceInfo>
parsePymobiledeviceDeviceDiscovery( klogg::livecapture::Generation generation,
                                    const QByteArray& output )
{
    const auto cleanedOutput = stripAnsiSequences( QString::fromUtf8( output ) ).toUtf8();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson( cleanedOutput, &parseError );
    if ( parseError.error != QJsonParseError::NoError ) {
        return protocolError( generation, parseError.errorString() );
    }
    if ( !document.isArray() ) {
        return protocolError( generation, QStringLiteral( "Expected a JSON device array." ) );
    }

    auto devices = parsePymobiledeviceDeviceList( cleanedOutput );
    if ( !document.array().isEmpty() && devices.isEmpty() ) {
        return protocolError(
            generation, QStringLiteral( "The device array contained no usable identifiers." ) );
    }

    return { generation, std::move( devices ), std::nullopt };
}
