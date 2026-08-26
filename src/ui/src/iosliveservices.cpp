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
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "iosdevicecatalog.h"
#include "iosnativeadapter.h"
#include "iosnativetransport.h"
#include "livelogcontroller.h"

namespace klogg::livecapture::ios {
namespace {

class SerialCatalogExecutor final {
public:
    SerialCatalogExecutor()
        : thread_( [ this ] { run(); } )
    {
    }

    ~SerialCatalogExecutor()
    {
        {
            std::lock_guard<std::mutex> lock( mutex_ );
            stopping_ = true;
        }
        changed_.notify_all();
        if ( thread_.joinable() ) {
            thread_.join();
        }
    }

    void post( IosCatalogTask task )
    {
        {
            std::lock_guard<std::mutex> lock( mutex_ );
            if ( stopping_ ) {
                return;
            }
            tasks_.push_back( std::move( task ) );
        }
        changed_.notify_one();
    }

private:
    void run() noexcept
    {
        for ( ;; ) {
            IosCatalogTask task;
            {
                std::unique_lock<std::mutex> lock( mutex_ );
                changed_.wait( lock, [ this ] { return stopping_ || !tasks_.empty(); } );
                if ( tasks_.empty() ) {
                    if ( stopping_ ) {
                        return;
                    }
                    continue;
                }
                task = std::move( tasks_.front() );
                tasks_.pop_front();
            }
            try {
                task();
            } catch ( ... ) { // NOLINT(bugprone-empty-catch)
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<IosCatalogTask> tasks_;
    bool stopping_{ false };
    std::thread thread_;
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
           && api.osTraceClientNew != nullptr && api.osTraceStart != nullptr
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
        , catalogExecutor_( std::make_unique<SerialCatalogExecutor>() )
    {
        std::string loadError;
        const auto api = loadIosNativeApiFromBundle( config.nativeStackRoot, &loadError );
        auto catalog = std::make_unique<IosDeviceCatalog>(
            api, [ executor = catalogExecutor_.get() ]( IosCatalogTask task ) {
                executor->post( std::move( task ) );
            } );
        static_cast<void>( catalog->start() );
        catalog_ = std::move( catalog );
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
    }

    ~Impl()
    {
        shutdown();
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
            lastConfigurationError_ = LiveSourceError{
                ErrorCategory::Configuration,
                "ios-options-invalid",
                ErrorScope::Stream,
                RetryPolicy::Never,
                "The typed iOS log options are invalid.",
                "The native stream configuration could not be constructed."
            };
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
        if ( auto* nativeCatalog = dynamic_cast<IosDeviceCatalog*>( catalog_.get() ) ) {
            nativeCatalog->stop();
        }
        workerFactory_.reset();
        catalogExecutor_.reset();
    }

    IosLiveServices& owner_;
    std::unique_ptr<SerialCatalogExecutor> catalogExecutor_;
    std::unique_ptr<IosCatalogSnapshotProvider> catalog_;
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
