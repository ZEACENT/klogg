from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import pathlib
import re
import shlex
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).parents[2]
ROOT_CMAKE = ROOT / "CMakeLists.txt"
LOCK = ROOT / "3rdparty" / "libimobiledevice" / "libimobiledevice.lock.json"
PATCH = (
    ROOT
    / "3rdparty"
    / "libimobiledevice"
    / "patches"
    / "0001-fix-ostrace-live-packet-leak.patch"
)
THIRD_PARTY_CMAKE = ROOT / "3rdparty" / "CMakeLists.txt"
PREFETCH_CMAKE = ROOT / "cmake" / "prefetch_cpm" / "CMakeLists.txt"
MAC_PACKAGE_ACTION = ROOT / ".github" / "actions" / "agent-package-mac" / "action.yml"
APP_CMAKE = ROOT / "src" / "app" / "CMakeLists.txt"
IOS_LIVE_SERVICES = ROOT / "src" / "ui" / "src" / "iosliveservices.cpp"
IOS_NATIVE_STREAM = ROOT / "src" / "livecapture" / "src" / "iosnativestream.cpp"
CI_BUILD_WORKFLOW = ROOT / ".github" / "workflows" / "ci-build.yml"
BUILD_SCRIPT = ROOT / "scripts" / "build_ios_native_stack.py"
VERIFY_SCRIPT = ROOT / "scripts" / "verify_ios_native_stack.py"
LEGAL_SCRIPT = ROOT / "scripts" / "build_ios_native_legal_assets.py"
SUPERBUILD = ROOT / "packaging" / "ios-native" / "superbuild" / "CMakeLists.txt"

_BUILD_SPEC = importlib.util.spec_from_file_location("build_ios_native_stack", BUILD_SCRIPT)
assert _BUILD_SPEC is not None and _BUILD_SPEC.loader is not None
BUILD_MODULE = importlib.util.module_from_spec(_BUILD_SPEC)
_BUILD_SPEC.loader.exec_module(BUILD_MODULE)
_VERIFY_SPEC = importlib.util.spec_from_file_location("verify_ios_native_stack", VERIFY_SCRIPT)
assert _VERIFY_SPEC is not None and _VERIFY_SPEC.loader is not None
VERIFY_MODULE = importlib.util.module_from_spec(_VERIFY_SPEC)
_VERIFY_SPEC.loader.exec_module(VERIFY_MODULE)

EXPECTED_SOURCES = {
    "openssl": {
        "version": "3.5.7",
        "commit": "8cf17aaeb4599f8af87fefd810b5b5fee90fe69e",
        "archive_sha256": "a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8",
        "archive_url": "https://www.openssl.org/source/openssl-3.5.7.tar.gz",
    },
    "curl": {
        "version": "8.21.0",
        "commit": "68720b4837284335b2d63cb358f8f6ce65f5bc55",
        "archive_sha256": "aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6",
        "archive_url": "https://curl.se/download/curl-8.21.0.tar.xz",
    },
    "libplist": {
        "version": "2.7.0",
        "commit": "cf5897a71ea412ea2aeb1e2f6b5ea74d4fabfd8c",
    },
    "libtatsu": {
        "version": "1.0.5",
        "commit": "42329cb756682535c7c0f087987b78d1dd5b16c8",
        "archive_sha256": "83f25dec2d4a3982d6d0b9cb0d9a7c891bc00f3c232b29fb518ad43b16c14bb3",
    },
    "libimobiledevice-glue": {
        "version": "1.3.2",
        "commit": "aef2bf0f5bfe961ad83d224166462d87b1df2b00",
    },
    "libusbmuxd": {
        "version": "2.1.1",
        "commit": "adf9c22b9010490e4b55eaeb14731991db1c172c",
    },
    "libimobiledevice": {
        "version": "1.4.0",
        "commit": "149f7623c672c1fa73122c7119a12bfc0012f2ac",
    },
}
EXPECTED_BUILD_ORDER = list(EXPECTED_SOURCES)
EXPECTED_LIBIMOBILEDEVICE_PATCH_CHAIN = [
    "patches/0001-fix-ostrace-live-packet-leak.patch",
    "patches/0002-interrupt-live-capture-receive.patch",
    "patches/0003-passive-lockdown-handshake.patch",
    "patches/0004-syslog-terminal-callback.patch",
    "patches/0005-ostrace-record-type-callback.patch",
]
EXPECTED_LIBIMOBILEDEVICE_FINAL_TREE_SHA256 = (
    "a5acf45cb73b96ded80d8944f8c2ee59e74a73181fe41caee2f81a30d9d8587d"
)
EXPECTED_THIN_ARTIFACTS = {
    "x86_64": "15.0",
    "arm64": "14.0",
}
UPSTREAM_LEAK_FIX_COMMIT = "5ca453f9e3950b1f24b51e4cdf255236e34254c4"
HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")


def required_text(path: pathlib.Path) -> str:
    if not path.is_file():
        raise AssertionError(f"required iOS native release contract file is missing: {path}")
    return path.read_text(encoding="utf-8")


def required_json(path: pathlib.Path) -> dict:
    try:
        value = json.loads(required_text(path))
    except json.JSONDecodeError as error:
        raise AssertionError(f"{path} is not valid JSON: {error}") from error
    if not isinstance(value, dict):
        raise AssertionError(f"{path} must contain a JSON object")
    return value


def cmake_calls(source: str, command: str) -> list[str]:
    calls = []
    lowered = source.lower()
    needle = f"{command.lower()}("
    position = 0
    while True:
        start = lowered.find(needle, position)
        if start < 0:
            return calls
        body_start = start + len(needle)
        depth = 1
        quote = None
        escaped = False
        cursor = body_start
        while cursor < len(source) and depth:
            character = source[cursor]
            if quote is not None:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
                    quote = None
            elif character in ('"', "'"):
                quote = character
            elif character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
            cursor += 1
        if depth:
            raise AssertionError(f"unterminated {command} call")
        calls.append(source[body_start : cursor - 1])
        position = cursor


def cmake_tokens(body: str) -> list[str]:
    uncommented = "\n".join(line.split("#", 1)[0] for line in body.splitlines())
    return shlex.split(uncommented, posix=True)


def cpm_packages(path: pathlib.Path) -> dict[str, list[str]]:
    packages = {}
    for body in cmake_calls(required_text(path), "cpmaddpackage"):
        tokens = cmake_tokens(body)
        try:
            name = tokens[tokens.index("NAME") + 1]
        except (ValueError, IndexError):
            continue
        packages[name.lower()] = tokens
    return packages


