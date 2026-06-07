/*
 * Copyright (C) 2014 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2019 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef VERSIONCHECKER_H
#define VERSIONCHECKER_H

#include <ctime>

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "persistable.h"

// This class holds the configuration options and persistent
// data for the version checker
class VersionCheckerConfig final : public Persistable<VersionCheckerConfig, session_settings> {
  public:
    static const char* persistableName()
    {
        return "VersionCheckerConfig";
    }
    std::time_t nextDeadline() const
    {
        return next_deadline_;
    }
    void setNextDeadline( std::time_t deadline )
    {
        next_deadline_ = deadline;
    }

    QString ignoredVersion() const
    {
        return ignored_version_;
    }
    void setIgnoredVersion( const QString& version )
    {
        ignored_version_ = version;
    }

    // Reads/writes the current config in the QSettings object passed
    void saveToStorage( QSettings& settings ) const;
    void retrieveFromStorage( QSettings& settings );

  private:
    std::time_t next_deadline_ = {};
    QString ignored_version_;
};

// This class compares the current version number with the latest
// stored on a central server
class VersionChecker : public QObject {
    Q_OBJECT

  public:
    VersionChecker();
    ~VersionChecker() override = default;

    // Starts an asynchronous check for a newer version if it is needed.
    // A newVersionFound signal is sent if one is found.
    // In case of error or if no new version is found, no signal is emitted.
    void startCheck();

    // Forces an immediate check for a newer version, bypassing the deadline.
    // Emits newVersionFound if a newer version is found.
    // Emits checkCompleted(false) if no new version is available or on error.
    void forceCheck();

    // Parses version data JSON and checks against current version.
    // Returns true if a newer version was found (and emits newVersionFound).
    // Returns false if no newer version was found.
    bool checkVersionData( QByteArray versionData );

  Q_SIGNALS:
    // New version "version" is available, with downloadUrl pointing to the
    // platform + architecture matched asset (empty if no matching asset found).
    void newVersionFound( const QString& version, const QString& url,
                          const QString& downloadUrl, const QStringList& changes );

    // Check completed without finding a new version.
    // hadError is true when the network request failed or version checking
    // is disabled, so callers can distinguish "no update" from "check failed".
    void checkCompleted( bool newVersionFound, bool hadError = false );

  private:
    // Called on the main thread after the background network request completes
    void processResponse( QByteArray data, bool hadError, bool wasManual );
};

#endif
