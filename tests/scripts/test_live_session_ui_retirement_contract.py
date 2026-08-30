"""Task 6 cycle 2 RED contract: live-source session UI retirement.

Grep-level source contracts for retiring the raw executable/argument UI from
the live-log new-session paths, keeping naming source-neutral, and carrying
i18n keys for every new live-session diagnostic. Runs against the working
tree; each failure names the retired pattern that must disappear.
"""

from __future__ import annotations

import pathlib
import re
import unittest
import xml.etree.ElementTree as ET


ROOT = pathlib.Path(__file__).parents[2]

ADB_DIALOG_FILES = (
    "src/ui/include/adblogcatdialog.h",
    "src/ui/src/adblogcatdialog.cpp",
)
IOS_DIALOG_FILES = (
    "src/ui/include/ioslogdialog.h",
    "src/ui/src/ioslogdialog.cpp",
)
OPTIONS_DIALOG_FILES = (
    "src/ui/include/optionsdialog.h",
    "src/ui/include/optionsdialog.ui",
    "src/ui/src/optionsdialog.cpp",
)
MAINWINDOW = "src/ui/src/mainwindow.cpp"
CONFIGURATION_HEADER = "src/settings/include/configuration.h"
CONFIGURATION_SOURCE = "src/settings/src/configuration.cpp"

# Tokens that must never appear in any live-session dialog translation unit:
# the dialogs compose typed sessions and must not resolve anything through
# PATH, an SDK, a Python environment, or a spawned process.
ENVIRONMENT_FREE_TOKENS = (
    "QFileDialog",
    "QStandardPaths",
    "findExecutable",
    "qgetenv",
    "qEnvironmentVariable",
    "ANDROID_HOME",
    "ANDROID_SDK_ROOT",
    "ANDROID_SDK_ROOT_WINDOWS",
    "PYTHONHOME",
    "PYTHONPATH",
    "QProcess",
)

LIVELOG_MESSAGES_CONTEXT = "klogg::livelog::messages"

# Distinctive stable prefixes of every user-facing live-session diagnostic.
# Matched as <source> prefixes so minor trailing rewording does not force
# catalog churn while still failing loudly when a key disappears.
REQUIRED_LIVELOG_MESSAGE_PREFIXES = (
    # legacy-raw-cli-options-unsupported: reopen guidance is contractual.
    "This live log session was saved with a custom executable",
    # migrated-pre-discriminator-session
    "This session predates transport selection",
    # compatibility-transport read-only state (cycle 2)
    "This session uses a compatibility transport and opens read-only",
    # transitional-backend-not-creatable
    "New sessions cannot use compatibility process transports.",
    # running-intent-requires-device
    "A detected device is required before the session can run",
    # invalid-capture-id
    "The session has no usable capture identifier.",
    # unknown-source-kind
    "The saved live-log session has an unrecognized source type",
    # malformed-session-spec
    "The saved live-log session payload is not a valid session object.",
    # unsupported-schema-version
    "This session was written by a newer version of klogg",
    # malformed-schema-version
    "The saved live-log session has a malformed schema version",
    # malformed-capture-id
    "The saved live-log session has no usable capture identifier;",
    # unknown-android-backend
    "Saved Android transport",
    # unknown-ios-backend
    "Saved iOS transport",
    # active capture storage collision
    "Another live session already uses this capture identifier",
    # unavailable native/application-owned factory during restore
    "The saved live-log transport is unavailable",
    # typed Android option validation
    "The saved Android process filter must be a non-negative PID.",
    "The saved Android buffers, priority, or filter expression is invalid.",
    # native iOS currently rejects unimplemented typed filtering/JSON semantics
    "Saved iOS level, category, subsystem, and JSON options are not supported",
)

REQUIRED_MAINWINDOW_MESSAGES = ("Live log sessions",)
REQUIRED_CONTEXT_MESSAGES = {
    "AdbLogcatSource": (
        "This compatibility session is read-only.",
        "Live log transport is unavailable.",
    ),
    "OptionsDialog": (
        "Keep all",
        "Number of old capture files to keep when rolling. Older files beyond this count are automatically deleted. Set to 0 to keep all rotated files.",
        "Number of backup capture files to retain during rotation. Older files beyond this count are deleted. Set to 0 to keep all rotated files.",
    ),
}


