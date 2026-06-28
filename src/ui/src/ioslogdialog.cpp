#include "ioslogdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QUuid>
#include <QtConcurrent>

#include <memory>

#include "configuration.h"
#include "iosdevicelistprovider.h"
#include "ioslogprocesstransport.h"

IosLogDialog::IosLogDialog( QWidget* parent )
    : QDialog( parent )
{
    setWindowTitle( tr( "Open iOS Log Stream" ) );
    setModal( true );
    resize( 720, 380 );

    auto* rootLayout = new QVBoxLayout( this );
    auto* formLayout = new QFormLayout();

    auto* executableRowLayout = new QHBoxLayout();
    executableEdit_ = new QLineEdit( this );
    executableEdit_->setObjectName( QStringLiteral( "iosLogExecutableEdit" ) );
    executableEdit_->setPlaceholderText( QStringLiteral( "pymobiledevice3" ) );
    refreshButton_ = new QPushButton( tr( "Refresh Devices" ), this );
    refreshButton_->setObjectName( QStringLiteral( "refreshDevicesButton" ) );
    executableRowLayout->addWidget( executableEdit_ );
    executableRowLayout->addWidget( refreshButton_ );

    deviceCombo_ = new QComboBox( this );
    deviceCombo_->setObjectName( QStringLiteral( "deviceCombo" ) );
    deviceCombo_->setSizeAdjustPolicy( QComboBox::AdjustToContents );
    extraArgsEdit_ = new QLineEdit( this );
    extraArgsEdit_->setObjectName( QStringLiteral( "extraArgsEdit" ) );
    extraArgsEdit_->setPlaceholderText(
        tr( "Optional args, appended after 'pymobiledevice3 syslog live --udid <udid>'" ) );
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

    formLayout->addRow( tr( "pymobiledevice3 executable" ), executableRowLayout );
    formLayout->addRow( tr( "Device" ), deviceCombo_ );
    formLayout->addRow( tr( "Extra iOS log args" ), extraArgsEdit_ );
    formLayout->addRow( QString{}, ansiOutputCheckBox_ );
    formLayout->addRow( QString{}, autoReconnectCheckBox_ );
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

    deviceRefreshWatcher_ = new QFutureWatcher<QList<IosDeviceInfo>>( this );
    connect( deviceRefreshWatcher_, &QFutureWatcher<QList<IosDeviceInfo>>::finished, this,
             &IosLogDialog::onDevicesEnumerated );

    loadSettings();
    QTimer::singleShot( 0, this, &IosLogDialog::refreshDevices );
}

AdbLogcatSessionData IosLogDialog::sessionData() const
{
    AdbLogcatSessionData sessionData;
    sessionData.adbExecutable = executableEdit_->text().trimmed();
    sessionData.deviceSerial = deviceCombo_->currentData( Qt::UserRole ).toString();
    sessionData.deviceDescription = deviceCombo_->currentText();
    sessionData.extraArgs = extraArgsEdit_->text().trimmed();
    sessionData.captureId = QUuid::createUuid().toString( QUuid::WithoutBraces );
    sessionData.sourceType = LiveLogSourceType::IosLogStream;
    sessionData.ansiOutputEnabled = ansiOutputCheckBox_->isChecked();
    sessionData.autoReconnectEnabled = autoReconnectCheckBox_->isChecked();
    sessionData.maxReconnectAttempts = maxAttemptsSpinBox_->value();
    sessionData.captureMaxFileSize = static_cast<qint64>( maxFileSizeSpinBox_->value() ) * 1024 * 1024;
    sessionData.captureBackupCount = backupCountSpinBox_->value();
    return sessionData;
}

void IosLogDialog::refreshDevices()
{
    deviceCombo_->clear();
    updateAcceptState();
    statusLabel_->setText( tr( "Detecting iOS devices..." ) );

    // Enumerate off the GUI thread: pymobiledevice3 can block for up to ~8s,
    // and the previous synchronous call froze the whole dialog. A shared_ptr
    // keeps the provider alive for the entire task, so this is use-after-free
    // safe even if the dialog is closed mid-enumeration.
    auto provider = std::make_shared<IosDeviceListProvider>( executableEdit_->text() );
    deviceRefreshWatcher_->setFuture(
        QtConcurrent::run( [provider]() { return provider->listDevices(); } ) );
}

void IosLogDialog::onDevicesEnumerated()
{
    const auto devices = deviceRefreshWatcher_->result();
    for ( const auto& device : devices ) {
        deviceCombo_->addItem( device.displayName, device.udid );
        deviceCombo_->setItemData( deviceCombo_->count() - 1, device.description, Qt::ToolTipRole );
    }

    if ( devices.isEmpty() ) {
        statusLabel_->setText( tr( "No iOS devices detected." ) );
    }
    else {
        statusLabel_->setText(
            tr( "Data stays in temp capture storage until you explicitly save or close the tab." ) );
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
    executableEdit_->setText( config.iosLogExecutable() );
    extraArgsEdit_->setText( config.iosLogExtraArgs() );
    ansiOutputCheckBox_->setChecked( config.iosLogAnsiOutputEnabled() );
    autoReconnectCheckBox_->setChecked( config.liveAutoReconnectEnabled() );
    maxAttemptsSpinBox_->setValue( config.liveAutoReconnectMaxAttempts() );
    maxFileSizeSpinBox_->setValue( config.liveCaptureRollingMaxFileSizeMb() );
    backupCountSpinBox_->setValue( config.liveCaptureRollingBackupCount() );
}

void IosLogDialog::saveSettings() const
{
    auto& config = Configuration::get();
    config.setIosLogExecutable( executableEdit_->text().trimmed() );
    config.setIosLogExtraArgs( extraArgsEdit_->text().trimmed() );
    config.setIosLogAnsiOutputEnabled( ansiOutputCheckBox_->isChecked() );
    config.setLiveAutoReconnectEnabled( autoReconnectCheckBox_->isChecked() );
    config.setLiveAutoReconnectMaxAttempts( maxAttemptsSpinBox_->value() );
    config.setLiveCaptureRollingMaxFileSizeMb( maxFileSizeSpinBox_->value() );
    config.setLiveCaptureRollingBackupCount( backupCountSpinBox_->value() );
    config.save();
}
