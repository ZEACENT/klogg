#ifndef ADBLOGCATDIALOG_H
#define ADBLOGCATDIALOG_H

#include <QDialog>
#include <QFutureWatcher>

#include "adbdeviceinfo.h"
#include "adblogcatsource.h"

class QComboBox;
class QDialogButtonBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class AdbLogcatDialog : public QDialog {
    Q_OBJECT

  public:
    explicit AdbLogcatDialog( QWidget* parent = nullptr );

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
    QFutureWatcher<QList<AdbDeviceInfo>>* deviceRefreshWatcher_ = nullptr;
    QLineEdit* adbExecutableEdit_ = nullptr;
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
