#ifndef ADBLOGCATDIALOG_H
#define ADBLOGCATDIALOG_H

#include <QDialog>

#include "adblogcatsource.h"

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;

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

  private:
    QLineEdit* adbExecutableEdit_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QComboBox* deviceCombo_ = nullptr;
    QLineEdit* extraArgsEdit_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
};

#endif
