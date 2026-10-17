"""Binary-only ADB and iOS cores are independently locked and deterministic."""

import contextlib
import hashlib
import io
import json
import os
import pathlib
import stat
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

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


def fixture_platform():
    return os.name


class DependencyCoreTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name).resolve(strict=True)
        self.source = self.root / "staged"
        self.source.mkdir()
        self.archive = self.root / "core.tar.gz"
        self.fixture_modes = {}
        self.fixture_links = {}
        self.default_target = "windows-x86_64" if fixture_platform() == "nt" else "linux-x86_64"
        self.helper_name = "helpers/adb.exe" if self.default_target.startswith("windows-") else "helpers/adb"
        self.runtime_name = "helpers/" + ADB_SIDECARS[self.default_target][0]
        self.binary_names = {self.helper_name} | {
            "helpers/" + name for name in ADB_SIDECARS[self.default_target]
        }
        self.stage_adb(self.default_target)

    def private_parent(self, name="private"):
        if fixture_platform() == "nt":
            return core.windows_security.create_private_parent(self.root, name)
        parent = self.root / name
        parent.mkdir(mode=0o700)
        return parent

    def write(self, name, content, mode=0o644):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(mode)
        self.fixture_modes[path] = mode
        self.fixture_links.pop(path, None)
        return path

    def symlink(self, path, target):
        if fixture_platform() == "nt":
            # Foreign archive-format model only; no Windows symlink support claim.
            path.write_bytes(b"logical foreign-format symlink fixture")
            self.fixture_links[path] = target
        else:
            path.symlink_to(target)

    @contextlib.contextmanager
    def foreign_format_metadata(self):
        if fixture_platform() != "nt":
            yield
            return
        # NTFS cannot represent POSIX mode bits. Only foreign archive-format
        # fixtures inject those logical attributes; native Windows staging never does.
        original_lstat = pathlib.Path.lstat
        original_readlink = os.readlink

        def lstat(path, *args, **kwargs):
            info = original_lstat(path, *args, **kwargs)
            fields = list(info)
            if path in self.fixture_links:
                fields[0] = stat.S_IFLNK | 0o777
                return os.stat_result(fields)
            if path in self.fixture_modes and stat.S_ISREG(info.st_mode):
                fields[0] = info.st_mode & ~0o777 | self.fixture_modes[path]
                return os.stat_result(fields)
            return info

        def readlink(path, *args, **kwargs):
            link = self.fixture_links.get(pathlib.Path(path))
            return link if link is not None else original_readlink(path, *args, **kwargs)

        with mock.patch.object(pathlib.Path, "lstat", lstat), \
                mock.patch.object(os, "readlink", readlink):
            yield

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
        self.symlink(self.source / "lib/libfoo.dylib", "libfoo.1.dylib")

    def package(self, *, component="adb-helper", target=None):
        target = self.default_target if target is None else target
        boundary = (self.foreign_format_metadata() if component == "ios-native"
                    or not target.startswith("windows-") else contextlib.nullcontext())
        with boundary:
            return core.package_core(self.source, self.archive, component=component,
                                     target=target, core_identity=KEY)

    def verify(self, identity, *, component="adb-helper", target=None):
        return core.verify_core(self.archive, component=component,
                                target=self.default_target if target is None else target,
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
        self.assertEqual(self.verify(first)["target"], self.default_target)
        for name in receipts:
            (qualification / name).write_text("policy v2\n")
        (qualification / "adb-helper-source-offer.txt").write_text(
            "version 04.05.06; https://github.com/ZEACENT/klogg/releases/tag/v04.05.06"
        )
        (self.source / self.helper_name).touch()
        self.archive.unlink()
        self.assertEqual(first, self.package())
        self.assertEqual(raw, self.archive.read_bytes())
        with tarfile.open(self.archive, "r:gz") as archive:
            self.assertEqual([member.name for member in archive],
                             [core.MANIFEST, *sorted(self.binary_names)])

    def test_changed_binary_or_private_runtime_invalidates_external_blob_identity(self):
        original = self.package()
        for name in (self.helper_name, self.runtime_name):
            self.archive.unlink()
            self.write(name, b"new bytes", 0o755 if name == self.helper_name else 0o644)
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
                    self.symlink(self.source / "lib/libfoo.dylib", "libfoo.1.dylib")
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

    def test_verified_binary_core_stages_only_allowlisted_files_in_private_destination(self):
        identity = self.package()
        parent = self.private_parent()
        destination = parent / "core"
        staged = core.stage_core(self.archive, destination, component="adb-helper",
                                 target=self.default_target, core_identity=KEY,
                                 expected_sha256=identity["sha256"], expected_size=identity["size"])
        self.assertEqual(staged["sha256"], identity["sha256"])
        self.assertEqual((destination / self.helper_name).read_bytes(), b"binary adb")
        self.assertEqual((destination / self.runtime_name).read_bytes(),
                         b"private runtime closure")
        self.assertEqual({path.relative_to(destination).as_posix() for path in destination.rglob("*")},
                         {"helpers"} | self.binary_names)
        with self.assertRaises(core.CoreError):
            core.stage_core(self.archive, destination, component="adb-helper",
                            target=self.default_target, core_identity=KEY,
                            expected_sha256=identity["sha256"], expected_size=identity["size"])
        self.assertEqual((destination / self.helper_name).read_bytes(), b"binary adb")

    @unittest.skipUnless(os.name == "posix", "native iOS dylib staging requires POSIX modes and symlinks")
    def test_verified_ios_core_stages_only_direct_internal_dylib_alias(self):
        self.stage_ios()
        identity = self.package(component="ios-native", target="arm64")
        parent = self.private_parent()
        destination = parent / "core"
        core.stage_core(self.archive, destination, component="ios-native", target="arm64",
                        core_identity=KEY, expected_sha256=identity["sha256"],
                        expected_size=identity["size"])
        self.assertEqual((destination / "lib/libfoo.1.dylib").read_bytes(), b"dylib binary")
        alias = destination / "lib/libfoo.dylib"
        self.assertTrue(alias.is_symlink())
        self.assertEqual(os.readlink(alias), "libfoo.1.dylib")
        self.assertFalse((destination / core.MANIFEST).exists())

    def test_staging_rejects_untrusted_bytes_or_writable_destination_parent(self):
        identity = self.package()
        private = self.private_parent()
        destination = private / "core"
        with self.assertRaises(core.CoreError):
            core.stage_core(self.archive, destination, component="adb-helper",
                            target=self.default_target, core_identity=KEY,
                            expected_sha256="b" * 64, expected_size=identity["size"])
        self.assertFalse(destination.exists())
        self.assertEqual(list(private.iterdir()), [])
        public = self.root / "public"
        public.mkdir(mode=0o755)
        with self.assertRaises(core.CoreError):
            core.stage_core(self.archive, public / "core", component="adb-helper",
                            target=self.default_target, core_identity=KEY,
                            expected_sha256=identity["sha256"], expected_size=identity["size"])
        self.assertFalse((public / "core").exists())

    def test_atomic_core_publish_cannot_replace_racing_empty_directory(self):
        parent = self.private_parent()
        source = parent / "pending"
        destination = parent / "core"
        source.mkdir()
        destination.mkdir()
        inode = destination.stat().st_ino
        with self.assertRaises((OSError, core.CoreError)):
            core._publish_no_replace(source, destination)
        self.assertEqual(destination.stat().st_ino, inode)
        self.assertTrue(source.is_dir())

    def test_staging_rejects_unverified_windows_acl_or_foreign_parent_owner(self):
        parent = self.root / "private"
        parent.mkdir(mode=0o700)
        with self.assertRaises(core.CoreError):
            core._require_private_parent(parent, platform="nt")
        if fixture_platform() == "posix":
            with mock.patch.object(core.os, "geteuid", return_value=os.geteuid() + 1), \
                    self.assertRaises(core.CoreError):
                core._require_private_parent(parent, platform="posix")
        else:
            # A modeled foreign-owner policy check; real native ownership/token
            # inspection is covered by NativeDirectorySecurityTest separately.
            current = "S-1-5-21-100-200-300-1001"
            foreign = "S-1-5-21-100-200-300-1002"
            descriptor = core.windows_security.DirectorySecurity(
                foreign, True, True,
                (core.windows_security.AccessEntry(0, 3,
                 core.windows_security.FILE_ALL_ACCESS, foreign),))
            with self.assertRaises(core.windows_security.WindowsSecurityError):
                core.windows_security.validate_directory_security(descriptor, current, private=True)

    def test_staging_race_cannot_replace_newly_created_destination(self):
        identity = self.package()
        parent = self.private_parent()
        destination = parent / "core"
        publish = core._publish_no_replace
        raced_inode = []

        def race(source, target):
            target.mkdir()
            raced_inode.append(target.stat().st_ino)
            publish(source, target)

        with mock.patch.object(core, "_publish_no_replace", side_effect=race), \
                self.assertRaises(core.CoreError):
            core.stage_core(self.archive, destination, component="adb-helper",
                            target=self.default_target, core_identity=KEY,
                            expected_sha256=identity["sha256"], expected_size=identity["size"])
        self.assertEqual(destination.stat().st_ino, raced_inode[0])
        self.assertEqual(list(destination.iterdir()), [])
        self.assertEqual(list(parent.iterdir()), [destination])

    def test_staging_rechecks_private_snapshot_before_publishing(self):
        identity = self.package()
        parent = self.private_parent()
        destination = parent / "core"
        extract = core.safe_extract

        def changed_after_extraction(snapshot, stage):
            extract(snapshot, stage)
            with snapshot.open("ab") as output:
                output.write(b"changed after verification")

        with mock.patch.object(core, "safe_extract", side_effect=changed_after_extraction), \
                self.assertRaises(core.CoreError):
            core.stage_core(self.archive, destination, component="adb-helper",
                            target=self.default_target, core_identity=KEY,
                            expected_sha256=identity["sha256"], expected_size=identity["size"])
        self.assertFalse(destination.exists())
        self.assertEqual(list(parent.iterdir()), [])

    def test_input_archive_must_be_outside_private_transaction_parent(self):
        identity = self.package()
        parent = self.private_parent()
        internal = parent / "download.tar.gz"
        internal.write_bytes(self.archive.read_bytes())
        with self.assertRaisesRegex(core.CoreError, "outside private"):
            core.stage_core(internal, parent / "core", component="adb-helper",
                            target=self.default_target, core_identity=KEY,
                            expected_sha256=identity["sha256"], expected_size=identity["size"])
        self.assertEqual(list(parent.iterdir()), [internal])

    def test_staging_keeps_injected_namespace_lease_through_cleanup_and_publish(self):
        # This is a transaction-lifetime model, not native Windows security proof.
        identity = self.package()
        parent = self.private_parent()
        destination = parent / "core"
        events = []

        class Lease:
            def verify(inner):
                events.append("verify")
                self.assertNotIn("released", events)

        @contextlib.contextmanager
        def boundary(actual):
            self.assertEqual(actual, parent)
            core._require_private_parent(actual)
            events.append("acquired")
            try:
                yield Lease()
            finally:
                self.assertEqual(list(parent.iterdir()), [destination])
                events.append("released")

        original = core._publish_no_replace

        def publish(source, target):
            self.assertEqual(events[-1], "verify")
            self.assertNotIn("released", events)
            events.append("published")
            original(source, target)

        with mock.patch.object(core, "_private_parent_boundary", side_effect=boundary), \
                mock.patch.object(core, "_publish_no_replace", side_effect=publish):
            core.stage_core(self.archive, destination, component="adb-helper",
                            target=self.default_target, core_identity=KEY,
                            expected_sha256=identity["sha256"], expected_size=identity["size"])
        self.assertEqual(events, ["acquired", "verify", "verify", "published", "released"])

    def test_namespace_revalidation_failure_prevents_publish_and_retains_first_error(self):
        identity = self.package()
        parent = self.private_parent()
        destination = parent / "core"
        events = []

        class Lease:
            def verify(inner):
                events.append("verify")
                if len(events) == 2:
                    raise core.windows_security.WindowsSecurityError("identity changed")

        @contextlib.contextmanager
        def boundary(actual):
            self.assertEqual(actual, parent)
            try:
                yield Lease()
            finally:
                self.assertEqual(list(parent.iterdir()), [])
                events.append("released")

        with mock.patch.object(core, "_private_parent_boundary", side_effect=boundary), \
                mock.patch.object(core, "_publish_no_replace") as publish, \
                self.assertRaisesRegex(core.windows_security.WindowsSecurityError, "identity changed"):
            core.stage_core(self.archive, destination, component="adb-helper",
                            target=self.default_target, core_identity=KEY,
                            expected_sha256=identity["sha256"], expected_size=identity["size"])
        publish.assert_not_called()
        self.assertEqual(events, ["verify", "verify", "released"])
        self.assertFalse(destination.exists())

    @unittest.skipUnless(os.name == "nt", "requires real Windows private directory security")
    def test_native_windows_binary_core_staging_and_collision(self):
        for path in (self.source / "helpers").iterdir():
            path.unlink()
        self.stage_adb("windows-x86_64")
        identity = self.package(target="windows-x86_64")
        parent = core.windows_security.create_private_parent(self.root, "native-private")
        destination = parent / "core"
        staged = core.stage_core(self.archive, destination, component="adb-helper",
                                 target="windows-x86_64", core_identity=KEY,
                                 expected_sha256=identity["sha256"], expected_size=identity["size"])
        self.assertEqual(staged["sha256"], identity["sha256"])
        self.assertEqual((destination / "helpers/adb.exe").read_bytes(), b"binary adb")
        before = (destination / "helpers/adb.exe").read_bytes()
        with self.assertRaises(core.CoreError):
            core.stage_core(self.archive, destination, component="adb-helper",
                            target="windows-x86_64", core_identity=KEY,
                            expected_sha256=identity["sha256"], expected_size=identity["size"])
        self.assertEqual((destination / "helpers/adb.exe").read_bytes(), before)

    def test_wrong_target_and_core_identity_are_rejected(self):
        identity = self.package()
        with self.assertRaises(core.CoreError):
            self.verify(identity, target="linux-arm64")
        with self.assertRaises(core.CoreError):
            core.verify_core(self.archive, component="adb-helper", target=self.default_target,
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
                              component="adb-helper", target=self.default_target,
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
        self.symlink(alias, "../../secret")
        with self.assertRaises(core.CoreError):
            self.package(component="ios-native", target="arm64")
        alias.unlink()
        self.symlink(alias, "libfoo.1.dylib")
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
            core.verify_core(self.archive, component="adb-helper", target=self.default_target,
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


@contextlib.contextmanager
def ntfs_permission_model():
    original = pathlib.Path.lstat

    def lstat(path, *args, **kwargs):
        info = original(path, *args, **kwargs)
        if stat.S_ISREG(info.st_mode):
            fields = list(info)
            fields[0] = info.st_mode & ~0o777 | 0o777
            return os.stat_result(fields)
        return info

    with mock.patch.object(pathlib.Path, "lstat", lstat):
        yield


class CoreFixturePortabilityModelTest(unittest.TestCase):
    """Run the real fixture bodies with modeled platform selection, not Win32."""

    def test_owned_temporary_root_is_canonical_before_backend_selection(self):
        # A test-owned lexical alias models an 8.3 ancestor, not native Win32.
        module = sys.modules[__name__]
        case = DependencyCoreTest()
        with tempfile.TemporaryDirectory() as owned:
            canonical = pathlib.Path(owned).resolve(strict=True)
            (canonical / "alias-child").mkdir()
            alias = canonical / "alias-child" / ".."
            temporary = mock.Mock()
            temporary.name = str(alias)

            def select_backend():
                self.assertEqual(case.root, canonical)
                return os.name

            resolve = pathlib.Path.resolve
            try:
                with mock.patch.object(tempfile, "TemporaryDirectory", return_value=temporary), \
                        mock.patch.object(module, "fixture_platform", side_effect=select_backend), \
                        mock.patch.object(pathlib.Path, "resolve", autospec=True,
                                          side_effect=resolve) as canonicalize:
                    case.setUp()
                    canonicalize.assert_any_call(alias, strict=True)
            finally:
                case.doCleanups()
            temporary.cleanup.assert_called_once_with()

    def test_positive_fixture_selects_private_windows_creation_api(self):
        module = sys.modules[__name__]

        native_create = core.windows_security.create_private_parent

        def create(parent, name):
            if os.name == "nt":
                return native_create(parent, name)
            path = parent / name
            path.mkdir(mode=0o700)
            return path

        result = unittest.TestResult()
        with mock.patch.object(module, "fixture_platform", return_value="nt"), \
                mock.patch.object(core.windows_security, "create_private_parent", side_effect=create) as factory:
            DependencyCoreTest(
                "test_verified_binary_core_stages_only_allowlisted_files_in_private_destination"
            ).run(result)
        self.assertTrue(result.wasSuccessful(), result.errors + result.failures)
        self.assertEqual(factory.call_count, 1)

    def test_common_format_fixture_uses_native_windows_target_under_ntfs_mode_model(self):
        module = sys.modules[__name__]
        result = unittest.TestResult()
        with mock.patch.object(module, "fixture_platform", return_value="nt"), \
                ntfs_permission_model():
            DependencyCoreTest(
                "test_qualification_receipt_and_release_version_changes_cannot_mutate_binary_core"
            ).run(result)
        self.assertTrue(result.wasSuccessful(), result.errors + result.failures)

    def test_foreign_archive_formats_do_not_claim_ntfs_posix_mode_or_link_support(self):
        module = sys.modules[__name__]
        result = unittest.TestResult()
        with mock.patch.object(module, "fixture_platform", return_value="nt"), \
                ntfs_permission_model():
            DependencyCoreTest("test_all_seven_targets_accept_only_their_binary_closures").run(result)
        self.assertTrue(result.wasSuccessful(), result.errors + result.failures)

    def test_supported_core_fixture_suite_under_windows_selection_and_ntfs_mode_model(self):
        module = sys.modules[__name__]
        native_create = core.windows_security.create_private_parent
        original_names = unittest.defaultTestLoader.getTestCaseNames(DependencyCoreTest)
        names = [name for name in original_names
                 if name != "test_verified_ios_core_stages_only_direct_internal_dylib_alias"]

        def create(parent, name):
            if os.name == "nt":
                return native_create(parent, name)
            path = parent / name
            path.mkdir(mode=0o700)
            return path

        result = unittest.TestResult()
        with mock.patch.object(module, "fixture_platform", return_value="nt"), \
                mock.patch.object(core.windows_security, "create_private_parent", side_effect=create), \
                ntfs_permission_model():
            unittest.TestSuite(DependencyCoreTest(name) for name in names).run(result)
        self.assertTrue(result.wasSuccessful(), result.errors + result.failures)
        self.assertEqual(result.testsRun, len(names))
        self.assertTrue(all(case._testMethodName == "test_native_windows_binary_core_staging_and_collision"
                            for case, _reason in result.skipped))

    def test_foreign_owner_fixture_handles_platform_without_geteuid(self):
        module = sys.modules[__name__]
        result = unittest.TestResult()
        with mock.patch.object(module, "fixture_platform", return_value="nt"), \
                mock.patch.dict(vars(os)):
            vars(os).pop("geteuid", None)
            DependencyCoreTest(
                "test_staging_rejects_unverified_windows_acl_or_foreign_parent_owner"
            ).run(result)
        self.assertTrue(result.wasSuccessful(), result.errors + result.failures)


if __name__ == "__main__":
    unittest.main()
