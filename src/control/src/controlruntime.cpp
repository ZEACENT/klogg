#include "controlruntime.h"
#include "controlruntime_pal.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QUuid>

namespace klogg::control {
namespace {
constexpr int MaxDescriptorBytes = 8192;
constexpr int MaxDescriptors = 256;

RuntimeOptions resolved( RuntimeOptions options )
{
    if ( options.descriptorRoot.isEmpty() ) {
        options.descriptorRoot = qEnvironmentVariable( "KLOGG_CONTROL_DESCRIPTOR_ROOT" );
        if ( options.descriptorRoot.isEmpty() ) options.descriptorRoot = pal::defaultDescriptorRoot();
    }
    if ( options.socketRoot.isEmpty() ) options.socketRoot = pal::defaultSocketRoot();
    return options;
}
bool hex( const QString& value, int length )
{
    if ( value.size() != length ) return false;
    for ( const auto character : value ) {
        if ( !( character >= QLatin1Char( '0' ) && character <= QLatin1Char( '9' ) )
             && !( character >= QLatin1Char( 'a' ) && character <= QLatin1Char( 'f' ) ) ) return false;
    }
    return true;
}
bool identityText( const QString& value )
{
    return value.size() <= 256 && !value.contains( QChar( 0 ) );
}
QJsonObject object( const InstanceDescriptor& descriptor )
{
    return { { QStringLiteral( "descriptorVersion" ), descriptor.descriptorVersion },
             { QStringLiteral( "protocolMin" ), descriptor.protocolMin },
             { QStringLiteral( "protocolMax" ), descriptor.protocolMax },
             { QStringLiteral( "instanceId" ), descriptor.instanceId },
             { QStringLiteral( "installationId" ), descriptor.installationId },
             { QStringLiteral( "profileId" ), descriptor.profileId },
             { QStringLiteral( "endpoint" ), descriptor.endpoint },
             { QStringLiteral( "token" ), descriptor.token } };
}
std::optional<InstanceDescriptor> decode( const QByteArray& bytes, const QString& name,
                                          const RuntimeOptions& options )
{
    const auto document = QJsonDocument::fromJson( bytes );
    if ( !document.isObject() ) return {};
    const auto value = document.object();
    InstanceDescriptor descriptor;
    if ( value.value( "descriptorVersion" ) != QJsonValue( 1 )
         || value.value( "protocolMin" ) != QJsonValue( ControlProtocolVersion )
         || value.value( "protocolMax" ) != QJsonValue( ControlProtocolVersion ) ) return {};
    for ( const auto* key : { "instanceId", "installationId", "profileId", "endpoint", "token" } ) {
        if ( !value.value( QLatin1String( key ) ).isString() ) return {};
    }
    descriptor.instanceId = value.value( "instanceId" ).toString();
    descriptor.installationId = value.value( "installationId" ).toString();
    descriptor.profileId = value.value( "profileId" ).toString();
    descriptor.endpoint = value.value( "endpoint" ).toString();
    descriptor.token = value.value( "token" ).toString();
    if ( !hex( descriptor.instanceId, 32 ) || !hex( descriptor.token, 64 )
         || name != descriptor.instanceId + QStringLiteral( ".json" )
         || !identityText( descriptor.installationId ) || !identityText( descriptor.profileId )
         || !pal::validEndpoint( descriptor.endpoint, options.socketRoot, descriptor.instanceId ) ) return {};
    return descriptor;
}
bool fail( QString* error, const QString& message )
{
    if ( error ) *error = message;
    return false;
}
} // namespace

DiscoveryResult discoverInstances( const RuntimeOptions& requested )
{
    const auto options = resolved( requested );
    DiscoveryResult result;
    pal::PrivateDirectory directory;
    // Discovery never creates resources. An absent root means no enabled server.
    if ( !QFileInfo::exists( options.descriptorRoot ) && !QFileInfo( options.descriptorRoot ).isSymLink() ) return result;
    if ( !directory.open( options.descriptorRoot, false ) ) {
        result.error = QStringLiteral( "Unsafe control discovery directory" );
        return result;
    }
    const auto entries = directory.entries( MaxDescriptors + 1 );
    if ( entries.size() > MaxDescriptors ) {
        result.error = QStringLiteral( "Control discovery limit exceeded" );
        return result;
    }
    for ( const auto& name : entries ) {
        const auto bytes = directory.read( name, MaxDescriptorBytes );
        const auto descriptor = bytes ? decode( *bytes, name, options ) : std::nullopt;
        if ( !descriptor ) { ++result.rejectedDescriptors; continue; }
        if ( !options.installationId.isEmpty() && descriptor->installationId != options.installationId ) continue;
        if ( !options.profileId.isEmpty() && descriptor->profileId != options.profileId ) continue;
        result.instances.push_back( *descriptor );
    }
    return result;
}

struct ControlRuntime::Private {
    InstanceDescriptor descriptor;
    std::unique_ptr<pal::PrivateDirectory> discovery;
    std::unique_ptr<pal::PrivateDirectory> socket;
    pal::FileIdentity descriptorIdentity;
    pal::FileIdentity socketIdentity;
    QString name;
};
ControlRuntime::ControlRuntime() : d_( std::make_unique<Private>() ) {}
ControlRuntime::~ControlRuntime() { clear(); }
void ControlRuntime::clear()
{
    if ( d_->discovery ) d_->discovery->removeOwned( d_->name, d_->descriptorIdentity );
    if ( d_->socket ) {
        d_->socket->removeOwned( QStringLiteral( "s" ), d_->socketIdentity );
        d_->socket->removeDirectory();
    }
    *d_ = Private{};
}
bool ControlRuntime::prepare( const RuntimeOptions& requested, QString* error )
{
    clear();
    if ( error ) error->clear();
    const auto options = resolved( requested );
    if ( !identityText( options.installationId ) || !identityText( options.profileId ) )
        return fail( error, QStringLiteral( "Invalid control identity" ) );
    d_->discovery = std::make_unique<pal::PrivateDirectory>();
    if ( !d_->discovery->open( options.descriptorRoot, true ) ) {
        clear();
        return fail( error, QStringLiteral( "Unsafe control discovery directory" ) );
    }
    auto& descriptor = d_->descriptor;
    descriptor.instanceId = QUuid::createUuid().toString( QUuid::Id128 );
    descriptor.installationId = options.installationId;
    descriptor.profileId = options.profileId;
    descriptor.endpoint = pal::endpoint( options.socketRoot, descriptor.instanceId );
    QByteArray secret;
    secret.reserve( 32 );
    for ( int i = 0; i < 8; ++i ) {
        const auto random = QRandomGenerator::system()->generate();
        secret.append( reinterpret_cast<const char*>( &random ), sizeof( random ) );
    }
    descriptor.token = QString::fromLatin1( secret.toHex() );
    d_->name = descriptor.instanceId + QStringLiteral( ".json" );
    const auto socketPath = pal::socketDirectory( options.socketRoot, descriptor.instanceId );
    if ( !socketPath.isEmpty() ) {
        d_->socket = std::make_unique<pal::PrivateDirectory>();
        // Random identity never reuses an existing endpoint directory.
        if ( QFileInfo::exists( socketPath ) || QFileInfo( socketPath ).isSymLink()
             || !d_->socket->open( socketPath, true ) ) {
            clear();
            return fail( error, QStringLiteral( "Unsafe control socket directory" ) );
        }
    }
    return true;
}
bool ControlRuntime::publish( QString* error )
{
    if ( !d_->discovery || d_->descriptorIdentity.valid
         || !d_->discovery->publish( d_->name, QJsonDocument( object( d_->descriptor ) ).toJson( QJsonDocument::Compact ),
                                     &d_->descriptorIdentity ) )
        return fail( error, QStringLiteral( "Cannot publish control descriptor" ) );
    return true;
}
const InstanceDescriptor& ControlRuntime::descriptor() const { return d_->descriptor; }
QString ControlRuntime::descriptorPath() const
{
    return d_->discovery ? QDir( d_->discovery->path() ).filePath( d_->name ) : QString{};
}
qintptr ControlRuntime::createListener( QString* error )
{
    const auto listener = pal::createListener( d_->descriptor.endpoint, &d_->socketIdentity );
    if ( d_->socket && listener < 0 )
        fail( error, QStringLiteral( "Cannot create local control socket" ) );
    return listener;
}
void ControlRuntime::closeListener( qintptr descriptor ) { pal::closeListener( descriptor ); }
} // namespace klogg::control
