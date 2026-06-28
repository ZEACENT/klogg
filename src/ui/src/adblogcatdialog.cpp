#include "adblogcatdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QUuid>
#include <QtConcurrent>

#include <memory>

#include "adbdevicelistprovider.h"
#include "adbprocesstransport.h"
#include "configuration.h"

AdbLogcatDialog::AdbLogcatDialog( QWidget* parent )
    : QDialog( parent )
{
    setWindowTitle( tr( "Open ADB Logcat" ) );
    setModal( true );
    resize( 720, 380 );

    auto* rootLayout = new QVBoxLayout( this );
    auto* formLayout = new QFormLayout();

    auto* adbRowLayout = new QHBoxLayout();
    adbExecutableEdit_ = new QLineEdit( this );
    adbExecutableEdit_->setObjectName( QStringLiteral( "adbExecutableEdit" ) );
    refreshButton_ = new QPushButton( tr( "Refresh Devices" ), this );
    refreshButton_->setObjectName( QStringLiteral( "refreshDevicesButton" ) );
    adbRowLayout->addWidget( adbExecutableEdit_ );
    adbRowLayout->addWidget( refreshButton_ );

    deviceCombo_ = new QComboBox( this );
    deviceCombo_->setObjectName( QStringLiteral( "deviceCombo" ) );
    deviceCombo_->setSizeAdjustPolicy( QComboBox::AdjustToContents );
    extraArgsEdit_ = new QLineEdit( this );
    extraArgsEdit_->setObjectName( QStringLiteral( "extraArgsEdit" ) );
    extraArgsEdit_->setPlaceholderText( tr( "Optional logcat args, appended after 'adb -s <serial> logcat'" ) );
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

    formLayout->addRow( tr( "ADB executable" ), adbRowLayout );
    formLayout->addRow( tr( "Device" ), deviceCombo_ );
    formLayout->addRow( tr( "Extra logcat args" ), extraArgsEdit_ );
    formLayout->addRow( QString{}, ansiOutputCheckBox_ );
    formLayout->addRow( QString{}, autoReconnectCheckBox_ );
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

    deviceRefreshWatcher_ = new QFutureWatcher<QList<AdbDeviceInfo>>( this );
    connect( deviceRefreshWatcher_, &QFutureWatcher<QList<AdbDeviceInfo>>::finished, this,
             &AdbLogcatDialog::onDevicesEnumerated );

    loadSettings();
    refreshDevices();
}

AdbLogcatSessionData AdbLogcatDialog::sessionData() const
{
    AdbLogcatSessionData sessionData;
    sessionData.adbExecutable = adbExecutableEdit_->text().trimmed();
    sessionData.deviceSerial = deviceCombo_->currentData( Qt::UserRole ).toString();
    sessionData.deviceDescription = deviceCombo_->currentText();
    sessionData.extraArgs = extraArgsEdit_->text().trimmed();
    sessionData.captureId = QUuid::createUuid().toString( QUuid::WithoutBraces );
    sessionData.sourceType = LiveLogSourceType::AdbLogcat;
    sessionData.ansiOutputEnabled = ansiOutputCheckBox_->isChecked();
    sessionData.autoReconnectEnabled = autoReconnectCheckBox_->isChecked();
    sessionData.maxReconnectAttempts = maxAttemptsSpinBox_->value();
    sessionData.captureMaxFileSize = static_cast<qint64>( maxFileSizeSpinBox_->value() ) * 1024 * 1024;
    sessionData.captureBackupCount = backupCountSpinBox_->value();
    return sessionData;
}

void AdbLogcatDialog::refreshDevices()
{
    deviceCombo_->clear();
    updateAcceptState();
    statusLabel_->setText( tr( "Detecting ADB devices..." ) );

    // Enumerate off the GUI thread: `adb devices` can block for up to ~8s, and
    // the previous synchronous call froze the whole dialog. A shared_ptr keeps
    // the provider alive for the entire task, so this is use-after-free safe
    // even if the dialog is closed mid-enumeration (the watcher is a dialog
    // child; its finished handler is disconnected automatically on destruction).
    auto provider = std::make_shared<AdbDeviceListProvider>( adbExecutableEdit_->text() );
    deviceRefreshWatcher_->setFuture(
        QtConcurrent::run( [provider]() { return provider->listDevices(); } ) );
}

void AdbLogcatDialog::onDevicesEnumerated()
{
    const auto devices = deviceRefreshWatcher_->result();
    for ( const auto& device : devices ) {
        deviceCombo_->addItem( device.displayName, device.serial );
        deviceCombo_->setItemData( deviceCombo_->count() - 1, device.description, Qt::ToolTipRole );
    }

    if ( devices.isEmpty() ) {
        statusLabel_->setText( tr( "No online ADB devices detected." ) );
    }
    else {
        statusLabel_->setText(
            tr( "Data stays in temp capture storage until you explicitly save or close the tab." ) );
    }

    updateAcceptState();
}

void AdbLogcatDialog::updateAcceptState()
{
    if ( auto* okButton = buttonBox_->button( QDialogButtonBox::Ok ) ) {
        okButton->setEnabled( deviceCombo_->count() > 0 && deviceCombo_->currentIndex() >= 0 );
    }
}

void AdbLogcatDialog::loadSettings()
{
    const auto& config = Configuration::get();
    adbExecutableEdit_->setText( config.adbExecutable() );
    extraArgsEdit_->setText( config.adbLogcatExtraArgs() );
    ansiOutputCheckBox_->setChecked( config.adbLogcatAnsiOutputEnabled() );
    autoReconnectCheckBox_->setChecked( config.liveAutoReconnectEnabled() );
    maxAttemptsSpinBox_->setValue( config.liveAutoReconnectMaxAttempts() );
    maxFileSizeSpinBox_->setValue( config.liveCaptureRollingMaxFileSizeMb() );
    backupCountSpinBox_->setValue( config.liveCaptureRollingBackupCount() );
}

void AdbLogcatDialog::saveSettings() const
{
    auto& config = Configuration::get();
    config.setAdbExecutable( adbExecutableEdit_->text().trimmed() );
    config.setAdbLogcatExtraArgs( extraArgsEdit_->text().trimmed() );
    config.setAdbLogcatAnsiOutputEnabled( ansiOutputCheckBox_->isChecked() );
    config.setLiveAutoReconnectEnabled( autoReconnectCheckBox_->isChecked() );
    config.setLiveAutoReconnectMaxAttempts( maxAttemptsSpinBox_->value() );
    config.setLiveCaptureRollingMaxFileSizeMb( maxFileSizeSpinBox_->value() );
    config.setLiveCaptureRollingBackupCount( backupCountSpinBox_->value() );
    config.save();
}
