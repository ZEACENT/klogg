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

#include <catch2/catch.hpp>

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QLabel>
#include <QPushButton>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "adbdevicelistprovider.h"
#include "adblogcatdialog.h"
#include "devicelistprovider.h"
#include "iosdeviceparser.h"
#include "ioslogdialog.h"
#include "livestate.h"

namespace {

using klogg::livecapture::ErrorCategory;
using klogg::livecapture::ErrorScope;
using klogg::livecapture::Generation;
using klogg::livecapture::LiveSourceError;
using klogg::livecapture::RetryPolicy;

template <typename Predicate>
void drainEventsUntil( Predicate predicate )
{
    for ( int attempt = 0; attempt < 10000 && !predicate(); ++attempt ) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents( QEventLoop::AllEvents );
        QThread::yieldCurrentThread();
    }
}

LiveSourceError actionableError( ErrorCategory category, ErrorScope scope, RetryPolicy retryPolicy,
                                 const char* code, const char* message )
{
    return LiveSourceError{ category, code, scope, retryPolicy, message, "native detail" };
}

struct ProviderGate {
    QSemaphore entered;
    QSemaphore release;
    QString device;
    std::optional<LiveSourceError> error;
};

class ControlledStringProvider final : public DeviceListProviderBase<QString> {
public:
    explicit ControlledStringProvider( std::shared_ptr<ProviderGate> gate )
        : DeviceListProviderBase<QString>( [ gate ]( Generation generation ) {
            gate->entered.release();
            gate->release.acquire();
            return DeviceDiscoveryResult<QString>{ generation,
                                                   gate->device.isEmpty()
                                                       ? QList<QString>{}
                                                       : QList<QString>{ gate->device },
                                                   gate->error };
        } )
    {
    }

protected:
    QList<QString> doListDevices( QString* ) const override
    {
        return {};
    }

    bool deviceMatches( const QString& device, const QString& deviceId ) const override
    {
        return device == deviceId;
    }
};

class MisreportingStringProvider final : public DeviceListProviderBase<QString> {
public:
    MisreportingStringProvider()
        : DeviceListProviderBase<QString>( []( Generation ) {
            return DeviceDiscoveryResult<QString>{ Generation{ 999 },
                                                   { QStringLiteral( "device" ) },
                                                   std::nullopt };
        } )
    {
    }

protected:
    QList<QString> doListDevices( QString* ) const override
    {
        return {};
    }

    bool deviceMatches( const QString& device, const QString& deviceId ) const override
    {
        return device == deviceId;
    }
};

template <typename DeviceInfo>
class ControlledDiscoverySlot {
public:
    template <typename Update>
    void updateResult( Update&& update )
    {
        const std::lock_guard<std::mutex> lock( resultMutex_ );
        update( result_ );
    }

    DeviceDiscoveryResult<DeviceInfo> result() const
    {
        const std::lock_guard<std::mutex> lock( resultMutex_ );
        return result_;
    }

    void setDevices( QList<DeviceInfo> devices )
    {
        updateResult( [ &devices ]( auto& result ) {
            result.devices = std::move( devices );
        } );
    }

    void setError( klogg::livecapture::LiveSourceError error )
    {
        updateResult( [ &error ]( auto& result ) {
            result.error = std::move( error );
        } );
    }

    QSemaphore entered;
    QSemaphore release;
    QSemaphore completed;

private:
    mutable std::mutex resultMutex_;
    DeviceDiscoveryResult<DeviceInfo> result_;
};

template <typename DeviceInfo>
class ControlledDiscoveryOperations {
public:
    explicit ControlledDiscoveryOperations( std::size_t count )
    {
        slots_.reserve( count );
        for ( std::size_t i = 0; i < count; ++i ) {
            slots_.push_back( std::make_unique<ControlledDiscoverySlot<DeviceInfo>>() );
        }
    }

    DeviceDiscoveryResult<DeviceInfo> run( Generation generation )
    {
        const auto index = nextSlot_.fetch_add( 1 );
        auto& slot = *slots_.at( index );
        slot.updateResult( [ generation ]( auto& result ) {
            result.generation = generation;
        } );
        slot.entered.release();
        slot.release.acquire();
        auto result = slot.result();
        slot.completed.release();
        return result;
    }

