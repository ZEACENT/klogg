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

#include "iosliveservices.h"

#include <QPointer>

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#include "boundedserialexecutor.h"
#include "iosdevicecatalog.h"
#include "iosnativeadapter.h"
#include "iosnativetransport.h"
#include "livelogcontroller.h"

namespace klogg::livecapture::ios {
namespace {

class CatalogMetadataObservation final {
public:
    explicit CatalogMetadataObservation( IosCatalogMetadataRequester& requester )
        : requester_( &requester )
    {
    }

    void observe( const IosCatalogSnapshot& snapshot )
    {
        std::lock_guard<std::recursive_mutex> lock( mutex_ );
        if ( requester_ == nullptr || snapshot.generation < generation_ ) {
            return;
        }
        if ( snapshot.generation != generation_ ) {
            generation_ = snapshot.generation;
            requested_.clear();
        }

        for ( const auto& entry : snapshot.entries ) {
            if ( entry.metadata.has_value() || entry.error.has_value() ) {
                continue;
            }
            const auto requested = std::find_if(
                requested_.cbegin(), requested_.cend(), [ &entry ]( const RequestedEntry& value ) {
                    return value.endpoint == entry.endpoint && value.epoch == entry.epoch;
                } );
            if ( requested != requested_.cend() ) {
                continue;
            }
            requested_.push_back( RequestedEntry{ entry.endpoint, entry.epoch } );
            requester_->requestMetadata( entry.endpoint );
        }
    }

    void stop()
    {
        std::lock_guard<std::recursive_mutex> lock( mutex_ );
        requester_ = nullptr;
        requested_.clear();
    }

private:
    struct RequestedEntry {
        IosEndpointKey endpoint;
        Generation epoch{ 0u };
    };

    std::recursive_mutex mutex_;
    IosCatalogMetadataRequester* requester_{ nullptr };
    Generation generation_{ 0u };
    std::vector<RequestedEntry> requested_;
};

LiveSourceError invalidLegacyOptions( const LiveSourceTransportConfig& config )
{
    auto detail = std::string{ "Native iOS capture rejected legacy process options: executable=" }
                  + config.executable.toStdString() + ", args=" + config.extraArgs.toStdString();
    return LiveSourceError{ ErrorCategory::Configuration,
                            "ios-legacy-process-options-unsupported",
                            ErrorScope::Infrastructure,
                            RetryPolicy::Never,
                            "Legacy process options are not accepted by native iOS capture.",
                            std::move( detail ) };
}

bool streamApiComplete( const IosNativeApi& api ) noexcept
{
    return api.deviceNewWithOptions != nullptr && api.deviceFree != nullptr
           && api.readPairRecord != nullptr && api.pairRecordFree != nullptr
           && api.lockdownClientNewWithExistingPair != nullptr && api.lockdownClientFree != nullptr
           && api.lockdownStartService != nullptr && api.serviceDescriptorFree != nullptr
           && api.lockdownGetStringValue != nullptr && api.nativeStringFree != nullptr
           && api.osTraceClientNew != nullptr && api.osTraceStartWithRecordType != nullptr
           && api.osTraceStop != nullptr && api.osTraceClientFree != nullptr
           && api.syslogRelayClientNew != nullptr && api.syslogRelayStart != nullptr
           && api.syslogRelayStop != nullptr && api.syslogRelayClientFree != nullptr;
}

LiveSourceError unavailableError()
{
    return LiveSourceError{ ErrorCategory::Configuration,
                            "ios-native-services-unavailable",
                            ErrorScope::Infrastructure,
                            RetryPolicy::Never,
                            "Native iOS services are unavailable.",
                            "The application-owned iOS composition root has shut down." };
}

} // namespace

class IosLiveServices::Impl final {
public:
    explicit Impl( IosLiveServices& owner, IosLiveServicesConfig config )
        : owner_( owner )
        , catalogExecutor_( std::make_shared<BoundedSerialExecutor>(
              config.catalogShutdownDeadline ) )
    {
        std::string loadError;
        const auto api = loadIosNativeApiFromBundle( config.nativeStackRoot, &loadError );
        auto catalog = std::make_unique<IosDeviceCatalog>(
            api, [ executor = catalogExecutor_ ]( IosCatalogTask task ) {
                executor->post( std::move( task ) );
            } );
        auto* const nativeCatalog = catalog.get();
        catalog_ = std::move( catalog );
        startMetadataObservation();
        static_cast<void>( nativeCatalog->start() );
        if ( streamApiComplete( api ) ) {
            workerFactory_ = std::make_unique<DefaultIosNativeStreamWorkerFactory>( api );
        }
        else {
            lastConfigurationError_
                = LiveSourceError{ ErrorCategory::Configuration,
                                   "ios-native-stack-unavailable",
                                   ErrorScope::Infrastructure,
                                   RetryPolicy::Never,
                                   "The bundled native iOS stack is unavailable.",
                                   std::move( loadError ) };
        }
    }

    Impl( IosLiveServices& owner, std::unique_ptr<IosCatalogSnapshotProvider> catalog,
          std::unique_ptr<IosNativeStreamWorkerFactory> workerFactory )
        : owner_( owner )
        , catalog_( std::move( catalog ) )
        , workerFactory_( std::move( workerFactory ) )
    {
        startMetadataObservation();
    }

    ~Impl()
    {
        shutdown();
    }

    void startMetadataObservation()
    {
        auto* const requester = dynamic_cast<IosCatalogMetadataRequester*>( catalog_.get() );
        if ( requester == nullptr ) {
            return;
        }

        metadataObservation_ = std::make_shared<CatalogMetadataObservation>( *requester );
        const auto observation = metadataObservation_;
        metadataSubscription_
            = catalog_->subscribe( [ observation ]( const IosCatalogSnapshot& snapshot ) {
                  observation->observe( snapshot );
              } );
        observation->observe( catalog_->snapshot() );
    }

