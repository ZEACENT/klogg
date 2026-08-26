#ifndef IOSLOGDIALOG_H
#define IOSLOGDIALOG_H

#include <QDialog>

#include "adblogcatsource.h"
#include "ioscatalogprovider.h"
#include "iosdevicelistprovider.h"

class QComboBox;
class QDialogButtonBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;

class IosLogDialog : public QDialog {
    Q_OBJECT

public:
    explicit IosLogDialog( QWidget* parent = nullptr );
    explicit IosLogDialog(
        DeviceListProviderBase<IosDeviceInfo>::AsyncListOperation discoveryOperation,
        QWidget* parent = nullptr );
    explicit IosLogDialog( klogg::livecapture::ios::IosCatalogSnapshotProvider& catalogProvider,
                           QWidget* parent = nullptr );

    AdbLogcatSessionData sessionData() const;

private Q_SLOTS:
    void refreshDevices();

private:
    void updateAcceptState();
    void loadSettings();
    void saveSettings() const;
    void applyDiscoveryResult( DeviceDiscoveryResult<IosDeviceInfo> result );

private:
    DeviceListProviderBase<IosDeviceInfo>::AsyncListOperation discoveryOperation_;
    klogg::livecapture::ios::IosCatalogSnapshotProvider* catalogProvider_{ nullptr };
    DeviceDiscoveryCoordinator<IosDeviceInfo> discoveryCoordinator_;
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
