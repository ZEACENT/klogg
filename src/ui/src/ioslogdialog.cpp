#include "ioslogdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QUuid>

#include "configuration.h"
#include "ioslogprocesstransport.h"

IosLogDialog::IosLogDialog( QWidget* parent )
    : QDialog( parent )
{
    setWindowTitle( tr( "Open iOS Log Stream" ) );
    setModal( true );
    resize( 720, 220 );

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

    formLayout->addRow( tr( "pymobiledevice3 executable" ), executableRowLayout );
    formLayout->addRow( tr( "Device" ), deviceCombo_ );
    formLayout->addRow( tr( "Extra iOS log args" ), extraArgsEdit_ );
    formLayout->addRow( QString{}, ansiOutputCheckBox_ );

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

AdbLogcatSessionData IosLogDialog::sessionData() const
{
    return AdbLogcatSessionData{
        executableEdit_->text().trimmed(),
        deviceCombo_->currentData( Qt::UserRole ).toString(),
        deviceCombo_->currentText(),
        extraArgsEdit_->text().trimmed(),
        QUuid::createUuid().toString( QUuid::WithoutBraces ),
        {},
        LiveLogSourceType::IosLogStream,
        ansiOutputCheckBox_->isChecked(),
    };
}

void IosLogDialog::refreshDevices()
{
    deviceCombo_->clear();

    QString error;
    const auto devices = IosLogProcessTransport::listDevices( executableEdit_->text(), &error );
    for ( const auto& device : devices ) {
        deviceCombo_->addItem( device.displayName, device.udid );
        deviceCombo_->setItemData( deviceCombo_->count() - 1, device.description, Qt::ToolTipRole );
    }

    if ( devices.isEmpty() ) {
        statusLabel_->setText( error.isEmpty() ? tr( "No iOS devices detected." ) : error );
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
}

void IosLogDialog::saveSettings() const
{
    auto& config = Configuration::get();
    config.setIosLogExecutable( executableEdit_->text().trimmed() );
    config.setIosLogExtraArgs( extraArgsEdit_->text().trimmed() );
    config.setIosLogAnsiOutputEnabled( ansiOutputCheckBox_->isChecked() );
    config.save();
}
