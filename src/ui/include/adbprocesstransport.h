#ifndef ADBPROCESSTRANSPORT_H
#define ADBPROCESSTRANSPORT_H

#include <QList>

#include "livesourcetransport.h"

struct AdbDeviceInfo {
    QString serial;
    QString displayName;
    QString description;
};

class AdbProcessTransport : public ProcessLiveSourceTransport {
    Q_OBJECT

  public:
    AdbProcessTransport( QString adbExecutable, QString deviceSerial, QString extraArgs,
                         QObject* parent = nullptr );

    static QList<AdbDeviceInfo> listDevices( const QString& adbExecutable, QString* error );

    // Probe well-known adb install locations and return the absolute path of
    // the first executable hit.  Returns an empty string when no candidate
    // exists.  Useful for "Detect adb" UI affordances; see docs/PORTABILITY.md
    // for the rationale (macOS GUI launchd PATH does not include the typical
    // SDK / Homebrew install directories).
    static QString detectAdbExecutable();

  protected:
    Command streamingCommand() const override;
    Command clearCommand() const override;

  private:
    QString normalizedAdbExecutable() const;
    QStringList logcatArguments() const;

  private:
    QString adbExecutable_;
    QString deviceSerial_;
    QString extraArgs_;
};

#endif
