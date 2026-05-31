/*
 * Copyright (C) 2021 Anton Filimonov
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

#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>
#include <qglobal.h>
#include <qthreadpool.h>
#include <string>
#include <tbb/version.h>

#include "klogg_version.h"

#include "issuereporter.h"

#ifndef KLOGG_BUILD_TYPE
#define KLOGG_BUILD_TYPE Unknown
#endif

#define KLOGG_STR_IMPL(x) #x
#define KLOGG_STR(x) KLOGG_STR_IMPL(x)

// Auto-filled environment section appended to every issue
static constexpr auto EnvironmentFooter
    = "\n---\n"
      "### :computer: Environment\n"
      "- **Klogg:** %1 (built %2, commit `%3`)\n"
      "- **Build type:** %4\n"
      "- **OS:** %5 (%6 %7)\n"
      "- **Architecture:** %8\n"
      "- **Concurrency:** %9\n";

// Qt/TBB versions appended separately to stay within Qt5's 9-arg limit for QString::arg
static constexpr auto QtTbbFooter = "- **Qt:** %1, **TBB:** %2\n";

// Bug report template (Help > Report Issue)
static constexpr auto BugTemplate
    = "### :bug: What is the problem?\n"
      "<!-- A clear description of what the issue is -->\n"
      "\n"
      "\n"
      "### :bulb: What did you expect to see?\n"
      "<!-- What should have happened instead? -->\n"
      "\n"
      "\n"
      "### :footprints: Steps to reproduce\n"
      "<!-- Detailed steps so someone else can reproduce the issue -->\n"
      "1. \n"
      "2. \n"
      "3. \n"
      "\n"
      "### :game_die: How often does this happen?\n"
      "- [ ] Always\n"
      "- [ ] Sometimes (roughly ___% of the time)\n"
      "- [ ] Happened only once / hard to reproduce\n"
      "\n"
      "### :alembic: Version information\n"
      "<!-- If you know which version introduced the issue or which one was fine -->\n"
      "- **First bad version** (if known): \n"
      "- **Last known good version** (if known): \n";

// Crash report template (used by crash handler)
static constexpr auto CrashTemplate
    = "### :boom: Klogg crashed\n"
      "**Crash ID:** `%1`\n"
      "\n"
      "### :footprints: What were you doing when it crashed?\n"
      "<!-- Describe what you were doing right before the crash -->\n"
      "\n"
      "\n"
      "### :game_die: Can you reproduce it?\n"
      "- [ ] Yes, always\n"
      "- [ ] Yes, sometimes\n"
      "- [ ] No, happened only once\n"
      "\n"
      "### :alembic: Version information\n"
      "- **First bad version** (if known): \n"
      "- **Last known good version** (if known): \n";

// Exception report template (used for unexpected runtime errors)
static constexpr auto ExceptionTemplate
    = "### :warning: An unexpected error occurred\n"
      "**Error:** `%1`\n"
      "\n"
      "### :footprints: What were you doing?\n"
      "<!-- Describe what you were doing when the error appeared -->\n"
      "\n"
      "\n"
      "### :footprints: Steps to reproduce\n"
      "<!-- How can someone else trigger this error? -->\n"
      "1. \n"
      "2. \n"
      "\n"
      "### :game_die: How often does this happen?\n"
      "- [ ] Always\n"
      "- [ ] Sometimes\n"
      "- [ ] Once\n"
      "\n"
      "### :alembic: Version information\n"
      "- **First bad version** (if known): \n"
      "- **Last known good version** (if known): \n";

static constexpr auto ExceptionAskUserAction
    = "Ooops! Something unexpected happend. Create issue on Github?";

static constexpr auto AskUserAction = "Create issue on Github?";

void IssueReporter::askUserAndReportIssue( IssueTemplate issueTemplate, const QString& information )
{
    const auto askAction
        = issueTemplate == IssueTemplate::Exception ? ExceptionAskUserAction : AskUserAction;

    if ( QMessageBox::Yes
         == QMessageBox::question( nullptr, "Klogg", askAction, QMessageBox::Yes,
                                   QMessageBox::No ) ) {
        IssueReporter::reportIssue( issueTemplate, information );
    }
}

void IssueReporter::reportIssue( IssueTemplate issueTemplate, const QString& information )
{
    QString body;

    switch ( issueTemplate ) {
    case IssueTemplate::Bug:
        body = QString( BugTemplate );
        break;
    case IssueTemplate::Crash:
        body = QString( CrashTemplate ).arg( information );
        break;
    case IssueTemplate::Exception:
        body = QString( ExceptionTemplate ).arg( information );
        break;
    }

    // Gather system information
    const auto version = kloggVersion();
    const auto buildDate = kloggBuildDate();
    const auto commit = kloggCommit();
    const auto buildType = QString::fromLatin1( KLOGG_STR( KLOGG_BUILD_TYPE ) );
    const auto os = QSysInfo::prettyProductName();
    const auto kernelType = QSysInfo::kernelType();
    const auto kernelVersion = QSysInfo::kernelVersion();
    const auto arch = QSysInfo::currentCpuArchitecture();
    const auto concurrency = QThreadPool::globalInstance()->maxThreadCount();

    body.append( QString( EnvironmentFooter )
                     .arg( version, buildDate, commit, buildType, os, kernelType, kernelVersion,
                           arch, QString::number( concurrency ) ) );
    body.append( QString( QtTbbFooter ).arg( qVersion(), TBB_runtime_version() ) );

    // Construct URL with single-pass percent encoding.
    // QUrl::fromEncoded treats the input as already-encoded and won't re-encode,
    // avoiding the double-encoding that QUrlQuery + setQuery can produce.
    const QByteArray encodedBody = QUrl::toPercentEncoding( body );
    const QByteArray urlBytes
        = QByteArrayLiteral( "https://github.com/ZEACENT/klogg/issues/new?body=" ) + encodedBody;

    QDesktopServices::openUrl( QUrl::fromEncoded( urlBytes ) );
}