class IosNativeReleaseContractTest(unittest.TestCase):
    def lock(self) -> dict:
        return required_json(LOCK)

    def sources(self) -> dict[str, dict]:
        sources = self.lock().get("sources")
        self.assertIsInstance(sources, list, "lock.sources must be an array")
        by_id = {source.get("id"): source for source in sources if isinstance(source, dict)}
        self.assertEqual(
            sorted(by_id),
            sorted(EXPECTED_BUILD_ORDER),
            "the lock must describe the complete source-built native dependency closure",
        )
        return by_id

    def test_catalog_dispatch_retains_its_executor_through_service_shutdown(self):
        source = required_text(IOS_LIVE_SERVICES)
        self.assertIn(
            "std::shared_ptr<BoundedSerialExecutor> catalogExecutor_", source
        )
        self.assertIn("[ executor = catalogExecutor_ ]", source)
        self.assertNotIn("[ executor = catalogExecutor_.get() ]", source)
        self.assertIn("catalogExecutor_->shutdownAsync();", source)

    def test_stop_acknowledgement_requires_cleanup_dispatch(self):
        source = required_text(IOS_NATIVE_STREAM)
        stop = re.search(
            r"void IosNativeStreamWorker::stop\([^)]*\) noexcept\s*\{(.*?)\n\}",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(stop)
        body = stop.group(1)
        self.assertIn("state->scheduleCleanup()", body)
        self.assertNotIn("publishStopped", body)

    def test_legal_source_archive_rejects_traversal_member_names(self):
        code = """
import io
import tarfile
from build_ios_native_legal_assets import LegalAssetError, add_bytes
for name in ("../victim", "..\\\\victim", "/tmp/victim", "patches/../../victim"):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w") as archive:
        try:
            add_bytes(archive, name, b"data")
        except LegalAssetError:
            pass
        else:
            raise SystemExit("accepted " + name)
"""
        result = subprocess.run(
            [sys.executable, "-c", code],
            cwd=ROOT / "scripts",
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_lock_schema_is_extended_for_release_and_artifact_evidence(self):
        self.assertEqual(type(self.lock().get("schema_version")), int)
        self.assertEqual(self.lock().get("schema_version"), 2)

    def test_verifier_rejects_boolean_lock_build_and_package_schema_versions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            lock_path = root / "lock.json"
            receipt_path = root / "build.json"
            lock_path.write_text('{"schema_version": true}\n', encoding="utf-8")
            receipt_path.write_text("{}\n", encoding="utf-8")
            argv = [
                str(VERIFY_SCRIPT),
                "--lock",
                str(lock_path),
                "--stack-root",
                str(root),
                "--architecture",
                "arm64",
                "--receipt",
                str(receipt_path),
            ]
            with (
                mock.patch.object(sys, "argv", argv),
                mock.patch.object(VERIFY_MODULE, "validate_build_receipt"),
                mock.patch.object(VERIFY_MODULE, "verify_stack", return_value=[]),
            ):
                self.assertNotEqual(VERIFY_MODULE.main(), 0)

        valid_build = {
            "schema_version": 1,
            "receipt_kind": "ios-native-build",
            "lock_sha256": VERIFY_MODULE.sha256(LOCK),
            "architecture": "arm64",
            "deployment_target": "14.0",
            "native_qualified": True,
            "qualification": "native",
        }
        invalid_build = dict(valid_build, schema_version=True)
        with self.assertRaisesRegex(VERIFY_MODULE.VerificationError, "schema"):
            VERIFY_MODULE.validate_build_receipt(
                self.lock(), LOCK, invalid_build, "arm64"
            )

        invalid_package = {
            "schema_version": True,
            "receipt_kind": "ios-native-package",
            "lock_sha256": "a" * 64,
            "architecture": "arm64",
            "build_receipt_sha256": "b" * 64,
            "dylibs": [],
        }
        with self.assertRaisesRegex(VERIFY_MODULE.VerificationError, "schema"):
            VERIFY_MODULE.verify_package_receipt(
                invalid_package,
                "a" * 64,
                "arm64",
                "b" * 64,
                [],
                {},
            )

    def test_verifier_rejects_boolean_source_and_legal_receipt_schema_versions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            lock_path = root / "lock.json"
            lock_path.write_text('{"schema_version": 2}\n', encoding="utf-8")
            lock_hash = VERIFY_MODULE.sha256(lock_path)

            def write_asset(name: str, content: bytes) -> pathlib.Path:
                path = root / name
                path.write_bytes(content)
                return path

            source_offer = write_asset("source-offer.txt", b"source offer\n")
            replacement = write_asset("replacement.txt", b"replacement\n")
            notice = write_asset("NOTICE.txt", b"notice\n")
            license_file = write_asset("COPYING", b"license\n")
            sbom_path = root / "sbom.json"
            sbom_path.write_text('{"spdxVersion": "SPDX-2.3"}\n', encoding="utf-8")

            def bound(path: pathlib.Path) -> dict:
                return {"path": path.name, "sha256": VERIFY_MODULE.sha256(path)}

            source_document = {
                "schema_version": 1,
                "receipt_kind": "component-source-set",
                "component": "ios-native",
                "lock_sha256": lock_hash,
                "source_identity": {
                    "manifest_or_closure_sha256": "a" * 64,
                    "tree_hash_algorithm": "sha256",
                    "final_tree_sha256": "b" * 64,
                },
                "patch_chain_sha256": "c" * 64,
                "package_support_assets": [
                    {
                        "kind": "source-offer",
                        "file_name": source_offer.name,
                        "sha256": VERIFY_MODULE.sha256(source_offer),
                    },
                    {
                        "kind": "replacement-guide",
                        "file_name": replacement.name,
                        "sha256": VERIFY_MODULE.sha256(replacement),
                    },
                    {
                        "kind": "notices",
                        "file_name": notice.name,
                        "sha256": VERIFY_MODULE.sha256(notice),
                    },
                    {
                        "kind": "license:fixture",
                        "file_name": license_file.name,
                        "sha256": VERIFY_MODULE.sha256(license_file),
                    },
                ],
                "distribution": {
                    "package_required": False,
                    "release_required": True,
                },
                "archive": {"file_name": "source.tar.gz", "sha256": "d" * 64},
            }
            legal_document = {
                "schema_version": 1,
                "receipt_kind": "legal",
                "lock_sha256": lock_hash,
                "architecture": "arm64",
                "notice": bound(notice),
                "replacement_guide": bound(replacement),
                "license_files": [bound(license_file)],
                "sbom": bound(sbom_path),
            }
            source_path = root / "source.json"
            legal_path = root / "legal.json"

            for label in ("source", "legal"):
                with self.subTest(label=label):
                    source_document["schema_version"] = (
                        True if label == "source" else 1
                    )
                    legal_document["schema_version"] = (
                        True if label == "legal" else 1
                    )
                    source_path.write_text(
                        json.dumps(source_document), encoding="utf-8"
                    )
                    legal_path.write_text(
                        json.dumps(legal_document), encoding="utf-8"
                    )
                    with self.assertRaisesRegex(
                        VERIFY_MODULE.VerificationError, "schema"
                    ):
                        VERIFY_MODULE.verify_legal_assets(
                            lock_path,
                            "arm64",
                            source_path,
                            legal_path,
                            sbom_path,
                            asset_scope="package",
                            source_assets_root=root,
                        )

            source_document["schema_version"] = 1
            legal_document["schema_version"] = 1
            source_document["distribution"] = {
                "package_required": 0,
                "release_required": 1,
            }
            source_path.write_text(json.dumps(source_document), encoding="utf-8")
            legal_path.write_text(json.dumps(legal_document), encoding="utf-8")
            with self.assertRaisesRegex(VERIFY_MODULE.VerificationError, "distribution"):
                VERIFY_MODULE.verify_legal_assets(
                    lock_path,
                    "arm64",
                    source_path,
                    legal_path,
                    sbom_path,
                    asset_scope="package",
                    source_assets_root=root,
                )

    def test_complete_native_stack_has_exact_versions_and_immutable_sources(self):
        sources = self.sources()
        for dependency, expected in EXPECTED_SOURCES.items():
            source = sources[dependency]
            self.assertEqual(source.get("version"), expected["version"], dependency)
            self.assertEqual(source.get("commit"), expected["commit"], dependency)
            if "archive_sha256" in expected:
                self.assertEqual(
                    source.get("archive_sha256"), expected["archive_sha256"], dependency
                )
            self.assertRegex(str(source.get("commit", "")), HEX40, dependency)
            self.assertRegex(str(source.get("archive_sha256", "")), HEX64, dependency)
            self.assertTrue(str(source.get("archive_url", "")).startswith("https://"))
            if dependency == "openssl":
                self.assertEqual(source.get("archive_url"), expected["archive_url"])
                self.assertEqual(source.get("release_tag"), "openssl-3.5.7")
                self.assertIn(source["commit"], str(source.get("commit_url", "")))
                self.assertEqual(source.get("support_track"), "LTS")
            elif dependency == "curl":
                self.assertEqual(source.get("archive_url"), expected["archive_url"])
                self.assertEqual(source.get("release_tag"), "curl-8_21_0")
                self.assertIn(source["commit"], str(source.get("commit_url", "")))
            else:
                self.assertIn(
                    source["commit"],
                    source["archive_url"],
                    f"{dependency} archive URL must identify the immutable locked revision",
                )
            self.assertTrue(source.get("archive_file"), dependency)
            self.assertTrue(source.get("source_built"), dependency)
            self.assertFalse(source.get("allow_system_substitution", True), dependency)

        self.assertTrue(
            sources["openssl"].get("selection_rationale"),
            "the supported OpenSSL 3.5.7 LTS source selection must record its rationale",
        )

    def test_build_and_prefetch_manifests_cover_the_same_locked_stack(self):
        sources = self.sources()
        build_packages = cpm_packages(THIRD_PARTY_CMAKE)
        prefetch_packages = cpm_packages(PREFETCH_CMAKE)

        for dependency, source in sources.items():
            key = dependency.lower()
            self.assertIn(key, build_packages, f"build manifest omits {dependency}")
            self.assertIn(key, prefetch_packages, f"prefetch manifest omits {dependency}")
            for package_set, label in ((build_packages, "build"), (prefetch_packages, "prefetch")):
                tokens = package_set[key]
                self.assertIn("GIT_TAG", tokens, f"{label} {dependency} is not commit pinned")
                self.assertEqual(tokens[tokens.index("GIT_TAG") + 1], source["commit"])

        build_script = required_text(BUILD_SCRIPT)
        for dependency in EXPECTED_BUILD_ORDER:
            self.assertIn(dependency, build_script)
        self.assertIn("MACOSX_DEPLOYMENT_TARGET", build_script)
        self.assertIn("x86_64", build_script)
        self.assertIn("arm64", build_script)
        self.assertNotRegex(build_script, r"\bbrew\s+(install|link|--prefix)\b")
        self.assertNotRegex(build_script, r"\bpkg-config\b.*(?:/opt/homebrew|/usr/local)")

    def test_catalog_stays_non_handshaking_and_stream_preflights_pair_record(self):
        adapter = required_text(ROOT / "src/livecapture/src/iosnativeadapter.cpp")
        api = required_text(ROOT / "src/livecapture/include/iosnativeapi.h")
        catalog = required_text(ROOT / "src/livecapture/src/iosdevicecatalog.cpp")
        stream = required_text(ROOT / "src/livecapture/src/iosnativestream.cpp")

        for source in (adapter, api, catalog, stream):
            self.assertNotIn("lockdownd_client_new_with_handshake", source)
            self.assertNotIn("lockdownClientNewWithHandshake", source)
        self.assertIn('"lockdownd_client_new"', adapter)
        self.assertIn('"lockdownd_client_new_with_existing_pair"', adapter)
        self.assertIn("lockdownClientNew", api)

        self.assertIn('"usbmuxd_read_pair_record"', adapter)
        self.assertIn("readPairRecord", api)
        self.assertIn("lockdownClientNewWithExistingPair", api)
        self.assertLess(
            stream.index("api.readPairRecord("),
            stream.index("api.lockdownClientNewWithExistingPair("),
            "the worker must preflight before the adapter revalidates and starts a passive session",
        )
        self.assertNotIn("api.pairDevice(", stream)
        self.assertNotIn("api.unpairDevice(", stream)

    def test_release_policy_forbids_runtime_host_and_apple_private_fallbacks(self):
        policy = self.lock().get("release_policy")
        self.assertIsInstance(policy, dict)
        required_false = (
            "allow_runtime_download",
            "allow_homebrew_runtime",
            "allow_system_dependency_substitution",
            "allow_usbmuxd_daemon_dependency",
            "allow_mobiledevice_framework",
            "allow_floating_revisions",
        )
        for field in required_false:
            self.assertIs(policy.get(field), False, field)
        self.assertIs(policy.get("source_build_required"), True)
        self.assertIs(policy.get("application_configure_disconnected"), True)
        self.assertIs(policy.get("fail_closed_on_missing_native_stack"), True)
        self.assertEqual(policy.get("required_architectures"), ["x86_64", "arm64"])

        combined = "\n".join(
            (
                required_text(THIRD_PARTY_CMAKE),
                required_text(BUILD_SCRIPT),
                required_text(MAC_PACKAGE_ACTION),
            )
        )
        self.assertNotIn("MobileDevice.framework", combined)
        self.assertNotRegex(combined, r"\b(?:brew|port)\s+install\b")
        self.assertNotRegex(combined, r"(?:launchctl|usbmuxd\s+(?:--daemon|-f|-v))")

    def test_leak_patch_is_mandatory_and_bound_to_source_clean_and_patched_trees(self):
        document = self.lock()
        patches = document.get("patches")
        self.assertIsInstance(patches, list)
        matching = [
            item
            for item in patches
            if item.get("upstream_commit") == UPSTREAM_LEAK_FIX_COMMIT
        ]
        self.assertEqual(len(matching), 1, "the 5ca453f9 leak fix must be mandatory")
        patch = matching[0]
        self.assertIs(patch.get("required"), True)
        self.assertEqual(patch.get("source_id"), "libimobiledevice")
        self.assertEqual(
            patch.get("path"), "patches/0001-fix-ostrace-live-packet-leak.patch"
        )
        for field in ("source_archive_sha256", "clean_tree_sha256", "sha256", "patched_tree_sha256"):
            self.assertRegex(str(patch.get(field, "")), HEX64, field)
        self.assertEqual(
            patch.get("source_archive_sha256"),
            self.sources()["libimobiledevice"]["archive_sha256"],
        )
        self.assertEqual(hashlib.sha256(PATCH.read_bytes()).hexdigest(), patch["sha256"])
        self.assertNotEqual(patch["clean_tree_sha256"], patch["patched_tree_sha256"])

        series = [item for item in patches if item.get("source_id") == "libimobiledevice"]
        self.assertGreaterEqual(len(series), 2)
        for previous, current in zip(series, series[1:]):
            self.assertEqual(
                previous["patched_tree_sha256"],
                current["clean_tree_sha256"],
                "each native patch must be based on the exact preceding intermediate tree",
            )
        final_patch = series[-1]

        build = required_text(THIRD_PARTY_CMAKE)
        build_verification = build + "\n" + required_text(BUILD_SCRIPT)
        for item in series:
            self.assertIn(pathlib.Path(item["path"]).name, build_verification)
        self.assertIn(
            patch["clean_tree_sha256"],
            build_verification,
            "build must verify the locked original clean tree",
        )
        self.assertIn(
            final_patch["patched_tree_sha256"],
            build_verification,
            "build must verify the final tree produced by the complete patch series",
        )

    def test_release_source_receipts_require_the_complete_ordered_libimobiledevice_patch_chain(self):
        patches = self.lock().get("patches")
        self.assertIsInstance(patches, list)
        actual = [
            patch.get("path")
            for patch in patches
            if isinstance(patch, dict) and patch.get("source_id") == "libimobiledevice"
        ]
        self.assertEqual(
            actual,
            EXPECTED_LIBIMOBILEDEVICE_PATCH_CHAIN,
            "release/source receipts must bind the typed-record ABI patch after the existing runtime series",
        )
        self.assertEqual(
            patches[-1].get("patched_tree_sha256"),
            EXPECTED_LIBIMOBILEDEVICE_FINAL_TREE_SHA256,
            "the lock must name the tree produced by applying the complete patch chain to the pinned archive",
        )

        build_inputs = "\n".join(
            (required_text(THIRD_PARTY_CMAKE), required_text(BUILD_SCRIPT))
        )
        for patch_path in EXPECTED_LIBIMOBILEDEVICE_PATCH_CHAIN:
            self.assertIn(pathlib.Path(patch_path).name, build_inputs)

    def test_dynamic_ostrace_error_callback_restores_the_application_context(self):
        adapter = required_text(ROOT / "src/livecapture/src/iosnativeadapter.cpp")
        self.assertIn("void relayOsTraceError(", adapter)
        self.assertIn(
            "relay->errorCallback( error, relay->context )",
            adapter,
        )
        self.assertRegex(
            adapter,
            r"symbols\.osTraceStartWithRecordType\(\s*client, nullptr, "
            r"&relayOsTraceRecord, &relayOsTraceError, relayContext\s*\)",
        )
        self.assertNotRegex(
            adapter,
            r"symbols\.osTraceStartWithRecordType\(\s*client, nullptr, "
            r"&relayOsTraceRecord, errorCallback, relayContext\s*\)",
        )

    def test_pinned_typed_ostrace_symbol_is_required_by_the_artifact_verifier(self):
        symbol = "ostrace_start_activity_with_record_type_and_error"
        contract = self.lock().get("artifact_contract")
        self.assertIsInstance(contract, dict)
        required = contract.get("required_exported_symbols")
        self.assertIsInstance(required, dict)
        self.assertEqual(
            required.get("libimobiledevice-1.0.dylib"),
            [
                "lockdownd_client_new_with_existing_pair",
                symbol,
                "syslog_relay_start_capture_raw_with_error",
            ],
        )

        verifier = required_text(VERIFY_SCRIPT)
        for token in ("nm", "required_exported_symbols", "missing required exported symbols"):
            self.assertIn(token, verifier)

    def test_exported_symbol_verification_normalizes_macho_names_and_fails_closed(self):
        dylib = pathlib.Path("libimobiledevice-1.0.dylib")
        with mock.patch.object(
            VERIFY_MODULE,
            "run",
            return_value=(
                "0000000000001000 T _ostrace_start_activity_with_record_type_and_error\n"
                "0000000000002000 T _lockdownd_client_new_with_existing_pair\n"
            ),
        ):
            self.assertEqual(
                VERIFY_MODULE.exported_symbols(dylib),
                {
                    "ostrace_start_activity_with_record_type_and_error",
                    "lockdownd_client_new_with_existing_pair",
                },
            )
            VERIFY_MODULE.verify_required_exported_symbols(
                dylib, ["ostrace_start_activity_with_record_type_and_error"]
            )
            with self.assertRaisesRegex(
                VERIFY_MODULE.VerificationError, "missing required exported symbols"
            ):
                VERIFY_MODULE.verify_required_exported_symbols(
                    dylib, ["syslog_relay_start_capture_raw_with_error"]
                )

    def test_exported_symbol_lock_contract_rejects_duplicates_and_unknown_dylibs(self):
        with self.assertRaises(VERIFY_MODULE.VerificationError):
            VERIFY_MODULE.locked_required_exported_symbols(
                {"required_exported_symbols": {"other.dylib": ["symbol"]}},
                ["libimobiledevice-1.0.dylib"],
            )
        with self.assertRaises(VERIFY_MODULE.VerificationError):
            VERIFY_MODULE.locked_required_exported_symbols(
                {
                    "required_exported_symbols": {
                        "libimobiledevice-1.0.dylib": ["symbol", "symbol"]
                    }
                },
                ["libimobiledevice-1.0.dylib"],
            )

    def test_builder_alias_layout_is_verified_and_alias_symbols_bind_to_the_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            prefix = root / "prefix"
            (prefix / "include").mkdir(parents=True)
            prefix_lib = prefix / "lib"
            prefix_lib.mkdir()
            physical = prefix_lib / "librequired.1.dylib"
            physical.write_bytes(b"required dylib fixture")
            try:
                (prefix_lib / "librequired.dylib").symlink_to(physical.name)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"filesystem symlinks are unavailable: {error}")

            stack_root = root / "artifact"
            identities = BUILD_MODULE.stage_artifact(prefix, stack_root, [physical])
            staged_physical = stack_root / "lib" / physical.name
            staged_alias = stack_root / "lib" / "librequired.dylib"
            self.assertTrue(staged_alias.is_symlink())
            self.assertEqual(staged_alias.resolve(), staged_physical.resolve())

            contract = {
                "thin_artifacts": {"arm64": {"deployment_target": "14.0"}},
                "allowed_dylib_rpaths": ["@loader_path"],
                "allowed_system_dependencies": ["/usr/lib/libSystem.B.dylib"],
                "required_dylibs": [staged_alias.name],
                "required_exported_symbols": {
                    staged_alias.name: ["required_symbol"]
                },
                "forbidden_dynamic_references": [],
            }
            with (
                mock.patch.object(
                    VERIFY_MODULE,
                    "install_name",
                    side_effect=lambda path: f"@rpath/{path.name}",
                ),
                mock.patch.object(
                    VERIFY_MODULE, "rpaths", return_value=["@loader_path"]
                ),
                mock.patch.object(
                    VERIFY_MODULE, "architectures", return_value=["arm64"]
                ),
                mock.patch.object(
                    VERIFY_MODULE, "deployment_target", return_value="14.0"
                ),
                mock.patch.object(VERIFY_MODULE, "dependencies", return_value=[]),
                mock.patch.object(
                    VERIFY_MODULE,
                    "exported_symbols",
                    return_value={"required_symbol"},
                ) as exported_symbols,
            ):
                evidence = VERIFY_MODULE.verify_stack(
                    {"artifact_contract": contract},
                    stack_root,
                    "arm64",
                    {"dylibs": identities},
                )

            self.assertEqual([item["name"] for item in evidence], [physical.name])
            exported_symbols.assert_called_once_with(staged_physical)

    def test_verifier_rejects_regular_dylibs_outside_the_locked_physical_closure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            prefix = root / "prefix"
            (prefix / "include").mkdir(parents=True)
            prefix_lib = prefix / "lib"
            prefix_lib.mkdir()
            required = prefix_lib / "librequired.1.dylib"
            extra = prefix_lib / "libextra.1.dylib"
            required.write_bytes(b"required dylib fixture")
            extra.write_bytes(b"extra dylib fixture")
            try:
                (prefix_lib / "librequired.dylib").symlink_to(required.name)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"filesystem symlinks are unavailable: {error}")

            stack_root = root / "artifact"
            identities = BUILD_MODULE.stage_artifact(
                prefix, stack_root, [required, extra]
            )
            contract = {
                "thin_artifacts": {"arm64": {"deployment_target": "14.0"}},
                "allowed_dylib_rpaths": ["@loader_path"],
                "allowed_system_dependencies": ["/usr/lib/libSystem.B.dylib"],
                "required_dylibs": ["librequired.dylib"],
                "required_exported_symbols": {
                    "librequired.dylib": ["required_symbol"]
                },
                "forbidden_dynamic_references": [],
            }
            with (
                mock.patch.object(
                    VERIFY_MODULE,
                    "install_name",
                    side_effect=lambda path: f"@rpath/{path.name}",
                ),
                mock.patch.object(
                    VERIFY_MODULE, "rpaths", return_value=["@loader_path"]
                ),
                mock.patch.object(
                    VERIFY_MODULE, "architectures", return_value=["arm64"]
                ),
                mock.patch.object(
                    VERIFY_MODULE, "deployment_target", return_value="14.0"
                ),
                mock.patch.object(VERIFY_MODULE, "dependencies", return_value=[]),
                mock.patch.object(
                    VERIFY_MODULE,
                    "exported_symbols",
                    return_value={"required_symbol"},
                ),
            ):
                with self.assertRaisesRegex(
                    VERIFY_MODULE.VerificationError, r"unrequired|physical closure"
                ):
                    VERIFY_MODULE.verify_stack(
                        {"artifact_contract": contract},
                        stack_root,
                        "arm64",
                        {"dylibs": identities},
                    )

    def test_verifier_rejects_dangling_chained_and_escaping_dylib_aliases(self):
        for scenario in ("dangling", "chain", "escape"):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                libdir = root / "lib"
                libdir.mkdir()
                physical = libdir / "librequired.1.dylib"
                physical.write_bytes(b"required dylib fixture")
                alias = libdir / "librequired.dylib"
                outside = root / "outside.dylib"
                outside.write_bytes(b"outside dylib fixture")
                try:
                    if scenario == "dangling":
                        alias.symlink_to("missing.dylib")
                    elif scenario == "chain":
                        intermediate = libdir / "libintermediate.dylib"
                        intermediate.symlink_to(physical.name)
                        alias.symlink_to(intermediate.name)
                    else:
                        alias.symlink_to(pathlib.Path("..") / outside.name)
                except (NotImplementedError, OSError) as error:
                    self.skipTest(f"filesystem symlinks are unavailable: {error}")

                contract = {
                    "thin_artifacts": {"arm64": {"deployment_target": "14.0"}},
                    "allowed_dylib_rpaths": ["@loader_path"],
                    "allowed_system_dependencies": ["/usr/lib/libSystem.B.dylib"],
                    "required_dylibs": [alias.name],
                    "required_exported_symbols": {alias.name: ["required_symbol"]},
                    "forbidden_dynamic_references": [],
                }
                receipt = {
                    "dylibs": [
                        {"name": physical.name, "sha256": VERIFY_MODULE.sha256(physical)}
                    ]
                }
                with self.assertRaisesRegex(
                    VERIFY_MODULE.VerificationError,
                    r"alias|chain|dangling|escape|regular target",
                ):
                    VERIFY_MODULE.verify_stack(
                        {"artifact_contract": contract}, root, "arm64", receipt
                    )

    def test_verifier_rejects_aliases_redirected_to_a_different_required_library(self):
        with tempfile.TemporaryDirectory() as temporary:
            libdir = pathlib.Path(temporary) / "lib"
            libdir.mkdir()
            other_library = libdir / "libother.1.dylib"
            other_library.write_bytes(b"other required dylib fixture")
            alias = libdir / "librequired.dylib"
            try:
                alias.symlink_to(other_library.name)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"filesystem symlinks are unavailable: {error}")

            with self.assertRaisesRegex(
                VERIFY_MODULE.VerificationError, r"alias.*name|library identity"
            ):
                VERIFY_MODULE.direct_dylib_target(alias, libdir, alias.name)

    def test_verifier_enforces_macho_closure_install_names_architectures_and_targets(self):
        verifier = required_text(VERIFY_SCRIPT)
        for token in (
            "otool",
            "-L",
            "-D",
            "@rpath",
            "install_name",
            "lipo",
            "x86_64",
            "arm64",
            "vtool",
            "deployment_target",
            "LC_BUILD_VERSION",
            "sha256",
        ):
            self.assertIn(token, verifier)
        for forbidden in (
            "/opt/homebrew",
            "/usr/local/opt",
            "MobileDevice.framework",
            "/System/Library/PrivateFrameworks",
        ):
            self.assertIn(
                forbidden,
                verifier,
                f"verifier must reject dynamic closure reference {forbidden}",
            )
        self.assertRegex(verifier, r"(?:usbmuxd|daemon)")
        self.assertRegex(verifier, r"(?:missing|not found).*(?:fail|error|raise)|raise.*missing")

        artifact_contract = self.lock().get("artifact_contract")
        self.assertIsInstance(artifact_contract, dict)
        self.assertEqual(artifact_contract.get("dylib_install_name_prefix"), "@rpath/")
        self.assertEqual(
            artifact_contract.get("required_architectures"), ["x86_64", "arm64"]
        )
        thin = artifact_contract.get("thin_artifacts")
        self.assertEqual(sorted(thin), sorted(EXPECTED_THIN_ARTIFACTS))
        for architecture, expected_target in EXPECTED_THIN_ARTIFACTS.items():
            receipt = thin[architecture]
            self.assertEqual(receipt.get("architecture"), architecture)
            self.assertEqual(receipt.get("deployment_target"), expected_target)
            self.assertTrue(receipt.get("receipt_file"))

    def test_legal_sbom_source_offer_and_receipts_bind_every_shipped_dylib(self):
        sources = self.sources()
        for dependency, source in sources.items():
            legal = source.get("legal")
            self.assertIsInstance(legal, dict, dependency)
            self.assertTrue(legal.get("spdx_license"), dependency)
            self.assertTrue(legal.get("license_files"), dependency)
            self.assertTrue(legal.get("notice_label"), dependency)
            self.assertTrue(legal.get("source_offer_label"), dependency)

        legal_script = required_text(LEGAL_SCRIPT)
        for token in (
            "SPDX",
            "source",
            "license",
            "notice",
            "sbom",
            "receipt",
            "sha256",
            "package",
        ):
            self.assertIn(token.lower(), legal_script.lower())
        for dependency in sources:
            self.assertIn(dependency, legal_script)

        receipts = self.lock().get("receipts")
        self.assertIsInstance(receipts, dict)
        self.assertEqual(
            sorted(receipts),
            sorted(("build", "source", "legal", "sbom", "package")),
        )
        for kind, path in receipts.items():
            self.assertTrue(path, kind)

        package_action = required_text(MAC_PACKAGE_ACTION)
        for path in receipts.values():
            self.assertIn(path, package_action)
        self.assertIn(VERIFY_SCRIPT.relative_to(ROOT).as_posix(), package_action)
        self.assertIn(LOCK.relative_to(ROOT).as_posix(), package_action)

    def test_source_set_receipt_is_architecture_independent_and_hash_bound(self):
        legal = required_text(LEGAL_SCRIPT)
        verifier = required_text(VERIFY_SCRIPT)
        build = required_text(BUILD_SCRIPT)
        receipt_name = "ios-native-source-set-receipt.json"

        self.assertIn(receipt_name, legal)
        self.assertIn('"receipt_kind": "component-source-set"', legal)
        self.assertIn('"component": "ios-native"', legal)
        source_set_start = legal.index('"receipt_kind": "component-source-set"')
        source_set_end = legal.find("write_json", source_set_start)
        self.assertGreater(source_set_end, source_set_start)
        source_set_block = legal[source_set_start:source_set_end]
        self.assertNotIn("architecture", source_set_block)
        self.assertNotIn("deployment_target", source_set_block)
        for token in (
            "lock_sha256",
            "archive",
            "source_identity",
            "tree_hash_algorithm",
            "final_tree_sha256",
            "package_support_assets",
            "package_required",
            "release_required",
        ):
            self.assertIn(token, source_set_block)

        self.assertIn("source_set_receipt_sha256", legal + "\n" + build)
        package_start = verifier.index("package = {")
        package_end = verifier.index("args.package_receipt.write_text", package_start)
        self.assertIn(
            "source_set_receipt_sha256",
            verifier[package_start:package_end],
            "iOS package receipts must bind the architecture-independent source set",
        )

    def test_package_and_release_scopes_resolve_ios_source_archive_separately(self):
        verifier = required_text(VERIFY_SCRIPT)
        self.assertNotRegex(
            verifier,
            re.compile(
                r"bound_asset\(source_receipt_path\.parent,\s*"
                r"source\.get\(\"source_offer\"\),\s*\"corresponding source\"\)"
            ),
            "package verification must not unconditionally resolve the release-only source archive beside the package receipt",
        )
        for token in (
            "--asset-scope",
            "--source-assets-root",
            "package",
            "release",
            "must not contain the corresponding source archive",
        ):
            self.assertIn(token, verifier)

    def test_release_scope_requires_external_source_assets_without_package_receipt_operation(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = pathlib.Path(tempdir)
            lock = root / "lock.json"
            receipt = root / "ios-native-build-receipt.json"
            lock.write_text("{}\n", encoding="utf-8")
            receipt.write_text("{}\n", encoding="utf-8")
            argv = [
                str(VERIFY_SCRIPT),
                "--lock",
                str(lock),
                "--stack-root",
                str(root),
                "--architecture",
                "arm64",
                "--receipt",
                str(receipt),
                "--asset-scope",
                "release",
                "--source-assets-root",
                str(root / "external-sources"),
                "--source-receipt",
                str(root / "source-set.json"),
                "--legal-receipt",
                str(root / "legal.json"),
                "--sbom",
                str(root / "sbom.json"),
            ]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                VERIFY_MODULE, "validate_build_receipt"
            ), mock.patch.object(
                VERIFY_MODULE, "verify_stack", return_value=[]
            ), mock.patch.object(
                VERIFY_MODULE,
                "verify_legal_assets",
                side_effect=VERIFY_MODULE.VerificationError(
                    "missing corresponding source archive"
                ),
            ):
                self.assertNotEqual(
                    VERIFY_MODULE.main(),
                    0,
                    "release scope must verify its external archive even without package receipt creation/verification",
                )

    def test_native_stack_smoke_receives_qt_path_without_packaging_rpath(self):
        app_cmake = required_text(APP_CMAKE)
        native_rpath_block = app_cmake.split("if(KLOGG_ENABLE_IOS_NATIVE_STACK)", 1)[
            1
        ].split("endif()", 1)[0]
        self.assertIn("SKIP_BUILD_RPATH ON", native_rpath_block)
        self.assertIn(
            'KLOGG_QT_BUILD_RPATH "${${KLOGG_QT_PACKAGE_DIR_VARIABLE}}/../.." ABSOLUTE',
            native_rpath_block,
        )
        self.assertIn(
            'KLOGG_QT_BUILD_RPATH "${KLOGG_QT_BUILD_RPATH}"', native_rpath_block
        )
        self.assertIn(
            '"LINKER:-rpath,@executable_path/../Frameworks"', native_rpath_block
        )
        self.assertIn(
            '"LINKER:-rpath,@executable_path/../Frameworks/ios-native/lib"',
            native_rpath_block,
        )
        root_cmake = required_text(ROOT_CMAKE)
        self.assertIn("DYLD_FRAMEWORK_PATH=${KLOGG_SMOKE_QT_RPATH}", root_cmake)
        self.assertIn("DYLD_LIBRARY_PATH=${KLOGG_SMOKE_QT_RPATH}", root_cmake)

    def test_ios_native_homebrew_bootstrap_cleans_aws_formula_and_tap_before_install(self):
        workflow = required_text(CI_BUILD_WORKFLOW)
        producer = workflow.split("  BuildIosNativeStacks:", 1)[1].split("\n  MacPackages:", 1)[0]
        install_step = producer.split(
            "      - name: Install iOS native source-build tools\n", 1
        )[1].split("\n      - uses: actions/download-artifact@", 1)[0]
        uninstall = "brew uninstall --ignore-dependencies aws-sam-cli"
        untap = "brew untap aws/tap"
        install = "brew install autoconf automake libtool pkg-config cmake ninja"
        failures = []
        for marker in (
            "HOMEBREW_NO_AUTO_UPDATE=1",
            "HOMEBREW_NO_INSTALL_CLEANUP=1",
            uninstall,
            untap,
            install,
        ):
            if marker not in install_step:
                failures.append(f"missing {marker}")
        if not failures:
            if not (
                install_step.index(uninstall)
                < install_step.index(untap)
                < install_step.index(install)
            ):
                failures.append(
                    "aws-sam-cli must be uninstalled before aws/tap is untapped "
                    "and before project build tools are installed"
                )
            cleanup = install_step[
                install_step.index(uninstall) : install_step.index(install)
            ]
            if "|| true" not in cleanup and "brew list --formula" not in cleanup:
                failures.append(
                    "shared AWS Homebrew cleanup must tolerate an absent formula and tap"
                )
        self.assertEqual(failures, [], "\n".join(failures))

    def test_ci_produces_both_thin_stacks_and_mac_consumes_the_bound_artifact(self):
        workflow = required_text(CI_BUILD_WORKFLOW)
        for token in (
            "BuildIosNativeStacks:",
            "prefetch_ios_native_sources.py",
            "build_ios_native_stack.py",
            "build_ios_native_legal_assets.py",
            "verify_ios_native_stack.py",
            "ios-native-x86_64",
            "ios-native-arm64",
            "ios-native-build-receipt.json",
            "ios-native-source-receipt.json",
            "ios-native-legal-receipt.json",
            "ios-native-sbom.spdx.json",
            "SHA256SUMS",
            "KLOGG_IOS_NATIVE_STACK_ROOT",
            "KLOGG_ENABLE_IOS_NATIVE_STACK=ON",
            "FETCHCONTENT_FULLY_DISCONNECTED=ON",
        ):
            self.assertIn(token, workflow)
        self.assertRegex(
            workflow,
            r"MacPackages:\s+needs:\s*\[[^\]]*BuildIosNativeStacks",
        )
        self.assertRegex(workflow, r"download-artifact@[^\r\n]+[\s\S]+name:\s+ios-native-\$\{\{")
        self.assertIn("${{ matrix.artifact }}.tar.gz", workflow)
        self.assertIn("tar -czf", workflow)
        self.assertIn("tar -xzf", workflow)
        self.assertIn("prefetch_artifacts/ios-native-archive", workflow)
        producer = workflow.split("  BuildIosNativeStacks:", 1)[1].split("\n  MacPackages:", 1)[0]
        for marker in (
            "id: upload_ios_stack",
            "continue-on-error: true",
            "steps.upload_ios_stack.outcome == 'failure'",
            "overwrite: true",
        ):
            self.assertIn(marker, producer)
        self.assertEqual(producer.count("continue-on-error: true"), 1)

    def test_commit_archives_receive_locked_autotools_tarball_versions(self):
        build = required_text(BUILD_SCRIPT)
        self.assertIn(".tarball-version", build)
        self.assertIn('record["version"]', build)
        self.assertLess(build.index("patched_tree_sha256"), build.index(".tarball-version"))

    def test_disconnected_superbuild_has_no_download_or_update_boundary(self):
        superbuild = required_text(SUPERBUILD)
        self.assertNotRegex(superbuild, r"https?://|\bGIT_REPOSITORY\b|\bURL\b")
        external_projects = cmake_calls(superbuild, "ExternalProject_Add")
        self.assertGreaterEqual(len(external_projects), 2)
        for project in external_projects:
            tokens = cmake_tokens(project)
            self.assertIn("DOWNLOAD_COMMAND", tokens)
            self.assertEqual(tokens[tokens.index("DOWNLOAD_COMMAND") + 1], "")
            self.assertIn("UPDATE_COMMAND", tokens)
            self.assertEqual(tokens[tokens.index("UPDATE_COMMAND") + 1], "")
            self.assertIn("PATCH_COMMAND", tokens)
            self.assertEqual(tokens[tokens.index("PATCH_COMMAND") + 1], "")

    def test_source_extraction_rejects_special_and_duplicate_members(self):
        def archive_with(members):
            temporary = tempfile.NamedTemporaryFile(suffix=".tar.gz", delete=False)
            temporary.close()
            path = pathlib.Path(temporary.name)
            with tarfile.open(path, "w:gz") as archive:
                root = tarfile.TarInfo("source")
                root.type = tarfile.DIRTYPE
                archive.addfile(root)
                for member in members:
                    archive.addfile(member, io.BytesIO(b"payload") if member.isfile() else None)
            self.addCleanup(path.unlink, missing_ok=True)
            return path

        fifo = tarfile.TarInfo("source/pipe")
        fifo.type = tarfile.FIFOTYPE
        duplicate_a = tarfile.TarInfo("source/repeated")
        duplicate_a.size = len(b"payload")
        duplicate_b = tarfile.TarInfo("source/repeated")
        duplicate_b.size = len(b"payload")

        with tempfile.TemporaryDirectory() as output:
            with self.assertRaises(BUILD_MODULE.BuildError):
                BUILD_MODULE.safe_extract(
                    archive_with([fifo]), pathlib.Path(output) / "special"
                )
            with self.assertRaises(BUILD_MODULE.BuildError):
                BUILD_MODULE.safe_extract(
                    archive_with([duplicate_a, duplicate_b]), pathlib.Path(output) / "duplicate"
                )

    def test_legal_assets_bind_build_receipt_sbom_and_lgpl_replacement_guide(self):
        legal = required_text(LEGAL_SCRIPT)
        for token in (
            "lock_sha256",
            "receipt_kind",
            "architecture",
            "deployment_target",
            "licenseInfoInFiles",
            "copyrightText",
            "relationships",
            "ios-native-lgpl-replacement.txt",
        ):
            self.assertIn(token, legal)
        action = required_text(MAC_PACKAGE_ACTION)
        self.assertIn("ios-native-lgpl-replacement.txt", action)
        self.assertIn("codesign --force --deep --sign -", legal)

    def test_external_ios_source_offer_is_actionable_and_archive_rebuild_paths_match(self):
        legal = required_text(LEGAL_SCRIPT)
        for token in (
            "--version",
            "--base-url",
            "not included in the installer",
            'f"{base_url}/releases"',
            "shasum -a 256",
            "validated_relative_path(",
            'patch.get("path"), "locked patch"',
            'f"3rdparty/libimobiledevice/{patch_path}"',
        ):
            self.assertIn(token, legal)
        self.assertIn('f"{base_url}/releases/tag/continuous"', legal)
        self.assertNotIn('source_asset_url(base_url, "continuous"', legal)
        self.assertIn("rolling and mutable", legal)
        self.assertNotIn(
            "The accompanying ios-native-corresponding-source.tar.gz",
            legal,
        )

    def test_macos_root_closure_check_targets_ios_specific_dylibs_only(self):
        action = required_text(MAC_PACKAGE_ACTION)
        start = action.index('if find "$app_frameworks"')
        end = action.index('case "${KLOGG_ARCH}"', start)
        check = action[start:end]
        for name in (
            "libimobiledevice",
            "libplist",
            "libusbmuxd",
            "libimobiledevice-glue",
            "libtatsu",
        ):
            self.assertIn(name, check)
        for qt_runtime_dependency in ("libssl", "libcrypto", "libcurl"):
            self.assertNotIn(qt_runtime_dependency, check)

    def test_staged_bundle_is_reverified_after_macdeployqt_before_signing(self):
        action = required_text(MAC_PACKAGE_ACTION)
        deploy = action.index("Mac deploy Qt")
        staged_verify = action.index("Verify staged iOS native bundle")
        signing = action.index("Sign and verify macOS application bundle")
        self.assertLess(deploy, staged_verify)
        self.assertLess(staged_verify, signing)
        staged_section = action[staged_verify:signing]
        self.assertIn("--app-executable", staged_section)
        self.assertIn("Contents/Frameworks/ios-native", staged_section)
        verifier = required_text(VERIFY_SCRIPT)
        for token in ("--app-executable", "LC_RPATH", "otool", "-l"):
            self.assertIn(token, verifier)

    def test_verifier_rejects_hostile_rpaths_unlocked_system_imports_and_unqualified_receipts(self):
        contract = self.lock().get("artifact_contract", {})
        self.assertEqual(contract.get("allowed_dylib_rpaths"), ["@loader_path"])
        allowed_system = contract.get("allowed_system_dependencies")
        self.assertIsInstance(allowed_system, list)
        self.assertIn("/usr/lib/libSystem.B.dylib", allowed_system)

        verifier = required_text(VERIFY_SCRIPT)
        for token in (
            "LC_RPATH",
            "allowed_dylib_rpaths",
            "allowed_system_dependencies",
            "native_qualified",
            "qualification",
            "receipt_kind",
        ):
            self.assertIn(token, verifier)

    def test_unsigned_validation_receipts_do_not_require_code_signatures(self):
        action = required_text(MAC_PACKAGE_ACTION)
        validation = action.split("- name: Validate unsigned macOS disk image", 1)[1].split(
            "- name: Sign and verify macOS disk image", 1
        )[0]
        self.assertIn("--unsigned-package-stage", validation)
        verifier = required_text(VERIFY_SCRIPT)
        self.assertIn("--unsigned-package-stage", verifier)
        self.assertIn(
            "args.verify_package_receipt is not None and not args.unsigned_package_stage",
            verifier,
        )

    def test_package_receipt_binds_legal_source_sbom_and_final_signed_closure(self):
        verifier = required_text(VERIFY_SCRIPT)
        for token in (
            "source_receipt",
            "legal_receipt",
            "sbom",
            "source_receipt_sha256",
            "legal_receipt_sha256",
            "sbom_sha256",
        ):
            self.assertIn(token, verifier)

        action = required_text(MAC_PACKAGE_ACTION)
        sign_position = action.index("Sign and verify macOS application bundle")
        final_verify_positions = [
            match.start()
            for match in re.finditer("verify_ios_native_stack.py", action)
            if match.start() > sign_position
        ]
        self.assertTrue(
            final_verify_positions,
            "the signed app-bundled closure must be reverified after application signing",
        )
        signed_verification = action[final_verify_positions[-1] :]
        self.assertIn("--source-receipt", signed_verification)
        self.assertIn("--legal-receipt", signed_verification)
        self.assertIn("--sbom", signed_verification)
        self.assertIn("--verify-package-receipt", signed_verification)

    def test_release_secrets_are_transported_through_step_environment(self):
        action = required_text(MAC_PACKAGE_ACTION)
        for unsafe in (
            'require_input "p12-password" "${{ inputs.p12-password }}"',
            '--password "${{ inputs.notarization-password }}"',
            'echo "KLOGG_CODESIGN=${{ inputs.codesign-identity }}"',
        ):
            self.assertNotIn(unsafe, action)
        for marker in (
            "KLOGG_P12_PASSWORD: ${{ inputs.p12-password }}",
            "KLOGG_NOTARIZATION_PASSWORD: ${{ inputs.notarization-password }}",
            'printf \'KLOGG_CODESIGN=%s\\n\'',
        ):
            self.assertIn(marker, action)

    def test_release_consumption_is_disconnected_and_fails_closed(self):
        action = required_text(MAC_PACKAGE_ACTION)
        self.assertTrue(
            "FETCHCONTENT_FULLY_DISCONNECTED=ON" in action,
            "macOS release consumption must configure with FetchContent fully disconnected",
        )
        self.assertTrue(
            "KLOGG_IOS_NATIVE_STACK_ROOT" in action,
            "macOS release consumption must require an explicit prefetched native stack root",
        )
        self.assertTrue(
            "verify_ios_native_stack.py" in action,
            "macOS packaging must invoke the fail-closed native stack verifier",
        )
        self.assertNotRegex(action, r"\|\|\s*(?:true|echo .*Warning)")

        build = required_text(THIRD_PARTY_CMAKE)
        self.assertIn("KLOGG_IOS_NATIVE_STACK_ROOT", build)
        self.assertRegex(build, r"(?:FATAL_ERROR|SEND_ERROR).*(?:iOS|IOS).*native")
        self.assertNotRegex(build, r"find_(?:package|library)\([^\n]*(?:usbmuxd|plist|imobiledevice)")


if __name__ == "__main__":
    unittest.main()