    ControlledDiscoverySlot<DeviceInfo>& slot( std::size_t index )
    {
        return *slots_.at( index );
    }

private:
    std::atomic<std::size_t> nextSlot_{ 0 };
    std::vector<std::unique_ptr<ControlledDiscoverySlot<DeviceInfo>>> slots_;
};

AdbDeviceInfo adbDevice( const QString& serial, const QString& stateText )
{
    auto state = AdbDeviceState::Other;
    if ( stateText == QStringLiteral( "device" ) ) {
        state = AdbDeviceState::Online;
    }
    else if ( stateText == QStringLiteral( "unauthorized" ) ) {
        state = AdbDeviceState::Unauthorized;
    }
    else if ( stateText == QStringLiteral( "offline" ) ) {
        state = AdbDeviceState::Offline;
    }
    return AdbDeviceInfo{ serial, serial, serial, state, stateText };
}

} // namespace

TEST_CASE( "asynchronous device discovery preserves generation devices and actionable error",
           "[device-discovery][generation][error]" )
{
    auto gate = std::make_shared<ProviderGate>();
    gate->error = actionableError( ErrorCategory::Backend, ErrorScope::Service,
                                   RetryPolicy::Immediate, "discovery-command-failed",
                                   "Install or configure the device discovery executable." );

    ControlledStringProvider provider( gate );
    auto future = provider.listDevicesAsync( Generation{ 41 } );
    REQUIRE( gate->entered.tryAcquire( 1, 3000 ) );
    gate->release.release();
    future.waitForFinished();

    const auto result = future.result();
    REQUIRE( result.generation == Generation{ 41 } );
    REQUIRE( result.devices.isEmpty() );
    REQUIRE( result.error.has_value() );
    CHECK( result.error->category == ErrorCategory::Backend );
    CHECK( result.error->code == "discovery-command-failed" );
    CHECK( result.error->retryPolicy == RetryPolicy::Immediate );
    CHECK_FALSE( result.error->message.empty() );
}

TEST_CASE( "asynchronous provider work remains valid after provider destruction",
           "[device-discovery][lifetime]" )
{
    auto gate = std::make_shared<ProviderGate>();
    gate->device = QStringLiteral( "snapshot-device" );

    QFuture<DeviceDiscoveryResult<QString>> future;
    {
        ControlledStringProvider provider( gate );
        future = provider.listDevicesAsync( Generation{ 7 } );
        REQUIRE( gate->entered.tryAcquire( 1, 3000 ) );
    }

    gate->release.release();
    future.waitForFinished();
    REQUIRE( future.result().generation == Generation{ 7 } );
    REQUIRE( future.result().devices == QList<QString>{ QStringLiteral( "snapshot-device" ) } );
}

TEST_CASE( "discovery request owns the generation reported by provider work",
           "[device-discovery][provider][generation]" )
{
    MisreportingStringProvider provider;
    auto future = provider.listDevicesAsync( Generation{ 17 } );
    future.waitForFinished();

    REQUIRE( future.result().generation == Generation{ 17 } );
    REQUIRE( future.result().devices == QList<QString>{ QStringLiteral( "device" ) } );
}

TEST_CASE( "shared discovery coordinator accepts only the current generation snapshot",
           "[device-discovery][coordinator][generation]" )
{
    DeviceDiscoveryCoordinator<QString> coordinator;
    const auto firstGeneration = coordinator.beginRefresh();
    const auto secondGeneration = coordinator.beginRefresh();

    REQUIRE( firstGeneration > 0 );
    REQUIRE( secondGeneration == firstGeneration + 1 );
    REQUIRE( coordinator.currentGeneration() == secondGeneration );

    const auto currentError
        = actionableError( ErrorCategory::Backend, ErrorScope::Service, RetryPolicy::Immediate,
                           "current-error", "Retry discovery." );
    REQUIRE( coordinator.accept( DeviceDiscoveryResult<QString>{
        secondGeneration, { QStringLiteral( "current" ) }, currentError } ) );
    REQUIRE( coordinator.currentDevices() == QList<QString>{ QStringLiteral( "current" ) } );
    REQUIRE( coordinator.currentError().has_value() );
    REQUIRE( coordinator.currentError()->code == "current-error" );

    CHECK_FALSE( coordinator.accept( DeviceDiscoveryResult<QString>{
        firstGeneration, { QStringLiteral( "stale" ) }, std::nullopt } ) );
    CHECK( coordinator.currentGeneration() == secondGeneration );
    CHECK( coordinator.currentDevices() == QList<QString>{ QStringLiteral( "current" ) } );
    REQUIRE( coordinator.currentError().has_value() );
    CHECK( coordinator.currentError()->code == "current-error" );
}

