import hashlib
import json
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).parents[2]
THIRD_PARTY_CMAKE = ROOT / "3rdparty" / "CMakeLists.txt"
PREFETCH_CMAKE = ROOT / "cmake" / "prefetch_cpm" / "CMakeLists.txt"
LOCK = ROOT / "3rdparty" / "libimobiledevice" / "libimobiledevice.lock.json"
PATCH = (
    ROOT
    / "3rdparty"
    / "libimobiledevice"
    / "patches"
    / "0001-fix-ostrace-live-packet-leak.patch"
)
INTERRUPT_PATCH = (
    ROOT
    / "3rdparty"
    / "libimobiledevice"
    / "patches"
    / "0002-interrupt-live-capture-receive.patch"
)
PASSIVE_HANDSHAKE_PATCH = (
    ROOT
    / "3rdparty"
    / "libimobiledevice"
    / "patches"
    / "0003-passive-lockdown-handshake.patch"
)
SYSLOG_TERMINAL_PATCH = (
    ROOT
    / "3rdparty"
    / "libimobiledevice"
    / "patches"
    / "0004-syslog-terminal-callback.patch"
)

LIBIMOBILEDEVICE_VERSION = "1.4.0"
LIBIMOBILEDEVICE_COMMIT = "149f7623c672c1fa73122c7119a12bfc0012f2ac"
LIBIMOBILEDEVICE_ARCHIVE_SHA256 = (
    "7feac29cd037470b2dfaae07947bdc28248597605c62c2ff74547301dc238c17"
)
UPSTREAM_LEAK_FIX_COMMIT = "5ca453f9e3950b1f24b51e4cdf255236e34254c4"
UPSTREAM_LEAK_FIX_SUBJECT = "ostrace: Fix memory leak (#1677)"
HEX64 = re.compile(r"[0-9a-f]{64}")


def required_text(path: pathlib.Path) -> str:
    if not path.is_file():
        raise AssertionError(f"required vendored contract file is missing: {path}")
    return path.read_text(encoding="utf-8")


def package_call(source: str) -> str:
    calls = []
    search_from = 0
    opener = re.compile(r"\bcpmaddpackage\s*\(", re.IGNORECASE)
    while match := opener.search(source, search_from):
        depth = 1
        cursor = match.end()
        quote = None
        escaped = False
        while cursor < len(source) and depth:
            character = source[cursor]
            if quote is not None:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
                    quote = None
            elif character in {'"', "'"}:
                quote = character
            elif character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
            cursor += 1
        if depth:
            raise AssertionError("unterminated cpmaddpackage call")
        calls.append(source[match.end() : cursor - 1])
        search_from = cursor

    matching = [
        call
        for call in calls
        if re.search(r"\bNAME\s+libimobiledevice(?=\s|$)", call, re.IGNORECASE)
    ]
    if len(matching) != 1:
        raise AssertionError(
            f"expected exactly one CPM package named libimobiledevice, found {len(matching)}"
        )
    return matching[0]