def read_repo_text(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def cpp_function_body(source_text: str, signature_prefix: str) -> str:
    """Return the brace-matched body of the first function whose signature
    starts with signature_prefix."""
    index = source_text.find(signature_prefix)
    if index < 0:
        raise AssertionError(f"function not found: {signature_prefix}")
    brace_start = source_text.index("{", index)
    depth = 0
    for position in range(brace_start, len(source_text)):
        character = source_text[position]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source_text[brace_start : position + 1]
    return source_text[brace_start:]


def message_box_titles(body: str) -> list[str]:
    return [
        match.group("title")
        for match in re.finditer(
            r'QMessageBox::(?:warning|information|critical|question)\s*\([^;]*?tr\s*\(\s*'
            r'"(?P<title>(?:\\.|[^"\\])*)"',
            body,
            re.DOTALL,
        )
    ]


def catalog_message_sources(ts_path: pathlib.Path) -> dict[str, set[str]]:
    tree = ET.parse(ts_path)
    catalogs: dict[str, set[str]] = {}
    for context in tree.getroot().findall("context"):
        name = context.findtext("name")
        if name is None:
            continue
        sources = {
            message.findtext("source") or ""
            for message in context.findall("message")
            if (message.find("translation") is not None)
            and (message.find("translation").attrib.get("type") != "obsolete")
        }
        catalogs.setdefault(name, set()).update(sources)
    return catalogs


class LiveSessionUiRetirementContract(unittest.TestCase):
    def test_adb_dialog_constructs_no_executable_or_argument_edits(self):
        for relative_path in ADB_DIALOG_FILES:
            text = read_repo_text(relative_path)
            for retired in (
                "adbExecutableEdit_",
                "extraArgsEdit_",
                '"adbExecutableEdit"',
                '"extraArgsEdit"',
                "setAdbExecutable",
                "adbExecutable()",
                "setAdbLogcatExtraArgs",
                "adbLogcatExtraArgs()",
            ):
                self.assertNotIn(
                    retired,
                    text,
                    f"{relative_path}: retired edit field {retired} must be removed",
                )

    def test_adb_dialog_has_no_detect_button(self):
        for relative_path in ADB_DIALOG_FILES:
            text = read_repo_text(relative_path)
            # 'tr( "Detecting' status text is fine; only a Detect BUTTON
            # affordance is retired.
            for retired in ('tr( "Detect"', '"detectButton"', "detectAdbExecutable"):
                self.assertNotIn(
                    retired,
                    text,
                    f"{relative_path}: retired Detect affordance {retired} must stay absent",
                )

    def test_preferences_have_no_legacy_process_transport_editor(self):
        retired = (
            "adbExecutableLineEdit",
            "adbLogcatArgsLineEdit",
            "adbDetectButton",
            "iosLogExecutableLineEdit",
            "iosLogArgsLineEdit",
            "iosLogDetectButton",
            "setAdbExecutable",
            "setAdbLogcatExtraArgs",
            "setIosLogExecutable",
            "setIosLogExtraArgs",
            "detectAdbExecutable",
            "detectIosSyslogExecutable",
            "ADB executable",
            "Extra logcat args",
            "pymobiledevice3 executable",
            "Extra iOS log args",
        )
        for relative_path in OPTIONS_DIALOG_FILES:
            text = read_repo_text(relative_path)
            for token in retired:
                self.assertNotIn(
                    token,
                    text,
                    f"{relative_path}: preferences must not expose retired process option {token}",
                )

    def test_configuration_scrubs_but_never_rewrites_retired_process_keys(self):
        header = read_repo_text(CONFIGURATION_HEADER)
        source = read_repo_text(CONFIGURATION_SOURCE)
        for api in (
            "adbExecutable()",
            "setAdbExecutable",
            "adbLogcatExtraArgs()",
            "setAdbLogcatExtraArgs",
            "iosLogExecutable()",
            "setIosLogExecutable",
            "iosLogExtraArgs()",
            "setIosLogExtraArgs",
        ):
            self.assertNotIn(api, header)
        for key in (
            "adb.executable",
            "adb.logcatExtraArgs",
            "iosLog.executable",
            "iosLog.extraArgs",
        ):
            self.assertNotRegex(
                source,
                rf"setValue\s*\(\s*(?:QStringLiteral\s*\()?\s*\"{re.escape(key)}\"",
                f"{key} may be removed during migration but must never be persisted again",
            )

    def test_default_dialogs_do_not_construct_legacy_discovery_providers(self):
        forbidden_construction = {
            "src/ui/src/adblogcatdialog.cpp": "AdbDeviceListProvider provider",
            "src/ui/src/ioslogdialog.cpp": "IosDeviceListProvider provider",
        }
        for relative_path, token in forbidden_construction.items():
            self.assertNotIn(
                token,
                read_repo_text(relative_path),
                f"{relative_path}: missing injection must fail closed, not discover executables",
            )

    def test_live_session_dialogs_are_toolchain_environment_free(self):
        for relative_path in ADB_DIALOG_FILES + IOS_DIALOG_FILES:
            text = read_repo_text(relative_path)
            for token in ENVIRONMENT_FREE_TOKENS:
                self.assertNotIn(
                    token,
                    text,
                    f"{relative_path}: new-session paths must not consult "
                    f"{token} (PATH/SDK/Python detection is retired)",
                )

    def test_ios_dialog_exposes_no_adb_named_ui_or_executable_fields(self):
        for relative_path in IOS_DIALOG_FILES:
            text = read_repo_text(relative_path)
            for retired in (
                "executableEdit_",
                "extraArgsEdit_",
                '"iosLogExecutableEdit"',
                "QLineEdit",
                "QFileDialog",
            ):
                self.assertNotIn(
                    retired,
                    text,
                    f"{relative_path}: retired iOS dialog field {retired} must be removed",
                )
            self.assertNotRegex(
                text,
                r"pymobiledevice3",
                f"{relative_path}: the Python sidecar must not be named by the iOS dialog",
            )
            for match in re.finditer(r'tr\s*\(\s*"((?:\\.|[^"\\])*)"', text):
                literal = match.group(1)
                self.assertIsNone(
                    re.search(r"(?i)(adb|executable|args\b)", literal),
                    f"{relative_path}: tr() string {literal!r} is not source-neutral",
                )
            self.assertNotRegex(
                text,
                r"\badb\b",
                f"{relative_path}: standalone ADB identifiers are not source-neutral",
            )

    def test_ios_setup_failures_never_show_adb_titled_warnings(self):
        text = read_repo_text(MAINWINDOW)

        ios_body = cpp_function_body(text, "void MainWindow::openIosLogStream()")
        self.assertNotIn(
            "ADB",
            ios_body,
            f"{MAINWINDOW}: openIosLogStream must not name ADB anywhere",
        )
        for title in message_box_titles(ios_body):
            self.assertNotIn(
                "adb",
                title.lower(),
                f"{MAINWINDOW}: iOS setup warnings must not be ADB-titled, saw {title!r}",
            )

        shared_body = cpp_function_body(text, "bool MainWindow::openAdbLogcatSource(")
        for title in message_box_titles(shared_body):
            self.assertNotIn(
                "adb",
                title.lower(),
                f"{MAINWINDOW}: openAdbLogcatSource serves both sources; its "
                f"warning titles must be source-neutral, saw {title!r}",
            )


class LiveSessionI18nContract(unittest.TestCase):
    def test_every_language_catalog_carries_the_rejection_dialog_title(self):
        for language in ("en", "zh_CN", "zh_TW"):
            with self.subTest(language=language):
                catalogs = catalog_message_sources(
                    ROOT / "src" / "app" / "i18n" / f"{language}.ts"
                )
                mainwindow_sources = catalogs.get("MainWindow", set())
                for message in REQUIRED_MAINWINDOW_MESSAGES:
                    self.assertIn(
                        message,
                        mainwindow_sources,
                        f"{language}.ts: MainWindow context is missing {message!r}",
                    )

    def test_every_language_catalog_carries_source_neutral_live_ui_messages(self):
        for language in ("en", "zh_CN", "zh_TW"):
            with self.subTest(language=language):
                catalogs = catalog_message_sources(
                    ROOT / "src" / "app" / "i18n" / f"{language}.ts"
                )
                for context, messages in REQUIRED_CONTEXT_MESSAGES.items():
                    self.assertIn(context, catalogs, f"{language}.ts: missing {context}")
                    for message in messages:
                        self.assertIn(
                            message,
                            catalogs[context],
                            f"{language}.ts: {context} is missing {message!r}",
                        )

    def test_every_language_catalog_carries_the_livelog_diagnostics(self):
        for language in ("en", "zh_CN", "zh_TW"):
            with self.subTest(language=language):
                catalogs = catalog_message_sources(
                    ROOT / "src" / "app" / "i18n" / f"{language}.ts"
                )
                self.assertIn(
                    LIVELOG_MESSAGES_CONTEXT,
                    catalogs,
                    f"{language}.ts: missing translation context "
                    f"{LIVELOG_MESSAGES_CONTEXT} for the live-log diagnostics",
                )
                sources = catalogs[LIVELOG_MESSAGES_CONTEXT]
                for prefix in REQUIRED_LIVELOG_MESSAGE_PREFIXES:
                    self.assertTrue(
                        any(source.startswith(prefix) for source in sources),
                        f"{language}.ts: {LIVELOG_MESSAGES_CONTEXT} is missing the "
                        f"diagnostic starting {prefix!r}",
                    )


if __name__ == "__main__":
    unittest.main()
