#ifndef ADBLOGCATDIALOG_H
#define ADBLOGCATDIALOG_H

#include <QDialog>
#include <QList>

#include <optional>

#include "adbdevicelistprovider.h"
#include "adbinfrastructuremanager.h"
#include "adblogcatsource.h"

class AdbTrackedDeviceProvider;
class QComboBox;
class QDialogButtonBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;

class AdbLogcatDialog : public QDialog {
    Q_OBJECT

public:
    explicit AdbLogcatDialog( QWidget* parent = nullptr );
    explicit AdbLogcatDialog(
        DeviceListProviderBase<AdbDeviceInfo>::AsyncListOperation discoveryOperation,
        QWidget* parent = nullptr );
    AdbLogcatDialog( AdbTrackedDeviceProvider& provider, QString explicitDeviceSerial,
                     QWidget* parent = nullptr );

    AdbLogcatSessionData sessionData() const;

private Q_SLOTS:
    void refreshDevices();

private:
    AdbLogcatDialog( DeviceListProviderBase<AdbDeviceInfo>::AsyncListOperation discoveryOperation,
                     AdbTrackedDeviceProvider* trackedProvider, QString explicitDeviceSerial,
                     QWidget* parent );
    void initializeUi();
    void updateAcceptState();
    void loadSettings();
    void saveSettings() const;
    void applyDiscoveryResult( DeviceDiscoveryResult<AdbDeviceInfo> result );
    void applyTrackedSnapshot( const DeviceDiscoveryResult<AdbDeviceInfo>& result );
    void populateDevices( const QList<AdbDeviceInfo>& devices,
                          const std::optional<klogg::livecapture::LiveSourceError>& error );

private:
    DeviceListProviderBase<AdbDeviceInfo>::AsyncListOperation discoveryOperation_;
    AdbTrackedDeviceProvider* trackedProvider_{ nullptr };
    klogg::livecapture::adb::AdbInfrastructureLease infrastructureLease_;
    DeviceDiscoveryCoordinator<AdbDeviceInfo> discoveryCoordinator_;
    klogg::livecapture::Generation latestTrackedGeneration_{ 0 };
    QString requestedDeviceSerial_;
    unsigned pendingRefreshCount_{ 0 };
    QPushButton* refreshButton_ = nullptr;
    QComboBox* deviceCombo_ = nullptr;
    QCheckBox* ansiOutputCheckBox_ = nullptr;
    QCheckBox* autoReconnectCheckBox_ = nullptr;
    QSpinBox* maxAttemptsSpinBox_ = nullptr;
    QSpinBox* maxFileSizeSpinBox_ = nullptr;
    QSpinBox* backupCountSpinBox_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
};

#endif
