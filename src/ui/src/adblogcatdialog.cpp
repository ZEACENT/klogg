#include "adblogcatdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>

#include <utility>

#include "adbdevicelistprovider.h"
#include "adbtrackeddeviceprovider.h"
#include "configuration.h"

namespace {

constexpr auto DeviceAvailableRole = Qt::UserRole + 1;

QString discoveryMessage( const klogg::livecapture::LiveSourceError& error )
{
    auto message
        = QString::fromUtf8( error.message.data(), static_cast<int>( error.message.size() ) );
    const auto detail = QString::fromUtf8( error.nativeDetail.data(),
                                           static_cast<int>( error.nativeDetail.size() ) );
    if ( !detail.isEmpty() && detail != message ) {
        if ( !message.isEmpty() ) {
            message.append( QStringLiteral( "\n" ) );
        }
        message.append( detail );
    }
    return message;
}

} // namespace

AdbLogcatDialog::AdbLogcatDialog( QWidget* parent )
    : AdbLogcatDialog( DeviceListProviderBase<AdbDeviceInfo>::AsyncListOperation{}, nullptr, {},
                       parent )
{
}

AdbLogcatDialog::AdbLogcatDialog(
    DeviceListProviderBase<AdbDeviceInfo>::AsyncListOperation discoveryOperation, QWidget* parent )
    : AdbLogcatDialog( std::move( discoveryOperation ), nullptr, {}, parent )
{
}

AdbLogcatDialog::AdbLogcatDialog( AdbTrackedDeviceProvider& provider, QString explicitDeviceSerial,
                                  QWidget* parent )
    : AdbLogcatDialog( {}, &provider, std::move( explicitDeviceSerial ), parent )
{
}

AdbLogcatDialog::AdbLogcatDialog(
    DeviceListProviderBase<AdbDeviceInfo>::AsyncListOperation discoveryOperation,
    AdbTrackedDeviceProvider* trackedProvider, QString explicitDeviceSerial, QWidget* parent )
    : QDialog( parent )
    , discoveryOperation_( std::move( discoveryOperation ) )
    , trackedProvider_( trackedProvider )
    , requestedDeviceSerial_( std::move( explicitDeviceSerial ) )
{
    initializeUi();
    loadSettings();

    if ( trackedProvider_ != nullptr ) {
        connect( trackedProvider_, &AdbTrackedDeviceProvider::snapshotChanged, this,
                 &AdbLogcatDialog::applyTrackedSnapshot );
        infrastructureLease_ = trackedProvider_->acquireLease();
    }
    refreshDevices();
}

