"""Task 6 cycle 3 RED source-ownership contracts.

The live-state reducer stays pure in livecapture, while one UI/application-layer
LiveLogController per live tab owns reducer dispatch, generation-tagged effects,
and retry scheduling. MainWindow may render projectLiveState output but may not
infer connection state or own a global reconnect countdown. AdbLogcatSource is
reduced to transport/capture work and may not retain a second reconnect state
machine.
"""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).parents[2]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


class LiveLogControllerOwnershipContract(unittest.TestCase):
    def test_controller_is_application_layer_not_livecapture_domain(self):
        self.assertTrue(
            (ROOT / "src/ui/include/livelogcontroller.h").is_file(),
            "LiveLogController belongs in src/ui/include (application layer)",
        )
        self.assertTrue(
            (ROOT / "src/ui/src/livelogcontroller.cpp").is_file(),
            "LiveLogController implementation belongs in src/ui/src",
        )
        self.assertFalse(
            (ROOT / "src/livecapture/include/livelogcontroller.h").exists(),
            "the pure livecapture domain must not execute transport effects",
        )

    def test_session_owns_one_controller_with_each_live_tab(self):
        header = read("src/ui/include/session.h")
        self.assertIn(
            "LiveLogController",
            header,
            "Session must name the application-layer per-tab controller",
        )
        self.assertRegex(
            header,
            r"(?:shared_ptr|unique_ptr)\s*<\s*(?:klogg::livelog::)?LiveLogController\s*>",
            "Session::OpenFile must own the per-tab controller lifetime",
        )
        self.assertIn(
            "getLiveLogController",
            header,
            "MainWindow must query the tab controller, not AdbLogcatSource state",
        )

    def test_mainwindow_has_no_global_reconnect_clock_or_countdown_owner(self):
        header = read("src/ui/include/mainwindow.h")
        source = read("src/ui/src/mainwindow.cpp")
        retired = (
            "reconnectCountdownTimer_",
            "reconnectCountdownCrawler_",
            "reconnectCountdownEndMs_",
            "reconnectCountdownTotalMs_",
            "startReconnectCountdown",
            "stopReconnectCountdown",
            "updateReconnectCountdown",
        )
        for token in retired:
            self.assertNotIn(
                token,
                header,
                f"MainWindow header retains global reconnect owner {token}",
            )
            self.assertNotIn(
                token,
                source,
                f"MainWindow source retains global reconnect owner {token}",
            )

        self.assertNotIn(
            "QDateTime::currentMSecsSinceEpoch",
            source,
            "MainWindow must not infer a per-tab countdown from wall-clock time",
        )

    def test_mainwindow_renders_projection_without_transport_state_inference(self):
        source = read("src/ui/src/mainwindow.cpp")
        forbidden = (
            "isAutoReconnectActive()",
            "reconnectAttempt()",
            "reconnectRemainingMs()",
            "AdbLogcatSource::State::Connected",
            "AdbLogcatSource::State::Disconnected",
            "AdbLogcatSource::State::Error",
        )
        for token in forbidden:
            self.assertNotIn(
                token,
                source,
                f"MainWindow must not infer live UI state from {token}",
            )

        self.assertIn(
            "projectLiveState",
            source,
            "tab icon/tooltip/status/actions/countdown must originate in the pure projection",
        )
        self.assertIn(
            "getLiveLogController",
            source,
            "MainWindow must obtain the current per-tab snapshot from Session",
        )

    def test_adblogcatsource_has_no_duplicate_reconnect_state_machine(self):
        header = read("src/ui/include/adblogcatsource.h")
        source = read("src/ui/src/adblogcatsource.cpp")
        retired = (
            "InitialReconnectDelayMs",
            "MaxReconnectDelayMs",
            "setAutoReconnectEnabled",
            "setAutoReconnectMaxAttempts",
            "isAutoReconnectActive",
            "reconnectAttempt",
            "reconnectRemainingMs",
            "cancelAutoReconnect",
            "scheduleReconnect",
            "attemptReconnect",
            "reconnectTimer_",
            "reconnectAttempt_",
            "reconnectionProven_",
            "reconnectingActive_",
            "QRandomGenerator",
        )
        for token in retired:
            self.assertNotIn(
                token,
                header,
                f"AdbLogcatSource header retains controller-owned state {token}",
            )
            self.assertNotIn(
                token,
                source,
                f"AdbLogcatSource source retains controller-owned state {token}",
            )

    def test_manual_reconnect_never_pre_writes_a_success_marker(self):
        source = read("src/ui/src/adblogcatsource.cpp")
        self.assertNotIn("----- reconnected", source)
        reconnect_match = re.search(
            r"bool\s+AdbLogcatSource::reconnectSource\s*\(\s*\)\s*\{(?P<body>.*?)\n\}",
            source,
            re.DOTALL,
        )
        if reconnect_match is not None:
            body = reconnect_match.group("body")
            self.assertNotIn("appendUtf8", body)
            self.assertNotIn("appendBytes", body)

    def test_manual_run_intent_changes_schedule_session_persistence(self):
        source = read("src/ui/src/mainwindow.cpp")
        for signature in (
            "void MainWindow::disconnectCurrentSource()",
            "void MainWindow::reconnectCurrentSource()",
        ):
            start = source.index(signature)
            body_start = source.index("{", start)
            depth = 0
            body_end = body_start
            for body_end in range(body_start, len(source)):
                if source[body_end] == "{":
                    depth += 1
                elif source[body_end] == "}":
                    depth -= 1
                    if depth == 0:
                        break
            body = source[body_start : body_end + 1]
            self.assertIn(
                "scheduleSessionPersistence()",
                body,
                f"{signature} must persist the controller-owned run intent",
            )

    def test_livestate_reducer_remains_transport_and_qt_free(self):
        header = read("src/livecapture/include/livestate.h")
        source = read("src/livecapture/src/livestate.cpp")
        for token in (
            "LiveSourceTransport",
            "LiveLogController",
            "QObject",
            "QTimer",
            "QDateTime",
            "QString",
        ):
            self.assertNotIn(token, header)
            self.assertNotIn(token, source)


if __name__ == "__main__":
    unittest.main()