class LibimobiledeviceOsTracePatchContractTest(unittest.TestCase):
    def lock(self) -> dict:
        try:
            document = json.loads(required_text(LOCK))
        except json.JSONDecodeError as error:
            self.fail(f"libimobiledevice lock is not valid JSON: {error}")
        self.assertIsInstance(document, dict)
        return document

    def locked_source(self, document: dict) -> dict:
        sources = document.get("sources")
        self.assertIsInstance(sources, list, "lock.sources must be a JSON array")
        matches = [source for source in sources if source.get("id") == "libimobiledevice"]
        self.assertEqual(
            len(matches),
            1,
            "lock.sources must contain exactly one libimobiledevice entry",
        )
        return matches[0]

    def locked_patch(self, document: dict) -> dict:
        patches = document.get("patches")
        self.assertIsInstance(patches, list, "lock.patches must be a JSON array")
        expected_path = "patches/0001-fix-ostrace-live-packet-leak.patch"
        matches = [patch for patch in patches if patch.get("path") == expected_path]
        self.assertEqual(
            len(matches),
            1,
            f"lock.patches must contain exactly one {expected_path} entry",
        )
        return matches[0]

    def test_lock_pins_exact_1_4_0_tag_commit_and_archive_sha(self):
        document = self.lock()
        self.assertGreaterEqual(document.get("schema_version", 0), 2)
        source = self.locked_source(document)
        self.assertEqual(source.get("version"), LIBIMOBILEDEVICE_VERSION)
        self.assertEqual(source.get("tag"), LIBIMOBILEDEVICE_VERSION)
        self.assertEqual(source.get("commit"), LIBIMOBILEDEVICE_COMMIT)
        self.assertEqual(
            source.get("archive_sha256"), LIBIMOBILEDEVICE_ARCHIVE_SHA256
        )
        archive_url = source.get("archive_url", "")
        self.assertTrue(archive_url.startswith("https://"))
        self.assertIn(
            LIBIMOBILEDEVICE_COMMIT,
            archive_url,
            "the archive URL must name the immutable 1.4.0 commit, not a mutable tag",
        )

    def test_build_and_prefetch_pin_the_exact_1_4_0_release_commit(self):
        for path in (THIRD_PARTY_CMAKE, PREFETCH_CMAKE):
            call = package_call(path.read_text(encoding="utf-8"))
            self.assertRegex(
                call,
                rf"\bGIT_TAG\s+{LIBIMOBILEDEVICE_COMMIT}\b",
                f"{path} must pin libimobiledevice 1.4.0 by immutable commit",
            )

    def test_build_applies_the_locked_vendored_leak_fix_patch(self):
        call = package_call(THIRD_PARTY_CMAKE.read_text(encoding="utf-8"))
        self.assertIn(PATCH.name, call)
        self.assertRegex(call, r"\bPATCH_COMMAND\b")
        self.assertRegex(call, r"\bgit\s+apply\b|\$\{GIT_EXECUTABLE\}\s+apply\b")

    def test_lock_records_upstream_fix_and_borrowed_callback_ownership(self):
        patch = self.locked_patch(self.lock())
        self.assertEqual(patch.get("upstream_commit"), UPSTREAM_LEAK_FIX_COMMIT)
        self.assertEqual(patch.get("upstream_subject"), UPSTREAM_LEAK_FIX_SUBJECT)
        self.assertEqual(
            patch.get("callback_buffer_ownership"),
            "borrowed-until-callback-returns",
            "the callback buffer is freed immediately after callback return, so klogg must copy it",
        )
        locked_sha = patch.get("sha256", "")
        self.assertRegex(locked_sha, HEX64)
        self.assertEqual(hashlib.sha256(PATCH.read_bytes()).hexdigest(), locked_sha)

    def test_patch_records_upstream_provenance_and_exact_ownership_order(self):
        source = required_text(PATCH)
        self.assertIn(UPSTREAM_LEAK_FIX_COMMIT, source)
        self.assertIn(UPSTREAM_LEAK_FIX_SUBJECT, source)
        self.assertIn("diff --git a/src/ostrace.c b/src/ostrace.c", source)
        self.assertNotIn("include/libimobiledevice/ostrace.h", source)
        self.assertEqual(source.count("diff --git "), 1)

        callback = "oswt->cbfunc(buf, received, oswt->user_data);"
        release = "free(buf);"
        self.assertIn(callback, source)
        self.assertIn(release, source)
        self.assertLess(
            source.index(callback),
            source.index(release),
            "the library callback receives borrowed bytes; release must happen only after it returns",
        )

    def test_patch_does_not_transfer_or_extend_callback_buffer_ownership(self):
        source = required_text(PATCH)
        added_lines = [
            line[1:]
            for line in source.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        ]
        self.assertEqual(
            [line.strip() for line in added_lines if line.strip()],
            ["free(buf);"],
            "the patch must remain the one-line upstream free-after-callback change",
        )

    def test_interrupt_patch_is_locked_with_bounded_stop_and_one_shot_provenance(self):
        document = self.lock()
        expected_path = "patches/0002-interrupt-live-capture-receive.patch"
        matches = [
            patch
            for patch in document.get("patches", [])
            if patch.get("path") == expected_path
        ]
        self.assertEqual(
            len(matches),
            1,
            f"lock.patches must contain exactly one {expected_path} entry",
        )
        patch = matches[0]
        self.assertTrue(patch.get("required"))
        self.assertEqual(patch.get("source_id"), "libimobiledevice")
        self.assertEqual(
            patch.get("purpose"), "interrupt-partial-frame-live-capture-receive"
        )
        self.assertEqual(patch.get("client_reuse_after_stop"), "forbidden")
        self.assertLessEqual(
            patch.get("bounded_stop_deadline_ms", 30_001),
            1_000,
            "native stop must not inherit the upstream 30 second partial-frame timeout",
        )
        self.assertEqual(patch.get("maximum_live_packet_bytes"), 16 * 1024 * 1024)
        self.assertLessEqual(patch.get("receive_poll_interval_ms", 30_001), 500)
        self.assertEqual(patch.get("partial_frame_timeout_policy"), "continue-until-cancelled")
        locked_sha = patch.get("sha256", "")
        self.assertRegex(locked_sha, HEX64)
        self.assertEqual(hashlib.sha256(INTERRUPT_PATCH.read_bytes()).hexdigest(), locked_sha)

    def test_build_applies_interrupt_patch_after_the_upstream_leak_fix(self):
        call = package_call(THIRD_PARTY_CMAKE.read_text(encoding="utf-8"))
        self.assertIn(PATCH.name, call)
        self.assertIn(INTERRUPT_PATCH.name, call)
        self.assertLess(
            call.index(PATCH.name),
            call.index(INTERRUPT_PATCH.name),
            "the cancellation patch is based on the already leak-fixed 1.4.0 tree",
        )

    def test_interrupt_patch_targets_live_receive_cancellation_not_callback_cleanup(self):
        source = required_text(INTERRUPT_PATCH)
        self.assertIn("diff --git a/src/ostrace.c b/src/ostrace.c", source)
        self.assertRegex(source, r"ostrace_(stop_activity|worker)")
        self.assertRegex(source, r"(interrupt|shutdown|disconnect|cancel)")
        self.assertNotIn("pair_device", source)
        self.assertNotIn("unpair_device", source)
        added = [
            line[1:].strip()
            for line in source.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        ]
        self.assertIn("ostrace_receive_interruptible", source)
        self.assertIn("OSTRACE_MAX_LIVE_PACKET_SIZE", source)
        self.assertIn("16 * 1024 * 1024", source)
        self.assertIn("while (*received < size)", source)
        self.assertIn("if (res == OSTRACE_E_TIMEOUT)", source)
        self.assertIn("if (!oswt->client->parent)", source)
        self.assertGreaterEqual(
            source.count("ostrace_receive_interruptible(oswt"),
            3,
            "message type, frame length, and payload must all tolerate fragmented reads",
        )
        self.assertNotRegex(
            source,
            r"service_receive_with_timeout\([^\n]*,\s*rlen,\s*&received,\s*500\)",
        )
        self.assertGreaterEqual(added.count("free(buf);"), 2)
        self.assertIn("ostrace_start_activity_with_error", source)
        self.assertRegex(source, r"terminal_(callback|cb)")

    def test_passive_handshake_patch_is_locked_and_never_pairs(self):
        document = self.lock()
        expected_path = "patches/0003-passive-lockdown-handshake.patch"
        matches = [
            patch for patch in document.get("patches", []) if patch.get("path") == expected_path
        ]
        self.assertEqual(len(matches), 1)
        patch = matches[0]
        self.assertTrue(patch.get("required"))
        self.assertEqual(patch.get("purpose"), "existing-pair-only-lockdown-session")
        self.assertEqual(patch.get("pairing_side_effects"), "forbidden")
        self.assertEqual(
            hashlib.sha256(PASSIVE_HANDSHAKE_PATCH.read_bytes()).hexdigest(),
            patch.get("sha256"),
        )

        source = required_text(PASSIVE_HANDSHAKE_PATCH)
        self.assertIn("lockdownd_client_new_with_existing_pair", source)
        added = "\n".join(
            line[1:]
            for line in source.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        self.assertNotIn("lockdownd_pair(", added)
        self.assertNotIn("userpref_save_pair_record", added)
        self.assertIn("userpref_read_pair_record", added)
        self.assertIn("lockdownd_validate_pair", added)
        self.assertIn("lockdownd_start_session", added)

    def test_syslog_terminal_callback_patch_is_locked(self):
        document = self.lock()
        expected_path = "patches/0004-syslog-terminal-callback.patch"
        matches = [
            patch for patch in document.get("patches", []) if patch.get("path") == expected_path
        ]
        self.assertEqual(len(matches), 1)
        patch = matches[0]
        self.assertTrue(patch.get("required"))
        self.assertEqual(patch.get("purpose"), "report-syslog-worker-terminal-error")
        self.assertEqual(
            hashlib.sha256(SYSLOG_TERMINAL_PATCH.read_bytes()).hexdigest(),
            patch.get("sha256"),
        )
        source = required_text(SYSLOG_TERMINAL_PATCH)
        self.assertIn("syslog_relay_start_capture_raw_with_error", source)
        self.assertRegex(source, r"terminal_(callback|cb)")

    def test_build_applies_the_complete_runtime_patch_series_in_order(self):
        call = package_call(THIRD_PARTY_CMAKE.read_text(encoding="utf-8"))
        names = [PATCH.name, INTERRUPT_PATCH.name, PASSIVE_HANDSHAKE_PATCH.name,
                 SYSLOG_TERMINAL_PATCH.name]
        for name in names:
            self.assertIn(name, call)
        self.assertEqual(sorted(call.index(name) for name in names),
                         [call.index(name) for name in names])


if __name__ == "__main__":
    unittest.main()
