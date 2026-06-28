#ifndef IOSLOGDIALOG_H
#define IOSLOGDIALOG_H

#include <QDialog>
#include <QFutureWatcher>

#include "adblogcatsource.h"
#include "iosdeviceparser.h"

class QComboBox;
class QDialogButtonBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class IosLogDialog : public QDialog {
    Q_OBJECT

  public:
    explicit IosLogDialog( QWidget* parent = nullptr );

    AdbLogcatSessionData sessionData() const;

  private Q_SLOTS:
    void refreshDevices();

  private:
    void updateAcceptState();
    void loadSettings();
    void saveSettings() const;
    // Populates the device combo from the async refresh result.
    void onDevicesEnumerated();

  private:
    QFutureWatcher<QList<IosDeviceInfo>>* deviceRefreshWatcher_ = nullptr;
    QLineEdit* executableEdit_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QComboBox* deviceCombo_ = nullptr;
    QLineEdit* extraArgsEdit_ = nullptr;
    QCheckBox* ansiOutputCheckBox_ = nullptr;
    QCheckBox* autoReconnectCheckBox_ = nullptr;
    QSpinBox* maxAttemptsSpinBox_ = nullptr;
    QSpinBox* maxFileSizeSpinBox_ = nullptr;
    QSpinBox* backupCountSpinBox_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
};

#endif