TEST_CASE( "Android parser retains online unauthorized and offline devices with explicit state",
           "[device-discovery][adb][parsing]" )
{
    const QByteArray output
        = "List of devices attached\n"
          "online-1 device product:foo model:Pixel_9 device:tokay transport_id:1\n"
          "locked-2 unauthorized usb:1-2 transport_id:2\n"
          "sleeping-3 offline transport_id:3\n";

    const auto devices = parseAdbDeviceListOutput( output );
    REQUIRE( devices.size() == 3 );
    CHECK( devices.at( 0 ).serial == QStringLiteral( "online-1" ) );
    CHECK( devices.at( 0 ).state == AdbDeviceState::Online );
    CHECK( devices.at( 0 ).stateText == QStringLiteral( "device" ) );
    CHECK( devices.at( 1 ).serial == QStringLiteral( "locked-2" ) );
    CHECK( devices.at( 1 ).state == AdbDeviceState::Unauthorized );
    CHECK( devices.at( 1 ).stateText == QStringLiteral( "unauthorized" ) );
    CHECK( devices.at( 2 ).serial == QStringLiteral( "sleeping-3" ) );
    CHECK( devices.at( 2 ).state == AdbDeviceState::Offline );
    CHECK( devices.at( 2 ).stateText == QStringLiteral( "offline" ) );
}

TEST_CASE( "ADB parser ignores daemon banners before the device-list header",
           "[device-discovery][adb][parsing]" )
{
    const QByteArray output = "* daemon not running; starting now at tcp:5037\n"
                              "* daemon started successfully\n"
                              "List of devices attached\n";

    CHECK( parseAdbDeviceListOutput( output ).isEmpty() );
}

TEST_CASE( "ADB discovery distinguishes no devices from malformed protocol output",
           "[device-discovery][adb][parsing][error]" )
{
    const auto noDevices
        = parseAdbDeviceDiscovery( Generation{ 6 }, QByteArray{ "List of devices attached\n" } );
    REQUIRE( noDevices.generation == Generation{ 6 } );
    REQUIRE( noDevices.devices.isEmpty() );
    REQUIRE_FALSE( noDevices.error.has_value() );

    const auto protocolFailure = parseAdbDeviceDiscovery(
        Generation{ 7 }, QByteArray{ "unexpected executable output\n" } );
    REQUIRE( protocolFailure.generation == Generation{ 7 } );
    REQUIRE( protocolFailure.devices.isEmpty() );
    REQUIRE( protocolFailure.error.has_value() );
    CHECK( protocolFailure.error->category == ErrorCategory::Backend );
    CHECK( protocolFailure.error->code == "adb-device-list-protocol-error" );

    const auto forgedHeader = parseAdbDeviceDiscovery(
        Generation{ 8 }, QByteArray{ "List of devices attached unexpectedly\n" } );
    REQUIRE( forgedHeader.error.has_value() );
    CHECK( forgedHeader.error->code == "adb-device-list-protocol-error" );
}

TEST_CASE( "iOS parser distinguishes a valid empty list from malformed protocol output",
           "[device-discovery][ios][parsing][error]" )
{
    const auto noDevices
        = parsePymobiledeviceDeviceDiscovery( Generation{ 3 }, QByteArray{ "[]" } );
    REQUIRE( noDevices.generation == Generation{ 3 } );
    REQUIRE( noDevices.devices.isEmpty() );
    REQUIRE_FALSE( noDevices.error.has_value() );

    const auto protocolFailure
        = parsePymobiledeviceDeviceDiscovery( Generation{ 4 }, QByteArray{ "not-json" } );
    REQUIRE( protocolFailure.generation == Generation{ 4 } );
    REQUIRE( protocolFailure.devices.isEmpty() );
    REQUIRE( protocolFailure.error.has_value() );
    CHECK( protocolFailure.error->category == ErrorCategory::Backend );
    CHECK( protocolFailure.error->code == "ios-device-list-protocol-error" );
    CHECK_FALSE( protocolFailure.error->message.empty() );
}

