#ifndef IOSLOGPROCESSTRANSPORT_H
#define IOSLOGPROCESSTRANSPORT_H

#include <QList>

#include "livesourcetransport.h"

struct IosDeviceInfo {
    QString udid;
    QString displayName;
    QString description;
};

class IosLogProcessTransport : public ProcessLiveSourceTransport {
    Q_OBJECT

  public:
    IosLogProcessTransport( QString executable, QString deviceUdid, QString extraArgs,
                            bool ansiOutputEnabled = false, QObject* parent = nullptr );

    static QList<IosDeviceInfo> listDevices( const QString& executable, QString* error );
    static QString detectIosSyslogExecutable();

    bool clearRemote( QString* error ) override;

  protected:
    Command streamingCommand() const override;
    Command clearCommand() const override;

  private:
    QString normalizedExecutable() const;
    QStringList streamArguments() const;

    QString executable_;
    QString deviceUdid_;
    QString extraArgs_;
    bool ansiOutputEnabled_;
};

#endif
