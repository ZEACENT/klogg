/*
 * Copyright (C) 2026
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

#pragma once

#include <QProcess>
#include <QString>

namespace klogg::testing {

// Qt-reported facts only; neither cached state nor PID proves native reap or liveness.
struct ProcessSnapshot {
    QProcess::ProcessState state;
    QProcess::ProcessError error;
    QString errorText;
    qint64 reportedPid;

    static ProcessSnapshot take( const QProcess& process )
    {
        return { process.state(), process.error(), process.errorString(), process.processId() };
    }

    QString diagnostic() const
    {
        const auto boundedError = errorText.size() > 512
            ? errorText.left( 500 ) + QStringLiteral( "<truncated>" ) : errorText;
        return QStringLiteral( "primary_state=%1 primary_error=%2 primary_error_text=%3 "
                               "primary_reported_pid=%4" )
            .arg( QString::number( static_cast<int>( state ) ),
                  QString::number( static_cast<int>( error ) ), boundedError,
                  QString::number( reportedPid ) );
    }
};

} // namespace klogg::testing