TEST_CASE( "iOS parser rejects non-empty JSON without a usable device identity",
           "[device-discovery][ios][parsing][error]" )
{
    const auto protocolFailure = parsePymobiledeviceDeviceDiscovery(
        Generation{ 5 }, QByteArray{ "[{\"DeviceName\":\"Missing identifier\"}]" } );

    REQUIRE( protocolFailure.generation == Generation{ 5 } );
    REQUIRE( protocolFailure.devices.isEmpty() );
    REQUIRE( protocolFailure.error.has_value() );
    CHECK( protocolFailure.error->category == ErrorCategory::Backend );
    CHECK( protocolFailure.error->code == "ios-device-list-protocol-error" );
}

TEST_CASE( "ADB duplicate refresh cannot replace current devices or re-enable Accept",
           "[device-discovery][adb][dialog][generation]" )
{
    auto operations = std::make_shared<ControlledDiscoveryOperations<AdbDeviceInfo>>( 2 );
    DeviceListProviderBase<AdbDeviceInfo>::AsyncListOperation operation
        = [ operations ]( Generation generation ) { return operations->run( generation ); };
    AdbLogcatDialog dialog( operation );

    REQUIRE( operations->slot( 0 ).entered.tryAcquire( 1, 3000 ) );
    REQUIRE( QMetaObject::invokeMethod( &dialog, "refreshDevices", Qt::DirectConnection ) );
    REQUIRE( operations->slot( 1 ).entered.tryAcquire( 1, 3000 ) );
    const auto firstGeneration = operations->slot( 0 ).result().generation;
    const auto secondGeneration = operations->slot( 1 ).result().generation;
    REQUIRE( firstGeneration > 0 );
    REQUIRE( secondGeneration == firstGeneration + 1 );

    operations->slot( 1 ).updateResult( []( auto& result ) {
        result.devices
            = { adbDevice( QStringLiteral( "locked-current" ), QStringLiteral( "unauthorized" ) ),
                adbDevice( QStringLiteral( "offline-current" ), QStringLiteral( "offline" ) ) };
    } );
    operations->slot( 1 ).release.release();
    REQUIRE( operations->slot( 1 ).completed.tryAcquire( 1, 3000 ) );

    auto* combo = dialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
    auto* buttons = dialog.findChild<QDialogButtonBox*>( QStringLiteral( "buttonBox" ) );
    REQUIRE( combo != nullptr );
    REQUIRE( buttons != nullptr );
    REQUIRE( buttons->button( QDialogButtonBox::Ok ) != nullptr );
    drainEventsUntil( [ combo ] { return combo->count() == 2; } );
    REQUIRE( combo->count() == 2 );
    CHECK( combo->itemText( 0 ).contains( QStringLiteral( "locked-current" ) ) );
    CHECK( combo->itemText( 1 ).contains( QStringLiteral( "offline-current" ) ) );
    CHECK_FALSE( buttons->button( QDialogButtonBox::Ok )->isEnabled() );

    operations->slot( 0 ).updateResult( []( auto& result ) {
        result.devices
            = { adbDevice( QStringLiteral( "stale-online" ), QStringLiteral( "device" ) ) };
    } );
    operations->slot( 0 ).release.release();
    REQUIRE( operations->slot( 0 ).completed.tryAcquire( 1, 3000 ) );
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();

    CHECK( combo->count() == 2 );
    CHECK( combo->itemText( 0 ).contains( QStringLiteral( "locked-current" ) ) );
    CHECK_FALSE( buttons->button( QDialogButtonBox::Ok )->isEnabled() );
}