void AdbLogcatDialog::initializeUi()
{
    setWindowTitle( tr( "Open ADB Logcat" ) );
    setModal( true );
    resize( 720, 380 );

    auto* rootLayout = new QVBoxLayout( this );
    auto* formLayout = new QFormLayout();

    // The dialog composes typed sessions exclusively through the built-in ADB
    // services: no executable path or free-form argument field exists.
    refreshButton_ = new QPushButton( tr( "Refresh Devices" ), this );
    refreshButton_->setObjectName( QStringLiteral( "refreshDevicesButton" ) );
    formLayout->addRow( refreshButton_ );

    deviceCombo_ = new QComboBox( this );
    deviceCombo_->setObjectName( QStringLiteral( "deviceCombo" ) );
    deviceCombo_->setSizeAdjustPolicy( QComboBox::AdjustToContents );
    ansiOutputCheckBox_ = new QCheckBox( tr( "Enable ANSI color output" ), this );
    ansiOutputCheckBox_->setObjectName( QStringLiteral( "ansiOutputCheckBox" ) );

    autoReconnectCheckBox_
        = new QCheckBox( tr( "Enable auto-reconnect on connection loss" ), this );
    autoReconnectCheckBox_->setObjectName( QStringLiteral( "autoReconnectCheckBox" ) );
    autoReconnectCheckBox_->setToolTip(
        tr( "When enabled, klogg automatically attempts to reconnect to the live source "
            "after an unexpected disconnection or error. Uses exponential backoff "
            "starting at 1 second and capping at 30 seconds between attempts." ) );

    maxAttemptsSpinBox_ = new QSpinBox( this );
    maxAttemptsSpinBox_->setObjectName( QStringLiteral( "maxAttemptsSpinBox" ) );
    maxAttemptsSpinBox_->setRange( 0, 9999 );
    maxAttemptsSpinBox_->setSpecialValueText( tr( "Unlimited" ) );
    maxAttemptsSpinBox_->setToolTip(
        tr( "Maximum number of automatic reconnection attempts before giving up. "
            "Set to 0 for unlimited retries. Each retry uses increasing delay "
            "(1s, 2s, 4s, 8s, ... up to 30s)." ) );

    maxFileSizeSpinBox_ = new QSpinBox( this );
    maxFileSizeSpinBox_->setObjectName( QStringLiteral( "maxFileSizeSpinBox" ) );
    maxFileSizeSpinBox_->setRange( 0, 1048576 );
    maxFileSizeSpinBox_->setSpecialValueText( tr( "Unlimited" ) );
    maxFileSizeSpinBox_->setSuffix( tr( " MB" ) );
    maxFileSizeSpinBox_->setToolTip(
        tr( "Maximum size of each rolling capture file in megabytes. "
            "When exceeded, the file is rotated and a new one is started. "
            "Set to 0 to disable size-based rolling." ) );

    backupCountSpinBox_ = new QSpinBox( this );
    backupCountSpinBox_->setObjectName( QStringLiteral( "backupCountSpinBox" ) );
    backupCountSpinBox_->setRange( 0, 999 );
    backupCountSpinBox_->setToolTip(
        tr( "Number of old capture files to keep when rolling by file size. "
            "Older files beyond this count are deleted. "
            "Set to 0 to keep all rotated files indefinitely." ) );

    formLayout->addRow( tr( "Device" ), deviceCombo_ );
    formLayout->addRow( ansiOutputCheckBox_ );
    formLayout->addRow( autoReconnectCheckBox_ );
    formLayout->addRow( tr( "Max reconnect attempts" ), maxAttemptsSpinBox_ );
    formLayout->addRow( tr( "Max capture file size" ), maxFileSizeSpinBox_ );
    formLayout->addRow( tr( "Rolling backup count" ), backupCountSpinBox_ );

    statusLabel_ = new QLabel( this );
    statusLabel_->setObjectName( QStringLiteral( "adbStatusLabel" ) );
    statusLabel_->setWordWrap( true );

    buttonBox_ = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
    buttonBox_->setObjectName( QStringLiteral( "buttonBox" ) );

    rootLayout->addLayout( formLayout );
    rootLayout->addWidget( statusLabel_ );
    rootLayout->addWidget( buttonBox_ );

    connect( refreshButton_, &QPushButton::clicked, this, &AdbLogcatDialog::refreshDevices );
    connect( deviceCombo_, qOverload<int>( &QComboBox::currentIndexChanged ), this,
             &AdbLogcatDialog::updateAcceptState );
    connect( buttonBox_, &QDialogButtonBox::accepted, this, [ this ] {
        saveSettings();
        accept();
    } );
    connect( buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject );
}

AdbLogcatSessionData AdbLogcatDialog::sessionData() const
{
    // Only built-in transports are composed: raw executable/argument fields
    // were retired with the compatibility process backend.
    AdbLogcatSessionData sessionData;
    sessionData.deviceSerial = deviceCombo_->currentData( Qt::UserRole ).toString();
    sessionData.deviceDescription = deviceCombo_->currentText();
    sessionData.captureId = QUuid::createUuid().toString( QUuid::WithoutBraces );
    sessionData.sourceType = LiveLogSourceType::AdbLogcat;
    sessionData.adbBackend = AdbTransportBackend::SmartSocket;
    sessionData.runIntent = klogg::livecapture::RunIntent::Running;
    sessionData.ansiOutputEnabled = ansiOutputCheckBox_->isChecked();
    sessionData.autoReconnectEnabled = autoReconnectCheckBox_->isChecked();
    sessionData.maxReconnectAttempts = maxAttemptsSpinBox_->value();
    sessionData.captureMaxFileSize
        = static_cast<qint64>( maxFileSizeSpinBox_->value() ) * 1024 * 1024;
    sessionData.captureBackupCount = backupCountSpinBox_->value();
    return sessionData;
}

void AdbLogcatDialog::refreshDevices()
{
    if ( trackedProvider_ != nullptr ) {
        refreshButton_->setEnabled( false );
        statusLabel_->setText( tr( "Detecting ADB devices..." ) );
        trackedProvider_->refresh();
        refreshButton_->setEnabled( true );
        return;
    }

    const auto generation = discoveryCoordinator_.beginRefresh();
    ++pendingRefreshCount_;
    refreshButton_->setEnabled( false );
    statusLabel_->setText( tr( "Detecting ADB devices..." ) );

    if ( !discoveryOperation_ ) {
        --pendingRefreshCount_;
        refreshButton_->setEnabled( true );
        statusLabel_->setText( tr( "No ADB devices detected." ) );
        deviceCombo_->clear();
        updateAcceptState();
        return;
    }

    auto future = runDeviceDiscoveryAsync<AdbDeviceInfo>( discoveryOperation_, generation );

    // The watcher owns its completion cleanup and outlives the dialog when a
    // discovery task is still publishing a result during dialog destruction.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    auto* watcher = new QFutureWatcher<DeviceDiscoveryResult<AdbDeviceInfo>>;
    const QPointer<AdbLogcatDialog> dialog( this );
    connect( watcher, &QFutureWatcher<DeviceDiscoveryResult<AdbDeviceInfo>>::finished, watcher,
             [ dialog, watcher ] {
                 auto result = watcher->result();
                 watcher->deleteLater();
                 if ( dialog == nullptr ) {
                     return;
                 }
                 if ( dialog->pendingRefreshCount_ > 0 ) {
                     --dialog->pendingRefreshCount_;
                 }
                 dialog->refreshButton_->setEnabled( dialog->pendingRefreshCount_ == 0 );
                 dialog->applyDiscoveryResult( std::move( result ) );
             } );
    watcher->setFuture( future );
}

