#ifndef ADBPROCESSTRANSPORT_H
#define ADBPROCESSTRANSPORT_H

#include <QList>
#include <memory>

#include "adbdeviceinfo.h"
#include "adbdevicelistprovider.h"
#include "livesourcetransport.h"

class AdbProcessTransport : public ProcessLiveSourceTransport {
    Q_OBJECT

  public:
    AdbProcessTransport( QString adbExecutable, QString deviceSerial, QString extraArgs,
                         bool ansiOutputEnabled = false, QObject* parent = nullptr );

    // Delegate to AdbDeviceListProvider.  Kept for backward compatibility
    // with dialog callers that call this static method directly.
    static QList<AdbDeviceInfo> listDevices( const QString& adbExecutable, QString* error );

    // Probe well-known adb install locations and return the absolute path of
    // the first executable hit.
    static QString detectAdbExecutable();

    // Access the device list provider for async enumeration.
    AdbDeviceListProvider* deviceListProvider() const;

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
    bool ansiOutputEnabled_;
    std::unique_ptr<AdbDeviceListProvider> deviceProvider_;
};

#endif