TEST_CASE( "refresh remains disabled until every overlapping request finishes",
           "[device-discovery][adb][dialog][generation]" )
{
    auto operations = std::make_shared<ControlledDiscoveryOperations<AdbDeviceInfo>>( 2 );
    DeviceListProviderBase<AdbDeviceInfo>::AsyncListOperation operation
        = [ operations ]( Generation generation ) { return operations->run( generation ); };
    AdbLogcatDialog dialog( operation );

    auto* refresh = dialog.findChild<QPushButton*>( QStringLiteral( "refreshDevicesButton" ) );
    auto* combo = dialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
    REQUIRE( refresh != nullptr );
    REQUIRE( combo != nullptr );
    REQUIRE( operations->slot( 0 ).entered.tryAcquire( 1, 3000 ) );
    CHECK_FALSE( refresh->isEnabled() );

    REQUIRE( QMetaObject::invokeMethod( &dialog, "refreshDevices", Qt::DirectConnection ) );
    REQUIRE( operations->slot( 1 ).entered.tryAcquire( 1, 3000 ) );
    operations->slot( 1 ).updateResult( []( auto& result ) {
        result.devices
            = { adbDevice( QStringLiteral( "current-online" ), QStringLiteral( "device" ) ) };
    } );
    operations->slot( 1 ).release.release();
    REQUIRE( operations->slot( 1 ).completed.tryAcquire( 1, 3000 ) );
    drainEventsUntil( [ combo ] { return combo->count() == 1; } );
    REQUIRE( combo->count() == 1 );
    CHECK_FALSE( refresh->isEnabled() );

    operations->slot( 0 ).release.release();
    REQUIRE( operations->slot( 0 ).completed.tryAcquire( 1, 3000 ) );
    drainEventsUntil( [ refresh ] { return refresh->isEnabled(); } );
    CHECK( refresh->isEnabled() );
}

TEST_CASE( "disabled refresh controls do not enqueue duplicate discovery work",
           "[device-discovery][dialog][refresh]" )
{
    SECTION( "ADB" )
    {
        auto operations = std::make_shared<ControlledDiscoveryOperations<AdbDeviceInfo>>( 2 );
        DeviceListProviderBase<AdbDeviceInfo>::AsyncListOperation operation
            = [ operations ]( Generation generation ) { return operations->run( generation ); };
        AdbLogcatDialog dialog( operation );

        auto* refresh = dialog.findChild<QPushButton*>( QStringLiteral( "refreshDevicesButton" ) );
        REQUIRE( refresh != nullptr );
        REQUIRE( operations->slot( 0 ).entered.tryAcquire( 1, 3000 ) );
        REQUIRE_FALSE( refresh->isEnabled() );

        for ( auto click = 0; click < 10; ++click ) {
            refresh->click();
        }
        const auto duplicateStarted = operations->slot( 1 ).entered.tryAcquire();
        if ( duplicateStarted ) {
            operations->slot( 1 ).release.release();
            REQUIRE( operations->slot( 1 ).completed.tryAcquire( 1, 3000 ) );
        }
        CHECK_FALSE( duplicateStarted );

        operations->slot( 0 ).release.release();
        REQUIRE( operations->slot( 0 ).completed.tryAcquire( 1, 3000 ) );
        drainEventsUntil( [ refresh ] { return refresh->isEnabled(); } );
        CHECK( refresh->isEnabled() );
    }

    SECTION( "iOS" )
    {
        auto operations = std::make_shared<ControlledDiscoveryOperations<IosDeviceInfo>>( 2 );
        DeviceListProviderBase<IosDeviceInfo>::AsyncListOperation operation
            = [ operations ]( Generation generation ) { return operations->run( generation ); };
        IosLogDialog dialog( operation );
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();

        auto* refresh = dialog.findChild<QPushButton*>( QStringLiteral( "refreshDevicesButton" ) );
        REQUIRE( refresh != nullptr );
        REQUIRE( operations->slot( 0 ).entered.tryAcquire( 1, 3000 ) );
        REQUIRE_FALSE( refresh->isEnabled() );

        for ( auto click = 0; click < 10; ++click ) {
            refresh->click();
        }
        const auto duplicateStarted = operations->slot( 1 ).entered.tryAcquire();
        if ( duplicateStarted ) {
            operations->slot( 1 ).release.release();
            REQUIRE( operations->slot( 1 ).completed.tryAcquire( 1, 3000 ) );
        }
        CHECK_FALSE( duplicateStarted );

        operations->slot( 0 ).release.release();
        REQUIRE( operations->slot( 0 ).completed.tryAcquire( 1, 3000 ) );
        drainEventsUntil( [ refresh ] { return refresh->isEnabled(); } );
        CHECK( refresh->isEnabled() );
    }
}