void AdbLogcatDialog::applyDiscoveryResult( DeviceDiscoveryResult<AdbDeviceInfo> result )
{
    if ( !discoveryCoordinator_.accept( std::move( result ) ) ) {
        return;
    }
    populateDevices( discoveryCoordinator_.currentDevices(), discoveryCoordinator_.currentError() );
}

void AdbLogcatDialog::applyTrackedSnapshot( const DeviceDiscoveryResult<AdbDeviceInfo>& result )
{
    if ( result.generation < latestTrackedGeneration_ ) {
        return;
    }
    latestTrackedGeneration_ = result.generation;
    populateDevices( result.devices, result.error );
}

void AdbLogcatDialog::populateDevices(
    const QList<AdbDeviceInfo>& devices,
    const std::optional<klogg::livecapture::LiveSourceError>& error )
{
    auto selectedDeviceId = deviceCombo_->currentData( Qt::UserRole ).toString();
    if ( selectedDeviceId.isEmpty() ) {
        selectedDeviceId = requestedDeviceSerial_;
    }

    deviceCombo_->clear();
    for ( const auto& device : devices ) {
        deviceCombo_->addItem( device.displayName, device.serial );
        const auto index = deviceCombo_->count() - 1;
        deviceCombo_->setItemData( index, device.description, Qt::ToolTipRole );
        deviceCombo_->setItemData( index, device.isOnline(), DeviceAvailableRole );
    }

    auto selectedIndex = deviceCombo_->findData( selectedDeviceId );
    if ( selectedIndex < 0 ) {
        for ( int index = 0; index < deviceCombo_->count(); ++index ) {
            if ( deviceCombo_->itemData( index, DeviceAvailableRole ).toBool() ) {
                selectedIndex = index;
                break;
            }
        }
    }
    deviceCombo_->setCurrentIndex( selectedIndex );

    if ( error.has_value() ) {
        statusLabel_->setText( discoveryMessage( *error ) );
    }
    else if ( devices.isEmpty() ) {
        statusLabel_->setText( tr( "No ADB devices detected." ) );
    }
    else {
        statusLabel_->setText( tr(
            "Data stays in temp capture storage until you explicitly save or close the tab." ) );
    }

    updateAcceptState();
}

void AdbLogcatDialog::updateAcceptState()
{
    if ( auto* okButton = buttonBox_->button( QDialogButtonBox::Ok ) ) {
        okButton->setEnabled( deviceCombo_->currentIndex() >= 0
                              && deviceCombo_->currentData( DeviceAvailableRole ).toBool() );
    }
}

void AdbLogcatDialog::loadSettings()
{
    const auto& config = Configuration::get();
    ansiOutputCheckBox_->setChecked( config.adbLogcatAnsiOutputEnabled() );
    autoReconnectCheckBox_->setChecked( config.liveAutoReconnectEnabled() );
    maxAttemptsSpinBox_->setValue( config.liveAutoReconnectMaxAttempts() );
    maxFileSizeSpinBox_->setValue( config.liveCaptureRollingMaxFileSizeMb() );
    backupCountSpinBox_->setValue( config.liveCaptureRollingBackupCount() );
}

void AdbLogcatDialog::saveSettings() const
{
    auto& config = Configuration::get();
    config.setAdbLogcatAnsiOutputEnabled( ansiOutputCheckBox_->isChecked() );
    config.setLiveAutoReconnectEnabled( autoReconnectCheckBox_->isChecked() );
    config.setLiveAutoReconnectMaxAttempts( maxAttemptsSpinBox_->value() );
    config.setLiveCaptureRollingMaxFileSizeMb( maxFileSizeSpinBox_->value() );
    config.setLiveCaptureRollingBackupCount( backupCountSpinBox_->value() );
    config.save();
}
