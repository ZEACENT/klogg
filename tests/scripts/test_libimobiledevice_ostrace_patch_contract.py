from __future__ import annotations

import hashlib
import json
import pathlib
import re
import unittest
from typing import Optional


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
TYPED_RECORD_PATCH = (
    ROOT
    / "3rdparty"
    / "libimobiledevice"
    / "patches"
    / "0005-ostrace-record-type-callback.patch"
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


def patch_sections(patch: str) -> dict[str, dict[str, str]]:
    sections: dict[str, dict[str, list[str]]] = {}
    current: Optional[str] = None
    in_hunk = False
    for line in patch.splitlines():
        if line.startswith("diff --git "):
            match = re.fullmatch(r"diff --git a/(\S+) b/(\S+)", line)
            if match is None or match.group(1) != match.group(2):
                raise AssertionError("typed-record patch has a malformed or cross-file diff header")
            current = match.group(2)
            if current in sections:
                raise AssertionError(f"typed-record patch repeats diff target {current}")
            sections[current] = {"added": [], "effective": [], "removed": []}
            in_hunk = False
            continue
        if line.startswith("@@"):
            if current is None:
                raise AssertionError("typed-record patch has a hunk outside a diff")
            in_hunk = True
            continue
        if not in_hunk:
            continue
        if line.startswith("+") and not line.startswith("+++"):
            sections[current]["added"].append(line[1:])
            sections[current]["effective"].append(line[1:])
        elif line.startswith("-") and not line.startswith("---"):
            sections[current]["removed"].append(line[1:])
        elif line.startswith(" "):
            sections[current]["effective"].append(line[1:])
        elif line.startswith("\\ No newline"):
            continue
        else:
            in_hunk = False

    if not sections or any(not values["added"] for values in sections.values()):
        raise AssertionError("typed-record patch must contain added code in every target")
    return {
        target: {kind: "\n".join(lines) for kind, lines in values.items()}
        for target, values in sections.items()
    }


def c_code_without_comments_or_literals(source: str) -> str:
    token = re.compile(
        r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.DOTALL,
    )
    return token.sub(lambda match: "\n" * match.group(0).count("\n"), source)


def validate_typed_record_patch(patch: str) -> None:
    sections = patch_sections(patch)
    expected_targets = {
        "include/libimobiledevice/ostrace.h",
        "src/ostrace.c",
    }
    if set(sections) != expected_targets:
        raise AssertionError("typed-record patch must modify exactly ostrace.h and ostrace.c")

    header_added = c_code_without_comments_or_literals(
        sections["include/libimobiledevice/ostrace.h"]["added"]
    )
    header_effective = c_code_without_comments_or_literals(
        sections["include/libimobiledevice/ostrace.h"]["effective"]
    )
    source_added = c_code_without_comments_or_literals(sections["src/ostrace.c"]["added"])
    source_effective = c_code_without_comments_or_literals(
        sections["src/ostrace.c"]["effective"]
    )

    callback = re.compile(
        r"typedef\s+void\s*\(\s*\*\s*ostrace_record_cb_t\s*\)\s*\(\s*"
        r"uint8_t\s+record_type\s*,\s*const\s+void\s*\*\s*buf\s*,\s*"
        r"size_t\s+len\s*,\s*void\s*\*\s*user_data\s*\)\s*;"
    )
    if callback.search(header_added) is None:
        raise AssertionError(
            "typed-record patch must add ostrace_record_cb_t with the outer uint8_t record type"
        )

    symbol = "ostrace_start_activity_with_record_type_and_error"
    declaration = re.compile(
        rf"LIBIMOBILEDEVICE_API\s+ostrace_error_t\s+{symbol}\s*\(\s*"
        r"ostrace_client_t\s+client\s*,\s*plist_t\s+options\s*,\s*"
        r"ostrace_record_cb_t\s+callback\s*,\s*ostrace_terminal_cb_t\s+"
        r"terminal_callback\s*,\s*void\s*\*\s*user_data\s*\)\s*;"
    )
    if declaration.search(header_added) is None:
        raise AssertionError(
            "typed-record patch must export ostrace_start_activity_with_record_type_and_error"
        )
    definition = re.compile(
        rf"ostrace_error_t\s+{symbol}\s*\([^)]*ostrace_record_cb_t\s+callback[^)]*"
        r"ostrace_terminal_cb_t\s+terminal_callback[^)]*\)"
    )
    if definition.search(source_added) is None:
        raise AssertionError("typed-record patch must define the additive typed-record symbol")

    for wrapper in ("ostrace_start_activity", "ostrace_start_activity_with_error"):
        if re.search(rf"ostrace_error_t\s+{wrapper}\s*\(", header_effective) is None:
            raise AssertionError(f"typed-record patch must preserve old declaration {wrapper}")
        if re.search(rf"ostrace_error_t\s+{wrapper}\s*\(", source_effective) is None:
            raise AssertionError(f"typed-record patch must preserve old wrapper {wrapper}")

    failed_thread_cleanup = re.search(
        r"if\s*\(\s*thread_new\([^)]*\)\s*==\s*0\s*\)\s*\{.*?\}"
        r"\s*else\s*\{\s*free\(\s*oswt\s*\)\s*;\s*\}",
        source_effective,
        re.DOTALL,
    )
    if failed_thread_cleanup is None:
        raise AssertionError(
            "typed-record activity startup must free worker state when thread creation fails"
        )

    dispatch = re.search(
        r"(?:record_callback|record_cbfunc)\s*\(\s*(?:oswt->)?(?:msgtype|record_type)\s*,"
        r"\s*buf\s*,\s*received\s*,\s*(?:oswt->)?user_data\s*\)\s*;",
        source_effective,
    )
    if dispatch is None:
        raise AssertionError(
            "typed-record worker dispatch must pass the outer record type with the borrowed payload"
        )
    empty_record_reset = re.search(
        r"void\s*\*\s*buf\s*=\s*NULL\s*;\s*received\s*=\s*0\s*;\s*"
        r"if\s*\(\s*rlen\s*>\s*0\s*\)",
        source_effective,
    )
    if empty_record_reset is None or empty_record_reset.end() > dispatch.start():
        raise AssertionError(
            "typed-record worker must report zero borrowed bytes for an empty control record"
        )
    legacy_dispatch = re.search(
        r"(?:oswt->)?cbfunc\s*\(\s*buf\s*,\s*received\s*,\s*(?:oswt->)?user_data\s*\)",
        source_effective,
    )
    if legacy_dispatch is None:
        raise AssertionError("typed-record patch must preserve legacy callback dispatch")
    release = source_effective.find("free(buf);", dispatch.end())
    if release < 0:
        raise AssertionError(
            "typed-record callback payload must remain borrowed until callback return"
        )
    if re.search(r"rlen\s*==\s*0\s*\|\|\s*rlen\s*>", source_effective):
        raise AssertionError("typed-record patch must permit empty type-1 control records")


TYPED_RECORD_STRUCTURAL_FIXTURE = """\
From: fixture <fixture@example.invalid>
Subject: [PATCH] ostrace: preserve relay record type

diff --git a/include/libimobiledevice/ostrace.h b/include/libimobiledevice/ostrace.h
--- a/include/libimobiledevice/ostrace.h
+++ b/include/libimobiledevice/ostrace.h
@@ -1,3 +1,5 @@
 typedef void (*ostrace_activity_cb_t)(const void* buf, size_t len, void *user_data);
+typedef void (*ostrace_record_cb_t)(uint8_t record_type, const void* buf, size_t len, void *user_data);
+LIBIMOBILEDEVICE_API ostrace_error_t ostrace_start_activity_with_record_type_and_error(ostrace_client_t client, plist_t options, ostrace_record_cb_t callback, ostrace_terminal_cb_t terminal_callback, void* user_data);
 LIBIMOBILEDEVICE_API ostrace_error_t ostrace_start_activity(ostrace_client_t client, plist_t options, ostrace_activity_cb_t callback, void* user_data);
 LIBIMOBILEDEVICE_API ostrace_error_t ostrace_start_activity_with_error(ostrace_client_t client, plist_t options, ostrace_activity_cb_t callback, ostrace_terminal_cb_t terminal_callback, void* user_data);
diff --git a/src/ostrace.c b/src/ostrace.c
--- a/src/ostrace.c
+++ b/src/ostrace.c
@@ -1,7 +1,12 @@
 ostrace_error_t ostrace_start_activity(ostrace_client_t client, plist_t options, ostrace_activity_cb_t callback, void* user_data)
 ostrace_error_t ostrace_start_activity_with_error(ostrace_client_t client, plist_t options, ostrace_activity_cb_t callback, ostrace_terminal_cb_t terminal_callback, void* user_data)
+ostrace_error_t ostrace_start_activity_with_record_type_and_error(ostrace_client_t client, plist_t options, ostrace_record_cb_t callback, ostrace_terminal_cb_t terminal_callback, void* user_data)
+oswt->record_cbfunc = callback;
+oswt->terminal_callback = terminal_callback;
+if (thread_new(&client->worker, ostrace_worker, oswt) == 0) {
+    res = OSTRACE_E_SUCCESS;
+} else {
+    free(oswt);
+}
+void* buf = NULL;
+received = 0;
+if (rlen > 0) {
+    buf = malloc(rlen);
+}
+if (oswt->record_cbfunc) {
+    oswt->record_cbfunc(msgtype, buf, received, oswt->user_data);
+} else {
 oswt->cbfunc(buf, received, oswt->user_data);
+}
 free(buf);
 if (rlen > OSTRACE_MAX_LIVE_PACKET_SIZE) return OSTRACE_E_REQUEST_FAILED;
"""


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

    def test_typed_record_patch_validator_accepts_the_required_structure(self):
        validate_typed_record_patch(TYPED_RECORD_STRUCTURAL_FIXTURE)

    def test_typed_record_patch_validator_rejects_worker_state_leak_on_thread_failure(self):
        leaking = TYPED_RECORD_STRUCTURAL_FIXTURE.replace(
            "+} else {\n+    free(oswt);\n+}\n", "+}\n", 1
        )
        with self.assertRaisesRegex(AssertionError, "thread creation fails"):
            validate_typed_record_patch(leaking)

    def test_typed_record_patch_validator_rejects_an_untyped_near_miss(self):
        near_miss = TYPED_RECORD_STRUCTURAL_FIXTURE.replace(
            "uint8_t record_type, const void* buf", "const void* buf"
        ).replace(
            "record_cbfunc(msgtype, buf, received",
            "record_cbfunc(buf, received",
        )
        with self.assertRaisesRegex(AssertionError, "outer uint8_t record type"):
            validate_typed_record_patch(near_miss)

    def test_typed_record_patch_validator_rejects_release_before_callback_return(self):
        near_miss = TYPED_RECORD_STRUCTURAL_FIXTURE.replace(
            "    oswt->record_cbfunc(msgtype, buf, received, oswt->user_data);",
            "    free(buf);\n    oswt->record_cbfunc(msgtype, buf, received, oswt->user_data);",
        ).replace("\n free(buf);", "")
        with self.assertRaisesRegex(AssertionError, "borrowed until callback return"):
            validate_typed_record_patch(near_miss)

    def test_typed_record_patch_validator_rejects_zero_length_control_regression(self):
        near_miss = TYPED_RECORD_STRUCTURAL_FIXTURE.replace(
            "if (rlen > OSTRACE_MAX_LIVE_PACKET_SIZE)",
            "if (rlen == 0 || rlen > OSTRACE_MAX_LIVE_PACKET_SIZE)",
        )
        with self.assertRaisesRegex(AssertionError, "permit empty type-1 control records"):
            validate_typed_record_patch(near_miss)

    def test_typed_record_patch_validator_rejects_stale_length_for_empty_control_record(self):
        near_miss = TYPED_RECORD_STRUCTURAL_FIXTURE.replace("+received = 0;\n", "")
        with self.assertRaisesRegex(AssertionError, "zero borrowed bytes"):
            validate_typed_record_patch(near_miss)

    def test_typed_record_patch_validator_rejects_early_borrowed_buffer_release(self):
        near_miss = TYPED_RECORD_STRUCTURAL_FIXTURE.replace(
            "+if (oswt->record_cbfunc) {",
            " free(buf);\n+if (oswt->record_cbfunc) {",
        ).replace(
            " free(buf);\n if (rlen > OSTRACE_MAX_LIVE_PACKET_SIZE)",
            " if (rlen > OSTRACE_MAX_LIVE_PACKET_SIZE)",
        )
        with self.assertRaisesRegex(AssertionError, "borrowed until callback return"):
            validate_typed_record_patch(near_miss)

    def test_typed_record_patch_validator_ignores_commit_message_and_removed_line_spoofs(self):
        spoof = """\
From: fixture <fixture@example.invalid>
Subject: ostrace_record_cb_t ostrace_start_activity_with_record_type_and_error

The prose says record_cbfunc(msgtype, buf, received, oswt->user_data); free(buf);
diff --git a/include/libimobiledevice/ostrace.h b/include/libimobiledevice/ostrace.h
--- a/include/libimobiledevice/ostrace.h
+++ b/include/libimobiledevice/ostrace.h
@@ -1,2 +1 @@
-typedef void (*ostrace_record_cb_t)(uint8_t record_type, const void* buf, size_t len, void *user_data);
-LIBIMOBILEDEVICE_API ostrace_error_t ostrace_start_activity_with_record_type_and_error(ostrace_client_t client, plist_t options, ostrace_record_cb_t callback, ostrace_terminal_cb_t terminal_callback, void* user_data);
+/* ostrace_record_cb_t(uint8_t record_type) only appears in a comment */
diff --git a/src/ostrace.c b/src/ostrace.c
--- a/src/ostrace.c
+++ b/src/ostrace.c
@@ -1,2 +1 @@
-oswt->record_cbfunc(msgtype, buf, received, oswt->user_data);
-free(buf);
+const char *spoof = "record_cbfunc(msgtype, buf, received, oswt->user_data); free(buf);";
"""
        with self.assertRaisesRegex(AssertionError, "outer uint8_t record type"):
            validate_typed_record_patch(spoof)

    def test_typed_record_patch_validator_fails_closed_on_malformed_diff(self):
        malformed = TYPED_RECORD_STRUCTURAL_FIXTURE.replace(
            "diff --git a/src/ostrace.c b/src/ostrace.c",
            "diff --git a/src/ostrace.c b/src/other.c",
        )
        with self.assertRaisesRegex(AssertionError, "malformed or cross-file"):
            validate_typed_record_patch(malformed)

    def test_typed_record_patch_adds_the_new_abi_without_replacing_old_wrappers(self):
        validate_typed_record_patch(required_text(TYPED_RECORD_PATCH))

    def test_typed_record_patch_is_locked_after_the_terminal_callback_patch(self):
        document = self.lock()
        paths = [
            patch.get("path")
            for patch in document.get("patches", [])
            if patch.get("source_id") == "libimobiledevice"
        ]
        expected_path = f"patches/{TYPED_RECORD_PATCH.name}"
        self.assertIn(expected_path, paths)
        self.assertEqual(paths[-1], expected_path)
        typed = next(patch for patch in document["patches"] if patch.get("path") == expected_path)
        self.assertTrue(typed.get("required"))
        self.assertEqual(typed.get("purpose"), "preserve-ostrace-relay-record-type")
        self.assertEqual(
            typed.get("typed_record_symbol"),
            "ostrace_start_activity_with_record_type_and_error",
        )
        self.assertEqual(typed.get("typed_record_callback"), "ostrace_record_cb_t")
        self.assertEqual(typed.get("relay_record_types"), {"control": 1, "activity": 2})
        self.assertEqual(
            typed.get("preserved_wrappers"),
            ["ostrace_start_activity", "ostrace_start_activity_with_error"],
        )
        self.assertEqual(
            typed.get("callback_buffer_ownership"),
            "borrowed-until-callback-returns",
        )
        self.assertRegex(typed.get("sha256", ""), HEX64)
        self.assertEqual(hashlib.sha256(TYPED_RECORD_PATCH.read_bytes()).hexdigest(), typed["sha256"])

    def test_build_applies_the_complete_runtime_patch_series_in_order(self):
        call = package_call(THIRD_PARTY_CMAKE.read_text(encoding="utf-8"))
        names = [
            PATCH.name,
            INTERRUPT_PATCH.name,
            PASSIVE_HANDSHAKE_PATCH.name,
            SYSLOG_TERMINAL_PATCH.name,
            TYPED_RECORD_PATCH.name,
        ]
        for name in names:
            self.assertIn(name, call)
        self.assertEqual(
            sorted(call.index(name) for name in names),
            [call.index(name) for name in names],
        )


if __name__ == "__main__":
    unittest.main()