TEST_CASE( "dialog request owns generation reported by injected discovery work",
           "[device-discovery][adb][dialog][generation]" )
{
    auto operations = std::make_shared<ControlledDiscoveryOperations<AdbDeviceInfo>>( 1 );
    DeviceListProviderBase<AdbDeviceInfo>::AsyncListOperation operation
        = [ operations ]( Generation generation ) {
              auto result = operations->run( generation );
              result.generation = generation + 100;
              return result;
          };
    AdbLogcatDialog dialog( operation );

    REQUIRE( operations->slot( 0 ).entered.tryAcquire( 1, 3000 ) );
    operations->slot( 0 ).setDevices(
        { adbDevice( QStringLiteral( "current-online" ), QStringLiteral( "device" ) ) } );
    operations->slot( 0 ).release.release();
    REQUIRE( operations->slot( 0 ).completed.tryAcquire( 1, 3000 ) );

    auto* combo = dialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
    REQUIRE( combo != nullptr );
    drainEventsUntil( [ combo ] { return combo->count() == 1; } );
    REQUIRE( combo->count() == 1 );
    CHECK( combo->currentData().toString() == QStringLiteral( "current-online" ) );
}

TEST_CASE( "successful refresh preserves the selected device and Accept state",
           "[device-discovery][dialog][selection]" )
{
    SECTION( "ADB" )
    {
        auto operations = std::make_shared<ControlledDiscoveryOperations<AdbDeviceInfo>>( 2 );
        DeviceListProviderBase<AdbDeviceInfo>::AsyncListOperation operation
            = [ operations ]( Generation generation ) { return operations->run( generation ); };
        AdbLogcatDialog dialog( operation );

        REQUIRE( operations->slot( 0 ).entered.tryAcquire( 1, 3000 ) );
        operations->slot( 0 ).setDevices(
            { adbDevice( QStringLiteral( "first-online" ), QStringLiteral( "device" ) ),
              adbDevice( QStringLiteral( "selected-online" ), QStringLiteral( "device" ) ) } );
        operations->slot( 0 ).release.release();
        REQUIRE( operations->slot( 0 ).completed.tryAcquire( 1, 3000 ) );

        auto* combo = dialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
        auto* buttons = dialog.findChild<QDialogButtonBox*>( QStringLiteral( "buttonBox" ) );
        REQUIRE( combo != nullptr );
        REQUIRE( buttons != nullptr );
        REQUIRE( buttons->button( QDialogButtonBox::Ok ) != nullptr );
        drainEventsUntil( [ combo ] { return combo->count() == 2; } );
        REQUIRE( combo->count() == 2 );
        combo->setCurrentIndex( 1 );
        REQUIRE( combo->currentData().toString() == QStringLiteral( "selected-online" ) );
        REQUIRE( buttons->button( QDialogButtonBox::Ok )->isEnabled() );

        REQUIRE( QMetaObject::invokeMethod( &dialog, "refreshDevices", Qt::DirectConnection ) );
        REQUIRE( operations->slot( 1 ).entered.tryAcquire( 1, 3000 ) );
        CHECK( combo->currentData().toString() == QStringLiteral( "selected-online" ) );
        CHECK( buttons->button( QDialogButtonBox::Ok )->isEnabled() );

        operations->slot( 1 ).setDevices(
            { adbDevice( QStringLiteral( "selected-online" ), QStringLiteral( "device" ) ),
              adbDevice( QStringLiteral( "first-online" ), QStringLiteral( "device" ) ),
              adbDevice( QStringLiteral( "new-online" ), QStringLiteral( "device" ) ) } );
        operations->slot( 1 ).release.release();
        REQUIRE( operations->slot( 1 ).completed.tryAcquire( 1, 3000 ) );
        drainEventsUntil( [ combo ] { return combo->count() == 3; } );
        REQUIRE( combo->count() == 3 );

        CHECK( combo->currentData().toString() == QStringLiteral( "selected-online" ) );
        CHECK( buttons->button( QDialogButtonBox::Ok )->isEnabled() );
    }

    SECTION( "iOS" )
    {
        auto operations = std::make_shared<ControlledDiscoveryOperations<IosDeviceInfo>>( 2 );
        DeviceListProviderBase<IosDeviceInfo>::AsyncListOperation operation
            = [ operations ]( Generation generation ) { return operations->run( generation ); };
        IosLogDialog dialog( operation );
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();

        REQUIRE( operations->slot( 0 ).entered.tryAcquire( 1, 3000 ) );
        operations->slot( 0 ).setDevices(
            { IosDeviceInfo{ QStringLiteral( "first-ios" ),
                             QStringLiteral( "first-ios" ),
                             QStringLiteral( "first-ios" ),
                             {},
                             {} },
              IosDeviceInfo{ QStringLiteral( "selected-ios" ),
                             QStringLiteral( "selected-ios" ),
                             QStringLiteral( "selected-ios" ),
                             {},
                             {} } } );
        operations->slot( 0 ).release.release();
        REQUIRE( operations->slot( 0 ).completed.tryAcquire( 1, 3000 ) );

        auto* combo = dialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
        auto* buttons = dialog.findChild<QDialogButtonBox*>( QStringLiteral( "buttonBox" ) );
        REQUIRE( combo != nullptr );
        REQUIRE( buttons != nullptr );
        REQUIRE( buttons->button( QDialogButtonBox::Ok ) != nullptr );
        drainEventsUntil( [ combo ] { return combo->count() == 2; } );
        REQUIRE( combo->count() == 2 );
        combo->setCurrentIndex( 1 );
        REQUIRE( combo->currentData().toString() == QStringLiteral( "selected-ios" ) );
        REQUIRE( buttons->button( QDialogButtonBox::Ok )->isEnabled() );

        REQUIRE( QMetaObject::invokeMethod( &dialog, "refreshDevices", Qt::DirectConnection ) );
        REQUIRE( operations->slot( 1 ).entered.tryAcquire( 1, 3000 ) );
        CHECK( combo->currentData().toString() == QStringLiteral( "selected-ios" ) );
        CHECK( buttons->button( QDialogButtonBox::Ok )->isEnabled() );

        operations->slot( 1 ).setDevices(
            { IosDeviceInfo{ QStringLiteral( "selected-ios" ),
                             QStringLiteral( "selected-ios" ),
                             QStringLiteral( "selected-ios" ),
                             {},
                             {} },
              IosDeviceInfo{ QStringLiteral( "first-ios" ),
                             QStringLiteral( "first-ios" ),
                             QStringLiteral( "first-ios" ),
                             {},
                             {} },
              IosDeviceInfo{ QStringLiteral( "new-ios" ),
                             QStringLiteral( "new-ios" ),
                             QStringLiteral( "new-ios" ),
                             {},
                             {} } } );
        operations->slot( 1 ).release.release();
        REQUIRE( operations->slot( 1 ).completed.tryAcquire( 1, 3000 ) );
        drainEventsUntil( [ combo ] { return combo->count() == 3; } );
        REQUIRE( combo->count() == 3 );

        CHECK( combo->currentData().toString() == QStringLiteral( "selected-ios" ) );
        CHECK( buttons->button( QDialogButtonBox::Ok )->isEnabled() );
    }
}

