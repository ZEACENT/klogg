#include "ioslogdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QVariantMap>

#include "configuration.h"
#include "iosdevicelistprovider.h"

namespace {

QString discoveryMessage( const klogg::livecapture::LiveSourceError& error )
{
    return QString::fromUtf8( error.message.data(), static_cast<int>( error.message.size() ) );
}

} // namespace

IosLogDialog::IosLogDialog( QWidget* parent )
    : IosLogDialog( DeviceListProviderBase<IosDeviceInfo>::AsyncListOperation{}, parent )
{
}

IosLogDialog::IosLogDialog(
    DeviceListProviderBase<IosDeviceInfo>::AsyncListOperation discoveryOperation, QWidget* parent )
    : QDialog( parent )
    , discoveryOperation_( std::move( discoveryOperation ) )
{
    setWindowTitle( tr( "Open iOS Log Stream" ) );
    setModal( true );
    resize( 720, 380 );

    auto* rootLayout = new QVBoxLayout( this );
    auto* formLayout = new QFormLayout();

    // The dialog composes typed native sessions exclusively through the
    // bundled services: no sidecar executable or free-form argument field
    // exists.
    refreshButton_ = new QPushButton( tr( "Refresh Devices" ), this );
    refreshButton_->setObjectName( QStringLiteral( "refreshDevicesButton" ) );

    deviceCombo_ = new QComboBox( this );
    deviceCombo_->setObjectName( QStringLiteral( "deviceCombo" ) );
    deviceCombo_->setSizeAdjustPolicy( QComboBox::AdjustToContents );
    ansiOutputCheckBox_ = new QCheckBox( tr( "Enable ANSI color output" ), this );
    ansiOutputCheckBox_->setObjectName( QStringLiteral( "ansiOutputCheckBox" ) );

    // Auto-reconnect: enables automatic reconnection with exponential backoff
    autoReconnectCheckBox_
        = new QCheckBox( tr( "Enable auto-reconnect on connection loss" ), this );
    autoReconnectCheckBox_->setObjectName( QStringLiteral( "autoReconnectCheckBox" ) );
    autoReconnectCheckBox_->setToolTip(
        tr( "When enabled, klogg automatically attempts to reconnect to the live source "
            "after an unexpected disconnection or error. Uses exponential backoff "
            "starting at 1 second and capping at 30 seconds between attempts." ) );

    // Max reconnect attempts
    maxAttemptsSpinBox_ = new QSpinBox( this );
    maxAttemptsSpinBox_->setObjectName( QStringLiteral( "maxAttemptsSpinBox" ) );
    maxAttemptsSpinBox_->setRange( 0, 9999 );
    maxAttemptsSpinBox_->setSpecialValueText( tr( "Unlimited" ) );
    maxAttemptsSpinBox_->setToolTip(
        tr( "Maximum number of automatic reconnection attempts before giving up. "
            "Set to 0 for unlimited retries. Each retry uses increasing delay "
            "(1s, 2s, 4s, 8s, ... up to 30s)." ) );

    // Max capture file size (displayed in MB, stored in bytes)
    maxFileSizeSpinBox_ = new QSpinBox( this );
    maxFileSizeSpinBox_->setObjectName( QStringLiteral( "maxFileSizeSpinBox" ) );
    maxFileSizeSpinBox_->setRange( 0, 1048576 ); // 0 to ~1 TB in MB
    maxFileSizeSpinBox_->setSpecialValueText( tr( "Unlimited" ) );
    maxFileSizeSpinBox_->setSuffix( tr( " MB" ) );
    maxFileSizeSpinBox_->setToolTip(
        tr( "Maximum size of each rolling capture file in megabytes. "
            "When exceeded, the file is rotated and a new one is started. "
            "Set to 0 to disable size-based rolling." ) );

    // Rolling backup count
    backupCountSpinBox_ = new QSpinBox( this );
    backupCountSpinBox_->setObjectName( QStringLiteral( "backupCountSpinBox" ) );
    backupCountSpinBox_->setRange( 0, 999 );
    backupCountSpinBox_->setToolTip(
        tr( "Number of old capture files to keep when rolling by file size. "
            "Older files beyond this count are deleted. "
            "Set to 0 to keep all rotated files indefinitely." ) );

    formLayout->addRow( refreshButton_ );
    formLayout->addRow( tr( "Device" ), deviceCombo_ );
    formLayout->addRow( ansiOutputCheckBox_ );
    formLayout->addRow( autoReconnectCheckBox_ );
    formLayout->addRow( tr( "Max reconnect attempts" ), maxAttemptsSpinBox_ );
    formLayout->addRow( tr( "Max capture file size" ), maxFileSizeSpinBox_ );
    formLayout->addRow( tr( "Rolling backup count" ), backupCountSpinBox_ );

    statusLabel_ = new QLabel( this );
    statusLabel_->setObjectName( QStringLiteral( "iosLogStatusLabel" ) );
    statusLabel_->setWordWrap( true );

    buttonBox_ = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
    buttonBox_->setObjectName( QStringLiteral( "buttonBox" ) );

    rootLayout->addLayout( formLayout );
    rootLayout->addWidget( statusLabel_ );
    rootLayout->addWidget( buttonBox_ );

    connect( refreshButton_, &QPushButton::clicked, this, &IosLogDialog::refreshDevices );
    connect( deviceCombo_, qOverload<int>( &QComboBox::currentIndexChanged ), this,
             &IosLogDialog::updateAcceptState );
    connect( buttonBox_, &QDialogButtonBox::accepted, this, [ this ] {
        saveSettings();
        accept();
    } );
    connect( buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject );

    loadSettings();
    QTimer::singleShot( 0, this, &IosLogDialog::refreshDevices );
}

