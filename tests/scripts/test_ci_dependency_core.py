"""Binary-only ADB and iOS cores are independently locked and deterministic."""

import hashlib
import io
import json
import pathlib
import sys
import tarfile
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_dependency_core as core

KEY = "a" * 64
ADB_SIDECARS = {
    "linux-x86_64": ("libusb-1.0.so.0",),
    "linux-arm64": ("libusb-1.0.so.0",),
    "windows-x86_64": ("AdbWinApi.dll", "AdbWinUsbApi.dll", "libusb-1.0.dll"),
    "macos-x86_64": (),
    "macos-arm64": (),
}


class DependencyCoreTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.source = self.root / "staged"
        self.source.mkdir()
        self.archive = self.root / "core.tar.gz"
        self.stage_adb("linux-x86_64")

    def write(self, name, content, mode=0o644):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(mode)
        return path

    def stage_adb(self, target):
        self.write("helpers/adb.exe" if target.startswith("windows-") else "helpers/adb",
                   b"binary adb", 0o755)
        for name in ADB_SIDECARS[target]:
            self.write("helpers/" + name, b"private runtime closure")

    def stage_ios(self):
        for path in self.source.rglob("*"):
            if path.is_file():
                path.unlink()
        (self.source / "helpers").rmdir()
        self.write("lib/libfoo.1.dylib", b"dylib binary", 0o755)
        (self.source / "lib/libfoo.dylib").symlink_to("libfoo.1.dylib")

    def package(self, *, component="adb-helper", target="linux-x86_64"):
        return core.package_core(self.source, self.archive, component=component,
                                 target=target, core_identity=KEY)

    def verify(self, identity, *, component="adb-helper", target="linux-x86_64"):
        return core.verify_core(self.archive, component=component, target=target,
                                core_identity=KEY, expected_sha256=identity["sha256"],
                                expected_size=identity["size"])

    def test_qualification_receipt_and_release_version_changes_cannot_mutate_binary_core(self):
        qualification = self.root / "qualification"
        qualification.mkdir()
        receipts = ("receipt.json", "smoke.json", "package-smoke.json",
                    "package-verification.json", "SHA256SUMS")
        for name in receipts:
            (qualification / name).write_text("policy v1\n")
        first = self.package()
        raw = self.archive.read_bytes()
        self.assertEqual(self.verify(first)["target"], "linux-x86_64")
        for name in receipts:
            (qualification / name).write_text("policy v2\n")
        (qualification / "adb-helper-source-offer.txt").write_text(
            "version 04.05.06; https://github.com/ZEACENT/klogg/releases/tag/v04.05.06"
        )
        (self.source / "helpers/adb").touch()
        self.archive.unlink()
        self.assertEqual(first, self.package())
        self.assertEqual(raw, self.archive.read_bytes())
        with tarfile.open(self.archive, "r:gz") as archive:
            self.assertEqual([member.name for member in archive],
                             [core.MANIFEST, "helpers/adb", "helpers/libusb-1.0.so.0"])

    def test_changed_binary_or_private_runtime_invalidates_external_blob_identity(self):
        original = self.package()
        for name in ("helpers/adb", "helpers/libusb-1.0.so.0"):
            self.archive.unlink()
            self.write(name, b"new bytes", 0o755 if name == "helpers/adb" else 0o644)
            changed = self.package()
            self.assertNotEqual(original["sha256"], changed["sha256"])
            with self.assertRaises(core.CoreError):
                self.verify(original)
            original = changed

    def test_all_seven_targets_accept_only_their_binary_closures(self):
        lock = json.loads((ROOT / "packaging/adb/adb-helper.lock.json").read_text())
        self.assertEqual(set(lock["targets"]), set(ADB_SIDECARS))
        for target, sidecars in ADB_SIDECARS.items():
            self.assertEqual(tuple(lock["targets"][target]["usb"].get("runtime_files", [])),
                             sidecars)
        for target in ADB_SIDECARS:
            with self.subTest(target=target):
                if self.archive.exists():
                    self.archive.unlink()
                for path in (self.source / "helpers").iterdir():
                    path.unlink()
                self.stage_adb(target)
                result = self.package(target=target)
                self.verify(result, target=target)
        for target in ("arm64", "x86_64"):
            with self.subTest(target=target):
                self.archive.unlink()
                if (self.source / "helpers").exists():
                    for path in (self.source / "helpers").iterdir():
                        path.unlink()
                    (self.source / "helpers").rmdir()
                if not (self.source / "lib").exists():
                    self.write("lib/libfoo.1.dylib", b"dylib binary", 0o755)
                    (self.source / "lib/libfoo.dylib").symlink_to("libfoo.1.dylib")
                result = self.package(component="ios-native", target=target)
                self.verify(result, component="ios-native", target=target)

    def test_windows_host_permission_bits_are_normalized_in_archive(self):
        for path in (self.source / "helpers").iterdir():
            path.unlink()
        self.stage_adb("windows-x86_64")
        (self.source / "helpers/adb.exe").chmod(0o777)
        for name in ADB_SIDECARS["windows-x86_64"]:
            (self.source / "helpers" / name).chmod(0o666)
        result = self.package(target="windows-x86_64")
        self.verify(result, target="windows-x86_64")
        with tarfile.open(self.archive, "r:gz") as archive:
            self.assertEqual(archive.getmember("helpers/adb.exe").mode, 0o755)
            for name in ADB_SIDECARS["windows-x86_64"]:
                self.assertEqual(archive.getmember("helpers/" + name).mode, 0o644)

    def test_wrong_target_and_core_identity_are_rejected(self):
        identity = self.package()
        with self.assertRaises(core.CoreError):
            self.verify(identity, target="linux-arm64")
        with self.assertRaises(core.CoreError):
            core.verify_core(self.archive, component="adb-helper", target="linux-x86_64",
                             core_identity="b" * 64, expected_sha256=identity["sha256"],
                             expected_size=identity["size"])
        with self.assertRaises(core.CoreError):
            self.package(target="unsupported-target")

    def test_qualification_and_overlay_files_inside_core_stage_are_rejected(self):
        for name in ("receipt.json", "smoke.json", "package-smoke.json",
                     "package-verification.json", "SHA256SUMS",
                     "adb-helper-overlay-receipt.json", "ios-native-source-offer.txt"):
            path = self.write(name, b"qualification material")
            with self.subTest(name=name), self.assertRaises(core.CoreError):
                self.package()
            path.unlink()

    def test_archive_cannot_be_written_inside_staged_tree(self):
        with self.assertRaises(core.CoreError):
            core.package_core(self.source, self.source / "core.tar.gz",
                              component="adb-helper", target="linux-x86_64",
                              core_identity=KEY)
        self.assertFalse((self.source / "core.tar.gz").exists())

    def test_unlisted_empty_directories_are_not_ignored(self):
        (self.source / "overlay").mkdir()
        with self.assertRaises(core.CoreError):
            self.package()

    def test_symlink_traversal_and_file_type_injection_are_rejected(self):
        self.stage_ios()
        alias = self.source / "lib/libfoo.dylib"
        alias.unlink()
        alias.symlink_to("../../secret")
        with self.assertRaises(core.CoreError):
            self.package(component="ios-native", target="arm64")
        alias.unlink()
        alias.symlink_to("libfoo.1.dylib")
        identity = self.package(component="ios-native", target="arm64")
        self._rewrite_archive(lambda member: setattr(member, "linkname", "../../secret"),
                              name="lib/libfoo.dylib")
        with self.assertRaises(core.CoreError):
            self.verify(self._current_identity(), component="ios-native", target="arm64")
        with self.assertRaises(core.CoreError):
            self.verify(identity, component="ios-native", target="arm64")

    def test_injected_overlay_member_is_rejected_even_if_blob_digest_is_relocked(self):
        self.package()
        self._rewrite_archive(None, extra=("ios-native-source-offer.txt", b"release URL"))
        with self.assertRaises(core.CoreError):
            self.verify(self._current_identity())

    def test_archive_substitution_needs_an_external_digest_and_size(self):
        identity = self.package()
        self.archive.write_bytes(self.archive.read_bytes() + b"substitution")
        with self.assertRaises(core.CoreError):
            self.verify(identity)
        with self.assertRaises(core.CoreError):
            core.verify_core(self.archive, component="adb-helper", target="linux-x86_64",
                             core_identity=KEY, expected_sha256=None,
                             expected_size=identity["size"])

    def test_ios_core_contains_only_dylibs_even_if_qualification_changes(self):
        self.stage_ios()
        qualification = self.root / "qualification"
        qualification.mkdir()
        for name in ("ios-native-build-receipt.json", "ios-native-source-set-receipt.json",
                     "ios-native-legal-receipt.json", "ios-native-sbom.spdx.json",
                     "NOTICE-ios-native.txt", "licenses/foo-LICENSE"):
            path = qualification / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("legal qualification 1\n")
        result = self.package(component="ios-native", target="arm64")
        raw = self.archive.read_bytes()
        for path in qualification.rglob("*"):
            if path.is_file():
                path.write_text("legal qualification 2\n")
        self.archive.unlink()
        self.assertEqual(result, self.package(component="ios-native", target="arm64"))
        self.assertEqual(raw, self.archive.read_bytes())
        for name in ("ios-native-build-receipt.json", "ios-native-legal-receipt.json",
                     "ios-native-sbom.spdx.json", "NOTICE-ios-native.txt"):
            self.archive.unlink(missing_ok=True)
            path = self.write(name, b"legal material")
            with self.subTest(name=name), self.assertRaises(core.CoreError):
                self.package(component="ios-native", target="arm64")
            path.unlink()

    def _current_identity(self):
        data = self.archive.read_bytes()
        return {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}

    def _rewrite_archive(self, mutate, *, name=None, extra=None):
        entries = []
        with tarfile.open(self.archive, "r:gz") as tar:
            for member in tar:
                entries.append((member, tar.extractfile(member).read() if member.isfile() else None))
        with tarfile.open(self.archive, "w:gz") as tar:
            for member, data in entries:
                if member.name == name and mutate:
                    mutate(member)
                tar.addfile(member, io.BytesIO(data) if data is not None else None)
            if extra:
                member = tarfile.TarInfo(extra[0])
                member.size = len(extra[1])
                tar.addfile(member, io.BytesIO(extra[1]))


if __name__ == "__main__":
    unittest.main()