TEST_CASE( "iOS dialog presents no-device executable and protocol outcomes differently",
           "[device-discovery][ios][dialog][error]" )
{
    auto operations = std::make_shared<ControlledDiscoveryOperations<IosDeviceInfo>>( 3 );
    DeviceListProviderBase<IosDeviceInfo>::AsyncListOperation operation
        = [ operations ]( Generation generation ) { return operations->run( generation ); };
    IosLogDialog dialog( operation );
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    REQUIRE( operations->slot( 0 ).entered.tryAcquire( 1, 3000 ) );

    operations->slot( 0 ).release.release();
    REQUIRE( operations->slot( 0 ).completed.tryAcquire( 1, 3000 ) );
    auto* status = dialog.findChild<QLabel*>( QStringLiteral( "iosLogStatusLabel" ) );
    REQUIRE( status != nullptr );
    drainEventsUntil(
        [ status ] { return status->text().contains( QStringLiteral( "No iOS devices" ) ); } );
    REQUIRE( status->text().contains( QStringLiteral( "No iOS devices" ) ) );
    const auto noDeviceText = status->text();

    REQUIRE( QMetaObject::invokeMethod( &dialog, "refreshDevices", Qt::DirectConnection ) );
    REQUIRE( operations->slot( 1 ).entered.tryAcquire( 1, 3000 ) );
    REQUIRE( operations->slot( 1 ).result().generation
             == operations->slot( 0 ).result().generation + 1 );
    operations->slot( 1 ).setError(
        actionableError( ErrorCategory::Configuration, ErrorScope::Service, RetryPolicy::Never,
                         "ios-executable-not-found",
                         "pymobiledevice3 executable was not found; configure it and retry." ) );
    operations->slot( 1 ).release.release();
    REQUIRE( operations->slot( 1 ).completed.tryAcquire( 1, 3000 ) );
    drainEventsUntil(
        [ status ] { return status->text().contains( QStringLiteral( "pymobiledevice3" ) ); } );
    const auto executableFailureText = status->text();
    REQUIRE( executableFailureText.contains( QStringLiteral( "pymobiledevice3" ) ) );
    CHECK( executableFailureText != noDeviceText );

    REQUIRE( QMetaObject::invokeMethod( &dialog, "refreshDevices", Qt::DirectConnection ) );
    REQUIRE( operations->slot( 2 ).entered.tryAcquire( 1, 3000 ) );
    REQUIRE( operations->slot( 2 ).result().generation
             == operations->slot( 1 ).result().generation + 1 );
    operations->slot( 2 ).setError( actionableError(
        ErrorCategory::Backend, ErrorScope::Service, RetryPolicy::Immediate,
        "ios-device-list-protocol-error",
        "The iOS discovery service returned an invalid response; retry discovery." ) );
    operations->slot( 2 ).release.release();
    REQUIRE( operations->slot( 2 ).completed.tryAcquire( 1, 3000 ) );
    drainEventsUntil(
        [ status ] { return status->text().contains( QStringLiteral( "invalid response" ) ); } );
    const auto protocolFailureText = status->text();
    CHECK( protocolFailureText.contains( QStringLiteral( "invalid response" ) ) );
    CHECK( protocolFailureText != executableFailureText );
}

