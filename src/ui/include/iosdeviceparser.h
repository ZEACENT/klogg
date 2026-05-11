#ifndef IOSDEVICEPARSER_H
#define IOSDEVICEPARSER_H

#include <QByteArray>
#include <QList>
#include <QString>

struct IosDeviceInfo {
    QString udid;
    QString displayName;
    QString description;
    QString productType;
    QString productVersion;
};

QString stripAnsiSequences( const QString& text );
QList<IosDeviceInfo> parsePymobiledeviceDeviceList( const QByteArray& output );
QList<IosDeviceInfo> parsePymobiledeviceSimpleDeviceList( const QByteArray& output );

#endif // IOSDEVICEPARSER_H
