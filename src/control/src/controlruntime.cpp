#include "controlruntime.h"
#include "controlruntime_pal.h"

#include <algorithm>

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
// Budget physical directory entries as well as admitted live descriptors,
// including files we will never read or remove. Never return a partial list.
constexpr int MaxInspectedEntries = 2 * MaxDescriptors + 2;
QString registryPath( const RuntimeOptions& options )
{
    // Older development builds did not hold kernel leases. Do not interpret or
    // garbage-collect their files, nor let them consume the new registry budget.
    return QDir( options.descriptorRoot ).filePath( QStringLiteral( "epochs-v1" ) );
}
QString identityKey( pal::FileIdentity identity )
{
    return identity.valid ? QString::number( identity.device ) + QLatin1Char( ':' )
                                + QString::number( identity.file ) : QString{};
}

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
    return value.size() == length && std::all_of( value.cbegin(), value.cend(), []( QChar character ) {
        return ( character >= QLatin1Char( '0' ) && character <= QLatin1Char( '9' ) )
               || ( character >= QLatin1Char( 'a' ) && character <= QLatin1Char( 'f' ) );
    } );
}
bool identityText( const QString& value )
{
    return value.size() <= 256 && !value.contains( QChar( 0 ) );
}
QJsonObject object( const InstanceDescriptor& descriptor, pal::FileIdentity file, pal::FileIdentity socket )
{
    return { { QStringLiteral( "descriptorVersion" ), descriptor.descriptorVersion },
             { QStringLiteral( "publishedFile" ), identityKey( file ) },
             { QStringLiteral( "publishedSocket" ), identityKey( socket ) },
             { QStringLiteral( "protocolMin" ), descriptor.protocolMin },
             { QStringLiteral( "protocolMax" ), descriptor.protocolMax },
             { QStringLiteral( "instanceId" ), descriptor.instanceId },
             { QStringLiteral( "installationId" ), descriptor.installationId },
             { QStringLiteral( "profileId" ), descriptor.profileId },
             { QStringLiteral( "endpoint" ), descriptor.endpoint },
             { QStringLiteral( "token" ), descriptor.token } };
}
std::optional<InstanceDescriptor> decode( const QJsonObject& value, const QString& name,
                                          const RuntimeOptions& options )
{
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
         || name != descriptor.instanceId + QStringLiteral( ".instance" )
         || !identityText( descriptor.installationId ) || !identityText( descriptor.profileId )
         || !pal::validEndpoint( descriptor.endpoint, options.socketRoot, descriptor.instanceId ) ) return {};
    return descriptor;
}
bool fail( QString* error, const QString& message )
{
    if ( error ) *error = message;
    return false;
}
// Reap only an immutable epoch record read from its original verified inode.
// Never follow a path supplied by the record, delete a replacement socket, or
// modify a live, malformed, inaccessible, or unrecognized registration.
void retireEpoch( pal::PrivateDirectory& registry, const QString& name, pal::FileIdentity file,
                  const RuntimeOptions& options, const InstanceDescriptor& epoch, const QString& socketKey )
{
    const auto path = pal::socketDirectory( options.socketRoot, epoch.instanceId );
    if ( !path.isEmpty() && !socketKey.isEmpty() && pal::privateEndpoint( epoch.endpoint, epoch.instanceId ) ) {
        pal::PrivateDirectory directory;
        if ( directory.open( path, false ) ) {
            const auto socket = directory.identity( QStringLiteral( "s" ) );
            if ( identityKey( socket ) == socketKey ) {
                directory.removeOwned( QStringLiteral( "s" ), socket );
                directory.removeDirectory();
            }
        }
    }
    registry.removeOwned( name, file );
}
DiscoveryResult inspectEpochs( pal::PrivateDirectory& registry, const RuntimeOptions& options, bool reclaim )
{
    DiscoveryResult result;
    const auto entries = registry.entries( MaxInspectedEntries );
    int liveCount = 0;
    for ( const auto& name : entries.names ) {
        const auto file = registry.read( name, MaxDescriptorBytes );
        if ( !file ) { ++result.rejectedDescriptors; continue; }
        const auto value = QJsonDocument::fromJson( file->bytes ).object();
        const auto descriptor = decode( value, name, options );
        if ( !descriptor || !file->identity.valid || value.value( "publishedFile" ).toString() != identityKey( file->identity ) ) {
            ++result.rejectedDescriptors;
            continue;
        }
        if ( !file->ownerPresent ) {
            if ( reclaim ) retireEpoch( registry, name, file->identity, options, *descriptor,
                                        value.value( "publishedSocket" ).toString() );
            continue;
        }
        ++liveCount;
        if ( reclaim ) continue;
        if ( !options.installationId.isEmpty() && descriptor->installationId != options.installationId ) continue;
        if ( !options.profileId.isEmpty() && descriptor->profileId != options.profileId ) continue;
        result.instances.push_back( *descriptor );
    }
    if ( !entries.complete || liveCount > MaxDescriptors || ( reclaim && liveCount == MaxDescriptors ) ) {
        result.instances.clear();
        result.error = QStringLiteral( "Control discovery limit exceeded or registry unreadable" );
    }
    return result;
}
} // namespace

DiscoveryResult discoverInstances( const RuntimeOptions& requested )
{
    const auto options = resolved( requested );
    DiscoveryResult result;
    pal::PrivateDirectory root;
    // Discovery never creates or deletes resources and never contacts an endpoint.
    if ( !QFileInfo::exists( options.descriptorRoot ) && !QFileInfo( options.descriptorRoot ).isSymLink() ) return result;
    if ( !root.open( options.descriptorRoot, false ) ) {
        result.error = QStringLiteral( "Unsafe control discovery directory" );
        return result;
    }
    const auto path = registryPath( options );
    if ( !QFileInfo::exists( path ) && !QFileInfo( path ).isSymLink() ) return result;
    pal::PrivateDirectory registry;
    if ( !registry.open( path, false ) ) {
        result.error = QStringLiteral( "Unsafe control epoch registry" );
        return result;
    }
    return inspectEpochs( registry, options, false );
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
    pal::PrivateDirectory root;
    if ( !root.open( options.descriptorRoot, true ) || !d_->discovery->open( registryPath( options ), true ) ) {
        clear();
        return fail( error, QStringLiteral( "Unsafe control discovery directory" ) );
    }
    const auto inspected = inspectEpochs( *d_->discovery, options, true );
    if ( !inspected.error.isEmpty() ) {
        clear();
        return fail( error, inspected.error );
    }
    auto& instance = d_->descriptor;
    instance.instanceId = QUuid::createUuid().toString( QUuid::Id128 );
    instance.installationId = options.installationId;
    instance.profileId = options.profileId;
    instance.endpoint = pal::endpoint( options.socketRoot, instance.instanceId );
    QByteArray secret;
    secret.reserve( 32 );
    for ( int i = 0; i < 8; ++i ) {
        const auto random = QRandomGenerator::system()->generate();
        secret.append( reinterpret_cast<const char*>( &random ), sizeof( random ) );
    }
    instance.token = QString::fromLatin1( secret.toHex() );
    d_->name = instance.instanceId + QStringLiteral( ".instance" );
    const auto socketPath = pal::socketDirectory( options.socketRoot, instance.instanceId );
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
         || !d_->discovery->publish( d_->name, [this]( pal::FileIdentity file ) {
             return QJsonDocument( object( d_->descriptor, file, d_->socketIdentity ) ).toJson( QJsonDocument::Compact );
         }, &d_->descriptorIdentity ) )
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