TEST_CASE( "dialog destruction drops pending discovery completions",
           "[device-discovery][dialog][lifetime]" )
{
    SECTION( "ADB" )
    {
        auto operations = std::make_shared<ControlledDiscoveryOperations<AdbDeviceInfo>>( 1 );
        DeviceListProviderBase<AdbDeviceInfo>::AsyncListOperation operation
            = [ operations ]( Generation generation ) { return operations->run( generation ); };
        auto dialog = std::make_unique<AdbLogcatDialog>( operation );
        REQUIRE( operations->slot( 0 ).entered.tryAcquire( 1, 3000 ) );
        CHECK( dialog->findChildren<QFutureWatcherBase*>().isEmpty() );
        dialog.reset();
        operations->slot( 0 ).release.release();
        REQUIRE( operations->slot( 0 ).completed.tryAcquire( 1, 3000 ) );
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
        QCoreApplication::processEvents();
    }

    SECTION( "iOS" )
    {
        auto operations = std::make_shared<ControlledDiscoveryOperations<IosDeviceInfo>>( 1 );
        DeviceListProviderBase<IosDeviceInfo>::AsyncListOperation operation
            = [ operations ]( Generation generation ) { return operations->run( generation ); };
        auto dialog = std::make_unique<IosLogDialog>( operation );
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        REQUIRE( operations->slot( 0 ).entered.tryAcquire( 1, 3000 ) );
        CHECK( dialog->findChildren<QFutureWatcherBase*>().isEmpty() );
        dialog.reset();
        operations->slot( 0 ).release.release();
        REQUIRE( operations->slot( 0 ).completed.tryAcquire( 1, 3000 ) );
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
        QCoreApplication::processEvents();
    }
}

TEST_CASE( "ADB and iOS share coordinator behavior while retaining provider-specific parsing",
           "[device-discovery][coordinator][adb][ios]" )
{
    static_assert( std::is_same_v<typename DeviceDiscoveryCoordinator<AdbDeviceInfo>::Generation,
                                  typename DeviceDiscoveryCoordinator<IosDeviceInfo>::Generation> );
    static_assert( std::is_same_v<typename DeviceDiscoveryCoordinator<AdbDeviceInfo>::Generation,
                                  klogg::livecapture::Generation> );

    const auto adbDevices = parseAdbDeviceListOutput(
        QByteArray{ "List of devices attached\nadb-1 offline transport_id:1\n" } );
    const auto iosDevices = parsePymobiledeviceDeviceDiscovery(
        Generation{ 8 }, QByteArray{ "[{\"Identifier\":\"ios-1\"}]" } );

    REQUIRE( adbDevices.size() == 1 );
    REQUIRE( adbDevices.front().state == AdbDeviceState::Offline );
    REQUIRE( iosDevices.devices.size() == 1 );
    REQUIRE( iosDevices.devices.front().udid == QStringLiteral( "ios-1" ) );
}
