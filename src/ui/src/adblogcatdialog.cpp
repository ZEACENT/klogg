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
#include <QVBoxLayout>
#include <QUuid>

#include "adbprocesstransport.h"
#include "configuration.h"

AdbLogcatDialog::AdbLogcatDialog( QWidget* parent )
    : QDialog( parent )
{
    setWindowTitle( tr( "Open ADB Logcat" ) );
    setModal( true );
    resize( 720, 220 );

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

    formLayout->addRow( tr( "ADB executable" ), adbRowLayout );
    formLayout->addRow( tr( "Device" ), deviceCombo_ );
    formLayout->addRow( tr( "Extra logcat args" ), extraArgsEdit_ );
    formLayout->addRow( QString{}, ansiOutputCheckBox_ );

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

    loadSettings();
    refreshDevices();
}

AdbLogcatSessionData AdbLogcatDialog::sessionData() const
{
    return AdbLogcatSessionData{
        adbExecutableEdit_->text().trimmed(),
        deviceCombo_->currentData( Qt::UserRole ).toString(),
        deviceCombo_->currentText(),
        extraArgsEdit_->text().trimmed(),
        QUuid::createUuid().toString( QUuid::WithoutBraces ),
        {},
        LiveLogSourceType::AdbLogcat,
        ansiOutputCheckBox_->isChecked(),
    };
}

void AdbLogcatDialog::refreshDevices()
{
    deviceCombo_->clear();

    QString error;
    const auto devices = AdbProcessTransport::listDevices( adbExecutableEdit_->text(), &error );
    for ( const auto& device : devices ) {
        deviceCombo_->addItem( device.displayName, device.serial );
        deviceCombo_->setItemData( deviceCombo_->count() - 1, device.description, Qt::ToolTipRole );
    }

    if ( devices.isEmpty() ) {
        statusLabel_->setText( error.isEmpty() ? tr( "No online ADB devices detected." ) : error );
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
}

void AdbLogcatDialog::saveSettings() const
{
    auto& config = Configuration::get();
    config.setAdbExecutable( adbExecutableEdit_->text().trimmed() );
    config.setAdbLogcatExtraArgs( extraArgsEdit_->text().trimmed() );
    config.setAdbLogcatAnsiOutputEnabled( ansiOutputCheckBox_->isChecked() );
    config.save();
}