IosLogDialog::IosLogDialog( klogg::livecapture::ios::IosCatalogSnapshotProvider& catalogProvider,
                            QWidget* parent )
    : IosLogDialog( DeviceListProviderBase<IosDeviceInfo>::AsyncListOperation{}, parent )
{
    catalogProvider_ = &catalogProvider;
}

AdbLogcatSessionData IosLogDialog::sessionData() const
{
    // Only the built-in native transport is composed: sidecar executable and
    // free-form argument fields were retired with the compatibility backend.
    AdbLogcatSessionData sessionData;
    sessionData.deviceDescription = deviceCombo_->currentText();
    if ( catalogProvider_ != nullptr ) {
        const auto endpoint = deviceCombo_->currentData( Qt::UserRole ).toMap();
        sessionData.deviceSerial = endpoint.value( QStringLiteral( "udid" ) ).toString();
        sessionData.iosEndpoint.connectionType
            = endpoint.value( QStringLiteral( "connection" ) ).toString()
                      == QStringLiteral( "network" )
                  ? klogg::livecapture::ios::NativeConnectionType::Network
                  : klogg::livecapture::ios::NativeConnectionType::Usb;
    }
    else {
        sessionData.deviceSerial = deviceCombo_->currentData( Qt::UserRole ).toString();
    }
    sessionData.iosBackend = IosTransportBackend::Native;
    sessionData.iosEndpoint.udid = sessionData.deviceSerial.toStdString();
    sessionData.captureId = QUuid::createUuid().toString( QUuid::WithoutBraces );
    sessionData.sourceType = LiveLogSourceType::IosLogStream;
    sessionData.runIntent = klogg::livecapture::RunIntent::Running;
    sessionData.ansiOutputEnabled = ansiOutputCheckBox_->isChecked();
    sessionData.autoReconnectEnabled = autoReconnectCheckBox_->isChecked();
    sessionData.maxReconnectAttempts = maxAttemptsSpinBox_->value();
    sessionData.captureMaxFileSize
        = static_cast<qint64>( maxFileSizeSpinBox_->value() ) * 1024 * 1024;
    sessionData.captureBackupCount = backupCountSpinBox_->value();
    return sessionData;
}