    void stopMetadataObservation()
    {
        if ( metadataObservation_ != nullptr ) {
            metadataObservation_->stop();
        }
        if ( catalog_ != nullptr && metadataSubscription_.has_value() ) {
            catalog_->unsubscribe( *metadataSubscription_ );
            metadataSubscription_.reset();
        }
        metadataObservation_.reset();
    }

    std::unique_ptr<LiveSourceTransport> create( const LiveSourceTransportConfig& config ) const
    {
        if ( config.sourceType != LiveLogSourceType::IosLogStream ) {
            return nullptr;
        }
        if ( shuttingDown_ ) {
            lastConfigurationError_ = unavailableError();
            return nullptr;
        }
        if ( config.iosBackend == IosTransportBackend::LegacyProcess ) {
            // Compatibility is intentionally opt-in. Only a persisted legacy
            // backend selection can reach the old process implementation.
            return legacyFactory_.create( config );
        }
        if ( workerFactory_ == nullptr ) {
            if ( !lastConfigurationError_.has_value() ) {
                lastConfigurationError_ = unavailableError();
            }
            return nullptr;
        }
        if ( !config.executable.trimmed().isEmpty() || !config.extraArgs.trimmed().isEmpty() ) {
            lastConfigurationError_ = invalidLegacyOptions( config );
            return nullptr;
        }
        if ( config.iosEndpoint.udid.empty()
             || ( config.iosEndpoint.connectionType != NativeConnectionType::Usb
                  && config.iosEndpoint.connectionType != NativeConnectionType::Network ) ) {
            lastConfigurationError_ = LiveSourceError{
                ErrorCategory::Configuration,
                "ios-endpoint-required",
                ErrorScope::Device,
                RetryPolicy::Never,
                "Select an iOS device endpoint before starting capture.",
                "The native transport requires an explicit UDID and supported connection type."
            };
            return nullptr;
        }

        auto nativeConfig = klogg::livelog::makeIosNativeStreamConfig( config );
        if ( !nativeConfig.has_value() ) {
            lastConfigurationError_
                = LiveSourceError{ ErrorCategory::Configuration,
                                   "ios-options-invalid",
                                   ErrorScope::Stream,
                                   RetryPolicy::Never,
                                   "The typed iOS log options are invalid.",
                                   "The native stream configuration could not be constructed." };
            return nullptr;
        }
        auto transport
            = std::make_unique<IosNativeTransport>( *workerFactory_, std::move( *nativeConfig ) );
        auto* const raw = transport.get();
        transports_.push_back( raw );
        QObject::connect( raw, &QObject::destroyed, &owner_, [ this, raw ] {
            const auto found = std::find( transports_.begin(), transports_.end(), raw );
            if ( found != transports_.end() ) {
                transports_.erase( found );
            }
        } );
        lastConfigurationError_.reset();
        return transport;
    }

    void shutdown()
    {
        if ( shuttingDown_ ) {
            return;
        }
        shuttingDown_ = true;
        while ( !transports_.empty() ) {
            QPointer<IosNativeTransport> transport( transports_.back() );
            transports_.pop_back();
            if ( transport != nullptr ) {
                transport->serviceShutdown();
            }
        }
        stopMetadataObservation();
        if ( auto* nativeCatalog = dynamic_cast<IosDeviceCatalog*>( catalog_.get() ) ) {
            nativeCatalog->stop();
        }
        workerFactory_.reset();
        if ( catalogExecutor_ != nullptr ) {
            catalogExecutor_->shutdownAsync();
            catalogExecutor_.reset();
        }
    }

    IosLiveServices& owner_;
    std::shared_ptr<BoundedSerialExecutor> catalogExecutor_;
    std::unique_ptr<IosCatalogSnapshotProvider> catalog_;
    std::shared_ptr<CatalogMetadataObservation> metadataObservation_;
    std::optional<IosCatalogSnapshotProvider::SubscriptionId> metadataSubscription_;
    std::unique_ptr<IosNativeStreamWorkerFactory> workerFactory_;
    mutable DefaultLiveSourceTransportFactory legacyFactory_;
    mutable std::optional<LiveSourceError> lastConfigurationError_;
    mutable std::vector<IosNativeTransport*> transports_;
    bool shuttingDown_{ false };
};

IosLiveServices::IosLiveServices( IosLiveServicesConfig config, QObject* parent )
    : QObject( parent )
    , impl_( std::make_unique<Impl>( *this, std::move( config ) ) )
{
}

IosLiveServices::IosLiveServices( std::unique_ptr<IosCatalogSnapshotProvider> catalog,
                                  std::unique_ptr<IosNativeStreamWorkerFactory> workerFactory,
                                  QObject* parent )
    : QObject( parent )
    , impl_( std::make_unique<Impl>( *this, std::move( catalog ), std::move( workerFactory ) ) )
{
}

IosLiveServices::~IosLiveServices() = default;

IosCatalogSnapshotProvider& IosLiveServices::catalogProvider() noexcept
{
    return *impl_->catalog_;
}

const IosCatalogSnapshotProvider& IosLiveServices::catalogProvider() const noexcept
{
    return *impl_->catalog_;
}

std::unique_ptr<LiveSourceTransport>
IosLiveServices::create( const LiveSourceTransportConfig& config ) const
{
    return impl_->create( config );
}

std::optional<LiveSourceError> IosLiveServices::lastConfigurationError() const
{
    return impl_->lastConfigurationError_;
}

void IosLiveServices::shutdown()
{
    impl_->shutdown();
}

} // namespace klogg::livecapture::ios
