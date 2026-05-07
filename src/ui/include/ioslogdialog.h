#ifndef IOSLOGDIALOG_H
#define IOSLOGDIALOG_H

#include <QDialog>

#include "adblogcatsource.h"

class QComboBox;
class QDialogButtonBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

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

  private:
    QLineEdit* executableEdit_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QComboBox* deviceCombo_ = nullptr;
    QLineEdit* extraArgsEdit_ = nullptr;
    QCheckBox* ansiOutputCheckBox_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
};

#endif