void IosLogDialog::refreshDevices()
{
    if ( catalogProvider_ != nullptr ) {
        const auto selectedEndpoint = deviceCombo_->currentData( Qt::UserRole );
        const auto snapshot = catalogProvider_->snapshot();
        deviceCombo_->clear();
        for ( const auto& entry : snapshot.entries ) {
            QVariantMap endpoint;
            endpoint.insert( QStringLiteral( "udid" ),
                             QString::fromStdString( entry.endpoint.udid ) );
            endpoint.insert( QStringLiteral( "connection" ),
                             entry.endpoint.connectionType
                                     == klogg::livecapture::ios::NativeConnectionType::Network
                                 ? QStringLiteral( "network" )
                                 : QStringLiteral( "usb" ) );
            const auto displayName
                = entry.metadata.has_value() && !entry.metadata->displayName.empty()
                      ? QString::fromStdString( entry.metadata->displayName )
                      : QString::fromStdString( entry.endpoint.udid );
            const auto connectionName
                = entry.endpoint.connectionType
                          == klogg::livecapture::ios::NativeConnectionType::Network
                      ? tr( "Network" )
                      : tr( "USB" );
            deviceCombo_->addItem( QStringLiteral( "%1 (%2)" ).arg( displayName, connectionName ),
                                   endpoint );
            if ( entry.metadata.has_value() ) {
                deviceCombo_->setItemData(
                    deviceCombo_->count() - 1,
                    QStringLiteral( "%1 · iOS %2" )
                        .arg( QString::fromStdString( entry.metadata->productType ),
                              QString::fromStdString( entry.metadata->productVersion ) ),
                    Qt::ToolTipRole );
            }
        }
        const auto selectedIndex = deviceCombo_->findData( selectedEndpoint );
        if ( selectedIndex >= 0 ) {
            deviceCombo_->setCurrentIndex( selectedIndex );
        }
        statusLabel_->setText(
            snapshot.entries.empty()
                ? tr( "No iOS devices detected by the bundled native service." )
                : tr( "Using the bundled native iOS transport. No Python process is started." ) );
        refreshButton_->setEnabled( true );
        updateAcceptState();
        return;
    }

    const auto generation = discoveryCoordinator_.beginRefresh();
    ++pendingRefreshCount_;
    refreshButton_->setEnabled( false );
    statusLabel_->setText( tr( "Detecting iOS devices..." ) );

    if ( !discoveryOperation_ ) {
        --pendingRefreshCount_;
        refreshButton_->setEnabled( true );
        statusLabel_->setText( tr( "No iOS devices detected." ) );
        deviceCombo_->clear();
        updateAcceptState();
        return;
    }

    auto future = runDeviceDiscoveryAsync<IosDeviceInfo>( discoveryOperation_, generation );

    // The dialog's QObject child tree owns each request-local watcher.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    auto* watcher = new QFutureWatcher<DeviceDiscoveryResult<IosDeviceInfo>>( this );
    connect( watcher, &QFutureWatcher<DeviceDiscoveryResult<IosDeviceInfo>>::finished, this,
             [ this, watcher ] {
                 auto result = watcher->result();
                 watcher->deleteLater();
                 if ( pendingRefreshCount_ > 0 ) {
                     --pendingRefreshCount_;
                 }
                 refreshButton_->setEnabled( pendingRefreshCount_ == 0 );
                 applyDiscoveryResult( std::move( result ) );
             } );
    watcher->setFuture( future );
}

void IosLogDialog::applyDiscoveryResult( DeviceDiscoveryResult<IosDeviceInfo> result )
{
    if ( !discoveryCoordinator_.accept( std::move( result ) ) ) {
        return;
    }

    const auto selectedDeviceId = deviceCombo_->currentData( Qt::UserRole );
    deviceCombo_->clear();
    for ( const auto& device : discoveryCoordinator_.currentDevices() ) {
        deviceCombo_->addItem( device.displayName, device.udid );
        deviceCombo_->setItemData( deviceCombo_->count() - 1, device.description, Qt::ToolTipRole );
    }

    const auto selectedIndex = deviceCombo_->findData( selectedDeviceId );
    if ( selectedIndex >= 0 ) {
        deviceCombo_->setCurrentIndex( selectedIndex );
    }

    if ( discoveryCoordinator_.currentError() ) {
        statusLabel_->setText( discoveryMessage( *discoveryCoordinator_.currentError() ) );
    }
    else if ( discoveryCoordinator_.currentDevices().isEmpty() ) {
        statusLabel_->setText( tr( "No iOS devices detected." ) );
    }
    else {
        statusLabel_->setText( tr(
            "Data stays in temp capture storage until you explicitly save or close the tab." ) );
    }

    updateAcceptState();
}

void IosLogDialog::updateAcceptState()
{
    if ( auto* okButton = buttonBox_->button( QDialogButtonBox::Ok ) ) {
        okButton->setEnabled( deviceCombo_->count() > 0 && deviceCombo_->currentIndex() >= 0 );
    }
}

void IosLogDialog::loadSettings()
{
    const auto& config = Configuration::get();
    ansiOutputCheckBox_->setChecked( config.iosLogAnsiOutputEnabled() );
    autoReconnectCheckBox_->setChecked( config.liveAutoReconnectEnabled() );
    maxAttemptsSpinBox_->setValue( config.liveAutoReconnectMaxAttempts() );
    maxFileSizeSpinBox_->setValue( config.liveCaptureRollingMaxFileSizeMb() );
    backupCountSpinBox_->setValue( config.liveCaptureRollingBackupCount() );
}

void IosLogDialog::saveSettings() const
{
    auto& config = Configuration::get();
    config.setIosLogAnsiOutputEnabled( ansiOutputCheckBox_->isChecked() );
    config.setLiveAutoReconnectEnabled( autoReconnectCheckBox_->isChecked() );
    config.setLiveAutoReconnectMaxAttempts( maxAttemptsSpinBox_->value() );
    config.setLiveCaptureRollingMaxFileSizeMb( maxFileSizeSpinBox_->value() );
    config.setLiveCaptureRollingBackupCount( backupCountSpinBox_->value() );
    config.save();
}
