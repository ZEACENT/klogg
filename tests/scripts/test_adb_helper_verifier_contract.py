from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).parents[2]
VERIFY_SCRIPT = ROOT / "scripts" / "verify_adb_helper_artifact.py"
ENVELOPE_SCRIPT = ROOT / "scripts" / "verify_adb_helper_envelope.py"
SMOKE_SCRIPT = ROOT / "scripts" / "smoke_adb_helper.py"

PRODUCTION_SERVER_SCRUBBED_ENVIRONMENT = [
    "ADB_VENDOR_KEYS",
    "ADB_SERVER_PORT",
    "ANDROID_ADB_SERVER_PORT",
    "ANDROID_ADB_SERVER_ADDRESS",
]


def production_server_invocation_probe(port: int) -> dict:
    return {
        "name": "production-server-invocation",
        "arguments": ["server", "nodaemon"],
        "environment": {"ADB_SERVER_SOCKET": f"tcp:{port}"},
        "scrubbed_environment": PRODUCTION_SERVER_SCRUBBED_ENVIRONMENT,
    }


class AdbHelperVerifierContractTest(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tempdir.name)

    def tearDown(self):
        self.tempdir.cleanup()

    def write_file(self, relative_path: str, content: bytes, executable: bool = False) -> pathlib.Path:
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        if executable:
            path.chmod(0o755)
        return path

    def make_release_fixture(self, target: str = "linux-x86_64", layout: str = "deb"):
        helper_relative = "usr/bin/helpers/adb"
        usb = {
            "backend": "dynamic-libusb",
            "required_imports": ["libusb-1.0.so.0"],
            "runtime_files": ["libusb-1.0.so.0"],
            "required_private_imports_by_binary": {
                "adb": ["libusb-1.0.so.0"],
            },
            "replacement_probe_required": True,
        }
        imports = ["libusb-1.0.so.0", "libc.so.6"]
        frameworks = []
        if target == "windows-x86_64":
            helper_relative = "helpers/adb.exe"
            usb["required_imports"] = ["AdbWinApi.dll", "libusb-1.0.dll"]
            usb["runtime_files"] = [
                "AdbWinApi.dll",
                "AdbWinUsbApi.dll",
                "libusb-1.0.dll",
            ]
            usb["required_delayed_runtime_loads"] = [
                {
                    "runtime_file": "AdbWinUsbApi.dll",
                    "loaded_by": "AdbWinApi.dll",
                    "source": "windows-platform-development/AdbWinApi.cpp",
                    "expression": 'LoadLibrary(L"AdbWinUsbApi.dll")',
                    "source_sha256": "a" * 64,
                    "loader_symbol": "LoadLibraryW",
                }
            ]
            usb["required_private_imports_by_binary"] = {
                "adb.exe": ["AdbWinApi.dll", "libusb-1.0.dll"],
                "AdbWinApi.dll": [],
                "AdbWinUsbApi.dll": ["AdbWinApi.dll"],
                "libusb-1.0.dll": [],
            }
            usb["allowed_system_imports_by_binary"] = {
                "adb.exe": ["KERNEL32.dll"],
                "AdbWinApi.dll": ["KERNEL32.dll"],
                "AdbWinUsbApi.dll": ["WINUSB.dll"],
                "libusb-1.0.dll": ["KERNEL32.dll"],
            }
            imports = ["AdbWinApi.dll", "libusb-1.0.dll", "kernel32.dll"]
        elif target.startswith("macos-"):
            helper_relative = "klogg.app/Contents/MacOS/helpers/adb"
            usb = {
                "backend": "native-iokit",
                "frameworks": ["IOKit", "CoreFoundation"],
                "forbidden_imports": ["libusb"],
            }
            imports = ["libSystem.B.dylib"]
            frameworks = ["IOKit", "CoreFoundation"]

        package_root = self.root / "package"
        helper = self.write_file(
            f"package/{helper_relative}", b"fixture complete adb executable\n", executable=True
        )
        helper_hash = hashlib.sha256(helper.read_bytes()).hexdigest()
        runtime_closure = []
        if usb["backend"] == "dynamic-libusb":
            for runtime_name in usb["runtime_files"]:
                runtime = self.write_file(
                    f"package/{pathlib.PurePosixPath(helper_relative).parent}/{runtime_name}",
                    f"fixture private runtime {runtime_name}\n".encode(),
                )
                runtime_closure.append(
                    {
                        "name": runtime_name,
                        "sha256": hashlib.sha256(runtime.read_bytes()).hexdigest(),
                        "symlink": False,
                    }
                )

        asset_specs = (
            ("source-archive", "adb-helper-source-archive.tar.gz", False),
            ("licenses", "adb-helper-licenses.tar.gz", True),
            ("notices", "adb-helper-notices.tar.gz", True),
            ("sbom", "adb-helper-sbom.spdx.json", True),
            ("source-offer", "ADB-HELPER-SOURCE-OFFER.txt", True),
            ("source-manifest", "adb-helper-source-manifest.json", True),
        )
        assets = []
        package_support_assets = []
        for kind, name, package_required in asset_specs:
            asset = self.write_file(f"release/{name}", f"{kind}\n".encode())
            asset_hash = hashlib.sha256(asset.read_bytes()).hexdigest()
            self.write_file(
                f"release/{asset.name}.sha256",
                f"{asset_hash}  {asset.name}\n".encode(),
            )
            assets.append({"kind": kind, "path": asset.name, "sha256": asset_hash})
            if package_required:
                package_support_assets.append(
                    {"kind": kind, "file_name": asset.name, "sha256": asset_hash}
                )

        source_archive = next(asset for asset in assets if asset["kind"] == "source-archive")
        source_set_document = {
            "schema_version": 1,
            "receipt_kind": "component-source-set",
            "component": "adb-helper",
            "lock_sha256": "1" * 64,
            "archive": {
                "file_name": source_archive["path"],
                "sha256": source_archive["sha256"],
            },
            "source_identity": {
                "manifest_or_closure_sha256": "2" * 64,
                "tree_hash_algorithm": "sha256",
                "final_tree_sha256": "3" * 64,
            },
            "patch_chain_sha256": "4" * 64,
            "package_support_assets": package_support_assets,
            "distribution": {"package_required": False, "release_required": True},
        }
        source_set = self.write_file(
            "release/adb-helper-source-set-receipt.json",
            (json.dumps(source_set_document, sort_keys=True) + "\n").encode(),
        )
        source_set_hash = hashlib.sha256(source_set.read_bytes()).hexdigest()
        self.write_file(
            f"release/{source_set.name}.sha256",
            f"{source_set_hash}  {source_set.name}\n".encode(),
        )
        assets.append(
            {"kind": "source-set-receipt", "path": source_set.name, "sha256": source_set_hash}
        )

        target_arch = "arm64" if target.endswith("arm64") else "x86_64"
        target_plan = {"arch": target_arch, "qualified": True, "usb": usb}
        if target.startswith("linux-"):
            target_plan["glibc_baseline"] = "2.35"
        package_target = {
            "deb": "linux-jammy-x86_64",
            "dmg": f"{target}-dmg",
            "nsis": "windows-x86_64",
        }.get(layout, f"{target}-package")
        required_receipts = ["binary-build", "binary-smoke", "package-verification"]
        if target.startswith("macos-"):
            required_receipts.extend(("signing", "notarization"))
        lock = {
            "schema_version": 2,
            "helper": {
                "kind": "complete-adb-executable",
                "required_client_commands": ["version", "help"],
                "required_roles": ["client", "server"],
            },
            "targets": {target: target_plan},
            "package_targets": {
                package_target: {
                    "helper_target": target,
                    "qualification": {
                        "validation_class": "locally-buildable",
                        "required_receipts": required_receipts,
                        "verified_receipts": [],
                        "release_qualified": False,
                    },
                }
            },
            "install_paths": {layout: helper_relative},
            "release_assets": [
                {
                    "kind": kind,
                    "required": True,
                    "file_name": name,
                    "sha256_file": name + ".sha256",
                    "distribution": {
                        "package_required": package_required,
                        "release_required": True,
                    },
                }
                for kind, name, package_required in (
                    *asset_specs,
                    ("source-set-receipt", source_set.name, True),
                )
            ],
        }
        lock_path = self.root / "lock.json"
        lock_path.write_text(json.dumps(lock), encoding="utf-8")
        source_set_document["lock_sha256"] = hashlib.sha256(lock_path.read_bytes()).hexdigest()
        source_set.write_text(json.dumps(source_set_document, sort_keys=True) + "\n", encoding="utf-8")
        source_set_hash = hashlib.sha256(source_set.read_bytes()).hexdigest()
        source_set.with_name(source_set.name + ".sha256").write_text(
            f"{source_set_hash}  {source_set.name}\n", encoding="utf-8"
        )
        next(asset for asset in assets if asset["kind"] == "source-set-receipt")[
            "sha256"
        ] = source_set_hash

        imports_by_binary = {pathlib.PurePosixPath(helper_relative).name: imports}
        runtime_loads = []
        runtime_load_evidence = []
        observed_runtime_edges = []
        windows_system_imports = {}
        if target == "windows-x86_64":
            imports_by_binary = {
                "adb.exe": imports,
                "AdbWinApi.dll": ["KERNEL32.dll"],
                "AdbWinUsbApi.dll": ["AdbWinApi.dll", "WINUSB.dll"],
                "libusb-1.0.dll": ["KERNEL32.dll"],
            }
            windows_system_imports = {
                "adb.exe": ["kernel32.dll"],
                "AdbWinApi.dll": ["KERNEL32.dll"],
                "AdbWinUsbApi.dll": ["WINUSB.dll"],
                "libusb-1.0.dll": ["KERNEL32.dll"],
            }
            runtime_loads = list(usb["runtime_files"])
            runtime_load_evidence = [
                {
                    **usb["required_delayed_runtime_loads"][0],
                    "runtime_name_encoding": "utf-16le",
                }
            ]
            observed_runtime_edges = [
                {"loaded_by": "AdbWinApi.dll", "runtime_file": "AdbWinUsbApi.dll"}
            ]

        receipt = {
            "schema_version": 1,
            "target": target,
            "layout": layout,
            "helper": {
                "path": helper_relative,
                "sha256": helper_hash,
                "executable": True,
                "symlink": False,
                "kind": "complete-adb-executable",
            },
            "source_set_receipt_sha256": source_set_hash,
            "binary_verification": {
                "schema_version": 2,
                "dynamic_imports": imports,
                "imports_by_binary": imports_by_binary,
                "windows_system_imports_by_binary": windows_system_imports,
                "native_frameworks": frameworks,
                "architectures": [target_arch],
                "runtime_closure": runtime_closure,
                "runtime_loads": runtime_loads,
                "runtime_load_evidence": runtime_load_evidence,
                "observed_runtime_edges": observed_runtime_edges,
                "glibc_maximum_required": "2.34" if target.startswith("linux-") else None,
                "libusb_replacement_probe": "passed"
                if usb["backend"] == "dynamic-libusb"
                else "not-applicable",
                "version_probe": "passed",
                "complete_client_probe": "passed",
            },
            "release_assets": assets,
        }
        receipt_path = self.root / "receipt.json"
        receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
        return lock_path, receipt_path, package_root, self.root / "release", receipt

    def write_smoke_receipt(self, helper: pathlib.Path, name: str = "binary-smoke.json") -> pathlib.Path:
        port = 5037
        receipt = {
            "schema_version": 1,
            "receipt_kind": "binary-smoke",
            "helper_sha256": hashlib.sha256(helper.read_bytes()).hexdigest(),
            "server_endpoint": f"tcp:127.0.0.1:{port}",
            "required_probe": production_server_invocation_probe(port),
            "host_version": "0029",
            "stability_host_version": "0029",
            "stability_window_seconds": 0.25,
            "passed_probes": [
                "version",
                "complete-client",
                "loopback-private-server",
                "smart-socket-host-version",
                "no-lingering-process",
            ],
        }
        path = self.root / name
        path.write_text(json.dumps(receipt), encoding="utf-8")
        return path

    def write_checksum_envelope(self, root: pathlib.Path, names: list[str]) -> pathlib.Path:
        lines = []
        for name in names:
            path = root / name
            lines.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {name}")
        envelope = root / "SHA256SUMS"
        envelope.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return envelope

    def run_verifier(self, lock, receipt, package_root, release_root, *extra_args):
        self.assertTrue(VERIFY_SCRIPT.is_file(), f"missing verifier script: {VERIFY_SCRIPT}")
        return subprocess.run(
            [
                sys.executable,
                str(VERIFY_SCRIPT),
                "--lock",
                str(lock),
                "--receipt",
                str(receipt),
                "--package-root",
                str(package_root),
                "--release-root",
                str(release_root),
                *extra_args,
            ],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def run_scoped_verifier(
        self,
        lock,
        receipt,
        package_root,
        source_assets_root,
        asset_scope,
        *extra_args,
        include_smoke: bool = True,
    ):
        self.assertTrue(VERIFY_SCRIPT.is_file(), f"missing verifier script: {VERIFY_SCRIPT}")
        command = [
            sys.executable,
            str(VERIFY_SCRIPT),
            "--lock",
            str(lock),
            "--receipt",
            str(receipt),
            "--package-root",
            str(package_root),
            "--asset-scope",
            asset_scope,
            "--source-assets-root",
            str(source_assets_root),
        ]
        if include_smoke:
            receipt_document = json.loads(pathlib.Path(receipt).read_text(encoding="utf-8"))
            helper = pathlib.Path(package_root) / receipt_document["helper"]["path"]
            smoke = self.write_smoke_receipt(helper, f"{asset_scope}-scope-smoke.json")
            command.extend(("--binary-smoke-receipt", str(smoke)))
        command.extend(extra_args)
        return subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def test_verifier_accepts_complete_locked_release_fixture(self):
        lock, receipt, package_root, release_root, _ = self.make_release_fixture()
        result = self.run_verifier(lock, receipt, package_root, release_root)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_verifier_accepts_exact_structured_production_server_invocation_probe(self):
        lock, receipt, package_root, release_root, document = self.make_release_fixture()
        helper = package_root / document["helper"]["path"]
        smoke = self.write_smoke_receipt(helper)

        result = self.run_verifier(
            lock,
            receipt,
            package_root,
            release_root,
            "--binary-smoke-receipt",
            str(smoke),
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_verifier_rejects_boolean_smoke_schema_version(self):
        lock, receipt, package_root, release_root, document = self.make_release_fixture()
        helper = package_root / document["helper"]["path"]
        smoke = self.write_smoke_receipt(helper)
        smoke_document = json.loads(smoke.read_text(encoding="utf-8"))
        smoke_document["schema_version"] = True
        smoke.write_text(json.dumps(smoke_document), encoding="utf-8")

        result = self.run_verifier(
            lock,
            receipt,
            package_root,
            release_root,
            "--binary-smoke-receipt",
            str(smoke),
        )

        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("schema", (result.stdout + result.stderr).lower())

    def test_verifier_rejects_smoke_receipts_with_failure_diagnostics(self):
        for field in ("error", "cleanup_error"):
            with self.subTest(field=field):
                lock, receipt, package_root, release_root, document = self.make_release_fixture()
                helper = package_root / document["helper"]["path"]
                smoke = self.write_smoke_receipt(helper, f"{field}-smoke.json")
                smoke_document = json.loads(smoke.read_text(encoding="utf-8"))
                smoke_document[field] = "fixture failure"
                smoke.write_text(json.dumps(smoke_document), encoding="utf-8")

                result = self.run_verifier(
                    lock,
                    receipt,
                    package_root,
                    release_root,
                    "--binary-smoke-receipt",
                    str(smoke),
                )

                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("failure", (result.stdout + result.stderr).lower())

    def test_verifier_fails_closed_on_invalid_or_spoofed_server_invocation_probe(self):
        scenarios = (
            "bad",
            "near-miss",
            "malformed",
            "spoof",
            "unstable",
            "no-stability-window",
        )
        for scenario in scenarios:
            with self.subTest(scenario=scenario):
                lock, receipt, package_root, release_root, document = self.make_release_fixture()
                helper = package_root / document["helper"]["path"]
                smoke = self.write_smoke_receipt(helper, f"{scenario}-smoke.json")
                smoke_document = json.loads(smoke.read_text(encoding="utf-8"))
                if scenario == "bad":
                    smoke_document["required_probe"]["arguments"] = [
                        "-L",
                        "tcp:5037",
                        "server",
                        "nodaemon",
                    ]
                elif scenario == "near-miss":
                    smoke_document["required_probe"]["environment"] = {
                        "ADB_SERVER_SOCKET": "tcp:127.0.0.1:5037"
                    }
                elif scenario == "malformed":
                    smoke_document["required_probe"] = "production-server-invocation"
                elif scenario == "spoof":
                    exact_probe = smoke_document.pop("required_probe")
                    smoke_document["diagnostic"] = (
                        "claimed required probe: " + json.dumps(exact_probe, sort_keys=True)
                    )
                elif scenario == "unstable":
                    smoke_document.pop("stability_host_version")
                else:
                    smoke_document.pop("stability_window_seconds")
                smoke.write_text(json.dumps(smoke_document), encoding="utf-8")

                result = self.run_verifier(
                    lock,
                    receipt,
                    package_root,
                    release_root,
                    "--binary-smoke-receipt",
                    str(smoke),
                )

                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("probe", (result.stdout + result.stderr).lower())

    def test_scoped_verification_requires_binary_smoke_evidence(self):
        lock, receipt, package_root, source_assets_root, document = self.make_release_fixture()
        archive = next(
            asset for asset in document["release_assets"] if asset["kind"] == "source-archive"
        )
        archive_path = source_assets_root / archive["path"]
        archive_path.unlink()
        archive_path.with_name(archive_path.name + ".sha256").unlink()

        result = self.run_scoped_verifier(
            lock,
            receipt,
            package_root,
            source_assets_root,
            "package",
            include_smoke=False,
        )

        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("binary-smoke", (result.stdout + result.stderr).lower())

    def test_package_scope_omits_source_archive_but_requires_and_hash_checks_source_set_receipt(self):
        lock, receipt, package_root, source_assets_root, document = self.make_release_fixture()
        archive = next(
            asset for asset in document["release_assets"] if asset["kind"] == "source-archive"
        )
        archive_path = source_assets_root / archive["path"]
        leaked = self.run_scoped_verifier(
            lock, receipt, package_root, source_assets_root, "package"
        )
        self.assertNotEqual(leaked.returncode, 0)
        self.assertIn("archive", (leaked.stdout + leaked.stderr).lower())
        archive_path.unlink()
        archive_path.with_name(archive_path.name + ".sha256").unlink()

        package = self.write_file("package/klogg.deb", b"qualified package\n")
        qualification = self.root / "package-scope-verification.json"
        package_args = (
            "--package-target",
            "linux-jammy-x86_64",
            "--package-file",
            str(package),
            "--package-verification-receipt",
            str(qualification),
        )
        result = self.run_scoped_verifier(
            lock,
            receipt,
            package_root,
            source_assets_root,
            "package",
            *package_args,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        evidence = json.loads(qualification.read_text(encoding="utf-8"))
        self.assertEqual(
            evidence.get("source_set_receipt_sha256"),
            document["source_set_receipt_sha256"],
        )

        source_set = source_assets_root / "adb-helper-source-set-receipt.json"
        source_set.write_text(source_set.read_text(encoding="utf-8") + "\n", encoding="utf-8")
        result = self.run_scoped_verifier(
            lock, receipt, package_root, source_assets_root, "package"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex((result.stdout + result.stderr).lower(), r"source.set|sha256")

    def test_release_scope_requires_source_archive_and_its_hash_sidecar(self):
        for missing in ("archive", "sidecar"):
            with self.subTest(missing=missing):
                lock, receipt, package_root, source_assets_root, document = self.make_release_fixture()
                archive = next(
                    asset
                    for asset in document["release_assets"]
                    if asset["kind"] == "source-archive"
                )
                archive_path = source_assets_root / archive["path"]
                if missing == "archive":
                    archive_path.unlink()
                else:
                    archive_path.with_name(archive_path.name + ".sha256").unlink()

                result = self.run_scoped_verifier(
                    lock, receipt, package_root, source_assets_root, "release"
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex((result.stdout + result.stderr).lower(), r"archive|sha256")

    def test_verifier_rejects_missing_nonexecutable_symlinked_and_hash_mismatched_helpers(self):
        scenarios = ["missing"]
        if os.name != "nt":
            scenarios.append("non-executable")
        scenarios.extend(("symlink", "hash"))
        for scenario in scenarios:
            with self.subTest(scenario=scenario):
                lock, receipt, package_root, release_root, document = self.make_release_fixture()
                helper = package_root / document["helper"]["path"]
                if scenario == "missing":
                    helper.unlink()
                elif scenario == "non-executable":
                    helper.chmod(0o644)
                elif scenario == "symlink":
                    target = self.write_file("outside-adb", b"outside\n", executable=True)
                    helper.unlink()
                    helper.symlink_to(target)
                else:
                    helper.write_bytes(b"tampered\n")
                    helper.chmod(0o755)

                result = self.run_verifier(lock, receipt, package_root, release_root)
                self.assertNotEqual(result.returncode, 0)
                expected = {
                    "missing": "missing",
                    "non-executable": "executable",
                    "symlink": "symlink",
                    "hash": "sha256",
                }[scenario]
                self.assertIn(expected, (result.stdout + result.stderr).lower())

    def test_verifier_requires_actual_dynamic_libusb_import_and_replacement_probe(self):
        for scenario in ("missing-import", "failed-replacement"):
            with self.subTest(scenario=scenario):
                lock, receipt, package_root, release_root, document = self.make_release_fixture()
                if scenario == "missing-import":
                    document["binary_verification"]["dynamic_imports"] = ["libc.so.6"]
                    document["binary_verification"]["imports_by_binary"]["adb"] = [
                        "libc.so.6"
                    ]
                else:
                    document["binary_verification"]["libusb_replacement_probe"] = "failed"
                receipt.write_text(json.dumps(document), encoding="utf-8")

                result = self.run_verifier(lock, receipt, package_root, release_root)
                self.assertNotEqual(result.returncode, 0)
                expected = (
                    "private imports" if scenario == "missing-import" else "libusb"
                )
                self.assertIn(expected, (result.stdout + result.stderr).lower())

    def test_verifier_requires_exact_dynamic_import_names(self):
        lock, receipt, package_root, release_root, document = self.make_release_fixture(
            target="windows-x86_64", layout="nsis"
        )
        document["binary_verification"]["dynamic_imports"] = [
            "evil-libusb-1.0.dll",
            "kernel32.dll",
        ]
        document["binary_verification"]["imports_by_binary"]["adb.exe"] = [
            "evil-libusb-1.0.dll",
            "kernel32.dll",
        ]
        receipt.write_text(json.dumps(document), encoding="utf-8")

        result = self.run_verifier(lock, receipt, package_root, release_root)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("private imports", (result.stdout + result.stderr).lower())

    def test_verifier_rejects_unclassified_windows_host_import(self):
        lock, receipt, package_root, release_root, document = self.make_release_fixture(
            target="windows-x86_64", layout="nsis"
        )
        binary = document["binary_verification"]
        binary["dynamic_imports"].append("host-only-vendor.dll")
        binary["imports_by_binary"]["adb.exe"].append("host-only-vendor.dll")
        binary["windows_system_imports_by_binary"]["adb.exe"].append(
            "host-only-vendor.dll"
        )
        receipt.write_text(json.dumps(document), encoding="utf-8")

        result = self.run_verifier(lock, receipt, package_root, release_root)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("system import evidence", (result.stdout + result.stderr).lower())

    def test_verifier_rejects_tampered_delayed_runtime_evidence(self):
        for scenario in (
            "source-hash",
            "loader-symbol",
            "missing-observed-edge",
            "missing-import-map",
            "invalid-import-map",
            "sidecar-became-direct",
        ):
            with self.subTest(scenario=scenario):
                lock, receipt, package_root, release_root, document = self.make_release_fixture(
                    target="windows-x86_64", layout="nsis"
                )
                binary = document["binary_verification"]
                if scenario == "source-hash":
                    binary["runtime_load_evidence"][0]["source_sha256"] = "0" * 64
                elif scenario == "loader-symbol":
                    binary["runtime_load_evidence"][0]["loader_symbol"] = "LoadLibraryA"
                elif scenario == "missing-observed-edge":
                    binary["observed_runtime_edges"] = []
                elif scenario == "missing-import-map":
                    binary["imports_by_binary"] = {}
                elif scenario == "invalid-import-map":
                    binary["imports_by_binary"]["AdbWinApi.dll"] = "KERNEL32.dll"
                else:
                    binary["imports_by_binary"]["AdbWinApi.dll"].append(
                        "AdbWinUsbApi.dll"
                    )
                receipt.write_text(json.dumps(document), encoding="utf-8")

                result = self.run_verifier(lock, receipt, package_root, release_root)

                self.assertNotEqual(result.returncode, 0)

    def test_verifier_requires_native_iokit_and_forbids_libusb_on_macos(self):
        for scenario in ("missing-iokit", "imports-libusb"):
            with self.subTest(scenario=scenario):
                lock, receipt, package_root, release_root, document = self.make_release_fixture(
                    target="macos-arm64", layout="dmg"
                )
                if scenario == "missing-iokit":
                    document["binary_verification"]["native_frameworks"] = ["CoreFoundation"]
                else:
                    document["binary_verification"]["dynamic_imports"].append("libusb-1.0.dylib")
                receipt.write_text(json.dumps(document), encoding="utf-8")

                result = self.run_verifier(lock, receipt, package_root, release_root)
                self.assertNotEqual(result.returncode, 0)
                output = (result.stdout + result.stderr).lower()
                self.assertTrue("iokit" in output or "libusb" in output, output)

    def test_verifier_rejects_missing_or_uncovered_release_assets(self):
        for scenario in ("missing-asset", "bad-asset-hash"):
            with self.subTest(scenario=scenario):
                lock, receipt, package_root, release_root, document = self.make_release_fixture()
                asset = release_root / document["release_assets"][0]["path"]
                if scenario == "missing-asset":
                    asset.unlink()
                else:
                    asset.write_bytes(b"tampered asset\n")

                result = self.run_verifier(lock, receipt, package_root, release_root)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("asset", (result.stdout + result.stderr).lower())

    def test_verifier_rejects_missing_or_mismatched_release_asset_hash_sidecars(self):
        for scenario in ("missing", "mismatched"):
            with self.subTest(scenario=scenario):
                lock, receipt, package_root, release_root, document = self.make_release_fixture()
                asset = release_root / document["release_assets"][0]["path"]
                sidecar = asset.with_name(asset.name + ".sha256")
                if scenario == "missing":
                    sidecar.unlink()
                else:
                    sidecar.write_text(f"{'0' * 64}  {asset.name}\n", encoding="utf-8")

                result = self.run_verifier(lock, receipt, package_root, release_root)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("sha256", (result.stdout + result.stderr).lower())

    def test_verifier_rejects_missing_or_tampered_private_runtime_closure(self):
        for scenario in ("missing", "tampered"):
            with self.subTest(scenario=scenario):
                lock, receipt, package_root, release_root, document = self.make_release_fixture()
                runtime_name = document["binary_verification"]["runtime_closure"][0]["name"]
                helper = package_root / document["helper"]["path"]
                runtime = helper.parent / runtime_name
                if scenario == "missing":
                    runtime.unlink()
                else:
                    runtime.write_bytes(b"uncontrolled runtime\n")

                result = self.run_verifier(lock, receipt, package_root, release_root)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("runtime", (result.stdout + result.stderr).lower())

    def test_verifier_derives_release_qualification_from_complete_native_evidence(self):
        lock, receipt, package_root, release_root, document = self.make_release_fixture()
        helper = package_root / document["helper"]["path"]
        smoke = self.write_smoke_receipt(helper)
        qualification = self.root / "package-verification.json"

        result = self.run_verifier(
            lock,
            receipt,
            package_root,
            release_root,
            "--binary-smoke-receipt",
            str(smoke),
            "--package-target",
            "linux-jammy-x86_64",
            "--package-verification-receipt",
            str(qualification),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("package file", (result.stdout + result.stderr).lower())

        package = self.write_file("package/klogg.deb", b"qualified package\n")
        result = self.run_verifier(
            lock,
            receipt,
            package_root,
            release_root,
            "--binary-smoke-receipt",
            str(smoke),
            "--package-target",
            "linux-jammy-x86_64",
            "--package-file",
            str(package),
            "--package-verification-receipt",
            str(qualification),
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        evidence = json.loads(qualification.read_text(encoding="utf-8"))
        self.assertEqual(evidence.get("package_sha256"), hashlib.sha256(package.read_bytes()).hexdigest())
        self.assertIs(evidence.get("release_qualified"), True)
        self.assertEqual(
            set(evidence.get("verified_receipts", [])),
            {"binary-build", "binary-smoke", "package-verification"},
        )

    def test_verifier_requires_checksum_envelope_to_bind_cross_job_receipts(self):
        lock, receipt, package_root, release_root, document = self.make_release_fixture()
        helper = package_root / document["helper"]["path"]
        envelope = self.write_checksum_envelope(
            self.root,
            [
                (package_root / document["helper"]["path"]).relative_to(self.root).as_posix(),
                receipt.relative_to(self.root).as_posix(),
            ],
        )

        result = self.run_verifier(
            lock,
            receipt,
            package_root,
            release_root,
            "--checksum-envelope",
            str(envelope),
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        receipt.write_text(receipt.read_text(encoding="utf-8") + "\n", encoding="utf-8")
        result = self.run_verifier(
            lock,
            receipt,
            package_root,
            release_root,
            "--checksum-envelope",
            str(envelope),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checksum", (result.stdout + result.stderr).lower())
        self.assertTrue(helper.is_file())

    def test_macos_final_qualification_bridges_unsigned_build_to_signed_dmg(self):
        lock, receipt, package_root, release_root, document = self.make_release_fixture(
            target="macos-arm64", layout="dmg"
        )
        helper = package_root / document["helper"]["path"]
        unsigned_hash = document["helper"]["sha256"]
        helper.write_bytes(b"fixture signed complete adb executable\n")
        helper.chmod(0o755)
        signed_hash = hashlib.sha256(helper.read_bytes()).hexdigest()
        smoke = self.write_smoke_receipt(helper)
        smoke_document = json.loads(smoke.read_text(encoding="utf-8"))
        smoke_document["helper_sha256"] = signed_hash
        smoke.write_text(json.dumps(smoke_document), encoding="utf-8")
        dmg = self.write_file("package/klogg.dmg", b"signed and notarized dmg\n")
        dmg_hash = hashlib.sha256(dmg.read_bytes()).hexdigest()
        signing = self.root / "signing.json"
        signing.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "receipt_kind": "signing",
                    "status": "passed",
                    "target": "macos-arm64",
                    "source_helper_sha256": unsigned_hash,
                    "signed_helper_sha256": signed_hash,
                    "package_sha256": dmg_hash,
                }
            ),
            encoding="utf-8",
        )
        notarization = self.root / "notarization.json"
        notarization.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "receipt_kind": "notarization",
                    "status": "passed",
                    "target": "macos-arm64",
                    "signed_helper_sha256": signed_hash,
                    "package_sha256": dmg_hash,
                    "submission_id": "fixture-submission",
                }
            ),
            encoding="utf-8",
        )
        qualification = self.root / "mac-package-verification.json"

        result = self.run_verifier(
            lock,
            receipt,
            package_root,
            release_root,
            "--binary-smoke-receipt",
            str(smoke),
            "--package-target",
            "macos-arm64-dmg",
            "--signing-receipt",
            str(signing),
            "--notarization-receipt",
            str(notarization),
            "--package-file",
            str(dmg),
            "--package-verification-receipt",
            str(qualification),
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        evidence = json.loads(qualification.read_text(encoding="utf-8"))
        self.assertIs(evidence.get("release_qualified"), True)
        self.assertEqual(evidence.get("source_helper_sha256"), unsigned_hash)
        self.assertEqual(evidence.get("helper_sha256"), signed_hash)
        self.assertEqual(evidence.get("package_sha256"), dmg_hash)
        self.assertEqual(evidence.get("packages"), [{"name": dmg.name, "sha256": dmg_hash}])

    def test_verifier_records_every_final_package_hash(self):
        lock, receipt, package_root, release_root, document = self.make_release_fixture(
            target="windows-x86_64", layout="nsis"
        )
        receipt.write_text(json.dumps(document), encoding="utf-8")
        helper = package_root / document["helper"]["path"]
        smoke = self.write_smoke_receipt(helper)
        portable = self.write_file("package/klogg-portable.zip", b"portable\n")
        installer = self.write_file("package/klogg-setup.exe", b"installer\n")
        qualification = self.root / "windows-package-verification.json"

        result = self.run_verifier(
            lock,
            receipt,
            package_root,
            release_root,
            "--binary-smoke-receipt",
            str(smoke),
            "--package-target",
            "windows-x86_64",
            "--package-file",
            str(portable),
            "--package-file",
            str(installer),
            "--package-verification-receipt",
            str(qualification),
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        evidence = json.loads(qualification.read_text(encoding="utf-8"))
        self.assertIsNone(evidence.get("package_sha256"))
        self.assertEqual(
            evidence.get("packages"),
            [
                {"name": portable.name, "sha256": hashlib.sha256(portable.read_bytes()).hexdigest()},
                {"name": installer.name, "sha256": hashlib.sha256(installer.read_bytes()).hexdigest()},
            ],
        )

    def test_verifier_rejects_glibc_requirement_newer_than_locked_baseline(self):
        lock, receipt, package_root, release_root, document = self.make_release_fixture()
        document["binary_verification"]["glibc_maximum_required"] = "2.36"
        receipt.write_text(json.dumps(document), encoding="utf-8")

        result = self.run_verifier(lock, receipt, package_root, release_root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("glibc", (result.stdout + result.stderr).lower())

    def test_verifier_rejects_glibc_requirement_newer_than_package_maximum(self):
        lock, receipt, package_root, release_root, _ = self.make_release_fixture()
        result = self.run_verifier(
            lock,
            receipt,
            package_root,
            release_root,
            "--maximum-glibc-version",
            "2.31",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("package maximum", (result.stdout + result.stderr).lower())


class AdbHelperEnvelopeContractTest(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tempdir.name)
        self.fixture_index = 0

    def tearDown(self):
        self.tempdir.cleanup()

    def make_envelope(self):
        self.fixture_index += 1
        artifact = self.root / f"artifact-{self.fixture_index}"
        helper = artifact / "helpers/adb"
        runtime = artifact / "helpers/libusb-1.0.so.0"
        helper.parent.mkdir(parents=True)
        helper.write_bytes(b"adb\n")
        helper.chmod(0o755)
        runtime.write_bytes(b"libusb\n")
        lock = {
            "schema_version": 1,
            "targets": {
                "linux-x86_64": {
                    "arch": "x86_64",
                    "usb": {"runtime_files": [runtime.name]},
                }
            },
        }
        lock_path = self.root / "lock.json"
        lock_path.write_text(json.dumps(lock), encoding="utf-8")
        helper_hash = hashlib.sha256(helper.read_bytes()).hexdigest()
        receipt = {
            "schema_version": 1,
            "receipt_kind": "binary-build",
            "target": "linux-x86_64",
            "lock_sha256": hashlib.sha256(lock_path.read_bytes()).hexdigest(),
            "helper": {"sha256": helper_hash},
        }
        smoke = {
            "schema_version": 1,
            "receipt_kind": "binary-smoke",
            "helper_sha256": helper_hash,
        }
        package = {
            "schema_version": 1,
            "receipt_kind": "package-verification",
            "target": "linux-x86_64",
            "helper_sha256": helper_hash,
        }
        documents = {
            "receipt.json": receipt,
            "smoke.json": smoke,
            "package-smoke.json": smoke,
            "package-verification.json": package,
        }
        for name, document in documents.items():
            (artifact / name).write_text(
                json.dumps(document, sort_keys=True) + "\n", encoding="utf-8"
            )
        members = [
            helper,
            runtime,
            *(artifact / name for name in documents),
        ]
        lines = [
            f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(artifact).as_posix()}"
            for path in sorted(members)
        ]
        (artifact / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")
        return lock_path, artifact

    def run_envelope(self, lock, artifact, target="linux-x86_64"):
        self.assertTrue(ENVELOPE_SCRIPT.is_file(), f"missing envelope verifier: {ENVELOPE_SCRIPT}")
        return subprocess.run(
            [
                sys.executable,
                str(ENVELOPE_SCRIPT),
                "--lock",
                str(lock),
                "--artifact-root",
                str(artifact),
                "--expected-target",
                target,
            ],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def test_envelope_verifier_accepts_complete_target_bound_checksum_set(self):
        lock, artifact = self.make_envelope()
        result = self.run_envelope(lock, artifact)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_envelope_verifier_rejects_tampering_unlisted_files_and_target_mismatch(self):
        for scenario in ("tampered", "unlisted", "target"):
            with self.subTest(scenario=scenario):
                lock, artifact = self.make_envelope()
                if scenario == "tampered":
                    (artifact / "helpers/adb").write_bytes(b"tampered\n")
                elif scenario == "unlisted":
                    (artifact / "helpers/extra.dll").write_bytes(b"unlisted\n")
                result = self.run_envelope(
                    lock,
                    artifact,
                    "linux-arm64" if scenario == "target" else "linux-x86_64",
                )
                self.assertNotEqual(result.returncode, 0)


class AdbHelperSmokeContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("smoke_adb_helper", SMOKE_SCRIPT)
        cls.smoke_module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(cls.smoke_module)

    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tempdir.name)

    def tearDown(self):
        self.tempdir.cleanup()

    def make_fake_adb(
        self,
        complete_client: bool,
        server_connection_limit: int | None = None,
        *,
        stall_after_connection_limit: bool = False,
        response_delays: tuple[float, ...] = (),
    ) -> pathlib.Path:
        path = self.root / ("complete-adb" if complete_client else "server-only-adb")
        source = textwrap.dedent(
            f"""\
            #!{sys.executable}
            import json
            import os
            import pathlib
            import signal
            import socket
            import sys
            import time

            COMPLETE_CLIENT = {complete_client!r}
            SERVER_CONNECTION_LIMIT = {server_connection_limit!r}
            STALL_AFTER_CONNECTION_LIMIT = {stall_after_connection_limit!r}
            RESPONSE_DELAYS = {response_delays!r}
            running = True

            def stop(*_args):
                global running
                running = False

            signal.signal(signal.SIGTERM, stop)
            signal.signal(signal.SIGINT, stop)

            def recv_exact(connection, size):
                payload = b""
                while len(payload) < size:
                    chunk = connection.recv(size - len(payload))
                    if not chunk:
                        raise RuntimeError("short smart-socket request")
                    payload += chunk
                return payload

            args = sys.argv[1:]
            if args == ["version"]:
                print("Android Debug Bridge version 1.0.41")
                raise SystemExit(0)
            if args == ["help"]:
                if not COMPLETE_CLIENT:
                    print("server-only fork", file=sys.stderr)
                    raise SystemExit(9)
                print("adb help: devices, shell, logcat")
                raise SystemExit(0)
            conflicting = [
                name
                for name in {PRODUCTION_SERVER_SCRUBBED_ENVIRONMENT!r}
                if name in os.environ
            ]
            server_socket = os.environ.get("ADB_SERVER_SOCKET", "")
            observation_path = os.environ.get("KLOGG_ADB_SMOKE_OBSERVATION")
            if observation_path:
                pathlib.Path(observation_path).write_text(
                    json.dumps(
                        dict(
                            arguments=args,
                            server_socket=server_socket,
                            conflicting_environment=conflicting,
                            adb_trace=os.environ.get("ADB_TRACE"),
                        ),
                        sort_keys=True,
                    ),
                    encoding="utf-8",
                )
            if args != ["server", "nodaemon"]:
                print("fatal: expected exact production server arguments", file=sys.stderr)
                raise SystemExit(6)

            socket_parts = server_socket.split(":")
            if (
                len(socket_parts) != 2
                or socket_parts[0] != "tcp"
                or not socket_parts[1].isdigit()
            ):
                print("fatal: expected ADB_SERVER_SOCKET=tcp:<port>", file=sys.stderr)
                raise SystemExit(6)
            if conflicting:
                print(f"fatal: conflicting endpoint environment: {{conflicting}}", file=sys.stderr)
                raise SystemExit(6)
            port = int(socket_parts[1])

            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", port))
            listener.listen(1)
            listener.settimeout(0.1)
            accepted_connections = 0
            while running:
                try:
                    connection, _ = listener.accept()
                except socket.timeout:
                    continue
                with connection:
                    header = recv_exact(connection, 4)
                    size = int(header.decode("ascii"), 16)
                    request = recv_exact(connection, size)
                    if accepted_connections < len(RESPONSE_DELAYS):
                        time.sleep(RESPONSE_DELAYS[accepted_connections])
                    if request == b"host:version":
                        connection.sendall(b"OKAY00040029")
                    else:
                        connection.sendall(b"FAIL000bunsupported")
                accepted_connections += 1
                if (
                    SERVER_CONNECTION_LIMIT is not None
                    and accepted_connections >= SERVER_CONNECTION_LIMIT
                ):
                    if STALL_AFTER_CONNECTION_LIMIT:
                        listener.close()
                        while running:
                            time.sleep(0.05)
                        raise SystemExit(0)
                    break
            listener.close()
            """
        )
        path.write_text(source, encoding="utf-8")
        path.chmod(0o755)
        return path

    def run_smoke(self, adb: pathlib.Path, report: pathlib.Path):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            return_code = self.smoke_module.smoke_adb(
                adb=adb,
                port=0,
                timeout_seconds=5,
                json_output=report,
                command_prefix=[sys.executable, str(adb)],
            )
        return subprocess.CompletedProcess([], return_code, output.getvalue(), "")

    def assert_reported_endpoint_closed(self, report: dict):
        endpoint = report.get("server_endpoint")
        if not isinstance(endpoint, str):
            return
        port = int(endpoint.rsplit(":", 1)[1])
        with self.assertRaises(OSError):
            socket.create_connection(("127.0.0.1", port), timeout=0.25)

    def test_smoke_probes_exact_production_server_invocation_and_cleanup(self):
        adb = self.make_fake_adb(complete_client=True)
        report_path = self.root / "smoke.json"
        observation_path = self.root / "server-invocation.json"
        hostile_environment = {
            "KLOGG_ADB_SMOKE_OBSERVATION": str(observation_path),
            "ADB_SERVER_SOCKET": "tcp:198.51.100.7:7000",
            "ADB_VENDOR_KEYS": "/untrusted/adbkey",
            "ADB_SERVER_PORT": "7001",
            "ANDROID_ADB_SERVER_PORT": "7002",
            "ANDROID_ADB_SERVER_ADDRESS": "198.51.100.8",
            "ADB_TRACE": "sockets",
        }
        with mock.patch.dict(os.environ, hostile_environment, clear=False):
            result = self.run_smoke(adb, report_path)
        observation = json.loads(observation_path.read_text(encoding="utf-8"))
        with self.subTest(contract="arguments"):
            self.assertEqual(observation.get("arguments"), ["server", "nodaemon"])
        with self.subTest(contract="socket"):
            self.assertRegex(observation.get("server_socket", ""), r"^tcp:[0-9]+$")
        with self.subTest(contract="scrubbed-environment"):
            self.assertEqual(observation.get("conflicting_environment"), [])
        with self.subTest(contract="unrelated-adb-environment"):
            self.assertEqual(observation.get("adb_trace"), "sockets")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(
            report.get("passed_probes"),
            [
                "version",
                "complete-client",
                "loopback-private-server",
                "smart-socket-host-version",
                "no-lingering-process",
            ],
        )
        port = int(report["server_endpoint"].rsplit(":", 1)[1])
        self.assertEqual(report.get("required_probe"), production_server_invocation_probe(port))
        self.assertIsInstance(report.get("server_pid"), int)
        self.assert_reported_endpoint_closed(report)

    def test_smoke_gives_the_second_probe_the_configured_timeout_budget(self):
        adb = self.make_fake_adb(
            complete_client=True,
            response_delays=(0.0, 0.35, 0.0),
        )
        report_path = self.root / "slow-second-probe.json"

        result = self.run_smoke(adb, report_path)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(report.get("host_version"), "0029")
        self.assertEqual(report.get("stability_host_version"), "0029")

    def test_smoke_gives_the_final_probe_the_configured_timeout_budget(self):
        adb = self.make_fake_adb(
            complete_client=True,
            response_delays=(0.0, 0.0, 0.35),
        )
        report_path = self.root / "slow-final-probe.json"

        result = self.run_smoke(adb, report_path)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(report.get("host_version"), "0029")
        self.assertEqual(report.get("stability_host_version"), "0029")

    def test_smoke_rejects_server_that_exits_after_one_successful_probe(self):
        adb = self.make_fake_adb(complete_client=True, server_connection_limit=1)
        report_path = self.root / "one-shot-smoke.json"

        result = self.run_smoke(adb, report_path)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("remain available", (result.stdout + result.stderr).lower())
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assert_reported_endpoint_closed(report)

    def test_smoke_rejects_server_that_exits_after_two_successful_probes(self):
        adb = self.make_fake_adb(complete_client=True, server_connection_limit=2)
        report_path = self.root / "two-shot-smoke.json"

        result = self.run_smoke(adb, report_path)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("remain stable", (result.stdout + result.stderr).lower())
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assert_reported_endpoint_closed(report)

    def test_smoke_rejects_server_that_stalls_during_the_stability_window(self):
        adb = self.make_fake_adb(
            complete_client=True,
            server_connection_limit=2,
            stall_after_connection_limit=True,
        )
        report_path = self.root / "stalled-smoke.json"

        result = self.run_smoke(adb, report_path)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("remain responsive", (result.stdout + result.stderr).lower())
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assert_reported_endpoint_closed(report)

    def test_smoke_rejects_server_only_fork_and_still_leaves_no_process(self):
        adb = self.make_fake_adb(complete_client=False)
        report_path = self.root / "smoke.json"
        result = self.run_smoke(adb, report_path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("complete", (result.stdout + result.stderr).lower())
        if report_path.is_file():
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assert_reported_endpoint_closed(report)


if __name__ == "__main__":
    unittest.main()
