---
weight: 5
bookFlatSection: true
---

## Privacy policy

**Scope:** this page retains the upstream privacy notice. In this fork,
crash-reporting availability depends on the build and package; upstream service
and retention statements below are not newly verified commitments by this fork's
maintainer. Refer to the selected release and its settings rather than assuming
all packages contain the same reporting functionality.

We strongly support your right to privacy when using klogg.

Our privacy policy is simple: your data is none of our business. 

To the extent that klogg app and website can provide their functionality without doing so, we prefer to avoid collecting data from you.

In the cases where we do collect data, we try to be clear about why we're collecting it, tell you how long we keep it, delete it when we no longer need it, and give you the ability to opt out of collection whenever possible.

### Crash Logs
Crash-reporting support is optional (`KLOGG_USE_SENTRY`) and is not compiled
into every package. When included and enabled, it can collect crash diagnostics
to help identify a failure. These "crash logs" can contain processor and operating
system information, process and thread details, stack traces, and loaded modules.
Review a report before sharing it; do not assume diagnostics are free of sensitive
information merely because they are intended for debugging.

Whenever possible, klogg will allow you to review the entire contents of the crash log before you decide whether or not to send it.

The upstream crash-reporting flow asks for confirmation before a crash report
is sent. Update checks, described below, are a separate network operation.

These data is sent for processing to [Sentry](https://sentry.io). Please read their [privacy policy](https://sentry.io/privacy/) and [security](https://sentry.io/security/).

We retain crash logs for 30 days.

Apple may also collect crash logs if the privacy settings of your device allow it.

### Update Checking
By default, klogg periodically check to see if a newer version of the app is available, so that you can be given the choice to update if you wish.

An update check downloads release information from GitHub. It does not upload
the contents of the logs you are viewing. Like other network requests, it exposes
normal connection metadata to the service handling the request.

You may turn off update checking from the app's preferences window.

We do not store any metadata about update requests.

### Data Not Collected
Except as described above, and as required to perform the application's core functionality at the user's request, klogg does not send out any private information. This includes:

 - Your keyboard input
 - Contents of files you are working with
 - Screen contents
 - Hostnames
 - Filenames
 - Usernames
 - Passwords

### Questions and Feedback
Our privacy policies might change or be edited for clarity over time. Up-to-date information will always be available from this page.

GitHub is this project's only communication channel. For a general,
non-sensitive question, use the [repository](https://github.com/ZEACENT/klogg).
Do not include private logs or personal data in public issues. Read the
[security policy](https://github.com/ZEACENT/klogg/blob/master/SECURITY.md)
before sharing sensitive security information.
 
 



