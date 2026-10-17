"""Generated archive fixtures; no vendor binaries are checked into the repository."""
import copy
import datetime
import hashlib
import io
import json
import os
import pathlib
import stat
import struct
import sys
import tempfile
import types
import unittest
import zipfile
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import materialize_openssl as openssl


def digest(data):
    return hashlib.sha256(data).hexdigest()


def pe(architecture):
    data = bytearray(128)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<H", data, 68, 0x14c if architecture == "x86" else 0x8664)
    struct.pack_into("<H", data, 84, 2)
    struct.pack_into("<H", data, 88, 0x10b if architecture == "x86" else 0x20b)
    return bytes(data)


class OpenSslTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name).resolve()
        self.archive = self.root / "openssl.zip"
        self.lock = self.root / "lock.json"
        self.document = {
            "schema_version": 2,
            "archive": {"file_name": "openssl.zip", "size_bytes": 1, "sha256": "0" * 64},
            "version": dict(openssl.VERSION), "limits": dict(openssl.LIMITS),
            "runtime": {arch: {name: {} for name in names} for arch, names in openssl.RUNTIME.items()},
            "transport": {"repository": "ZEACENT/klogg", "repository_id": 123,
                          "release_id": 456, "tag": "openssl-qt5-1.1.1w-input-v1",
                          "asset_id": 789, "asset_name": "openssl.zip",
                          "url": "https://github.com/ZEACENT/klogg/releases/download/"
                                 "openssl-qt5-1.1.1w-input-v1/openssl.zip"},
        }
        self.members = {"openssl-1.1/version.txt": b"OpenSSL_1_1_1w-0-ge04bd3433f\n"}
        for arch in ("x86", "x64"):
            self.members["openssl-1.1/" + arch + "/include/openssl/opensslv.h"] = (
                b'#define OPENSSL_VERSION_NUMBER  0x1010117fL\n'
                b'#define OPENSSL_VERSION_TEXT "OpenSSL 1.1.1w  11 Sep 2023"\n')
            for name in self.document["runtime"][arch]:
                self.members[name] = pe(arch)
        self.write_fixture()

    def write_fixture(self, entries=None, update_runtime=True):
        with zipfile.ZipFile(self.archive, "w", compression=zipfile.ZIP_STORED) as archive:
            for name, data in self.members.items() if entries is None else entries:
                archive.writestr(name, data)
        if update_runtime:
            for arch in ("x86", "x64"):
                for name in self.document["runtime"][arch]:
                    if name in self.members:
                        data = self.members[name]
                        self.document["runtime"][arch][name] = {"size_bytes": len(data), "sha256": digest(data)}
        self.pin_fixture()

    def pin_fixture(self):
        self.document["archive"]["size_bytes"] = self.archive.stat().st_size
        self.document["archive"]["sha256"] = digest(self.archive.read_bytes())
        self.save_lock()

    def save_lock(self):
        self.lock.write_text(json.dumps(self.document))

    def reject(self, pattern):
        with self.assertRaisesRegex(openssl.OpenSslError, pattern):
            openssl.verify_archive(self.archive, self.lock)

    def release_metadata(self):
        origin = self.document["transport"]
        asset = {"id": origin["asset_id"], "name": origin["asset_name"], "state": "uploaded",
                 "size": self.document["archive"]["size_bytes"],
                 "digest": "sha256:" + self.document["archive"]["sha256"],
                 "browser_download_url": origin["url"]}
        prefix = "repos/" + origin["repository"]
        return {
            prefix: {"id": origin["repository_id"], "full_name": origin["repository"]},
            prefix + "/releases/" + str(origin["release_id"]): {
                "id": origin["release_id"], "tag_name": origin["tag"], "draft": False,
                "assets": [copy.deepcopy(asset)]},
            prefix + "/releases/assets/" + str(origin["asset_id"]): asset,
        }

    def authenticate(self, responses):
        with mock.patch.object(openssl, "metadata", side_effect=responses.__getitem__):
            return openssl.authenticate_release(self.document)

    def cold_fixture(self):
        raw = self.archive.read_bytes()
        self.archive.unlink()
        return raw

    def acquire(self, raw, downloader=None, responses=None):
        responses = self.release_metadata() if responses is None else responses
        def download(url, archive, **options):
            self.assertEqual(url, self.document["transport"]["url"])
            deadline = options.pop("cancel_event")
            self.assertFalse(deadline.is_set())
            self.assertEqual(options, {"max_bytes": self.document["archive"]["size_bytes"],
                                       "attempts": 1, "socket_timeout": 30})
            self.assertEqual(archive.parent.parent, self.root)
            self.assertTrue(archive.parent.name.startswith(".klogg-openssl-acquire-"))
            self.assertEqual(stat.S_IMODE(archive.parent.stat().st_mode), 0o700)
            self.assertEqual(list(archive.parent.iterdir()), [])
            archive.write_bytes(raw)
        with mock.patch.object(openssl.sys, "platform", "linux"), mock.patch.dict(
                os.environ, {"GH_TOKEN": "synthetic-read-only-token"}), mock.patch.object(
                openssl, "metadata", side_effect=responses.__getitem__) as get, mock.patch.object(
                openssl, "download_source", side_effect=downloader or download):
            openssl.acquire(self.archive, self.lock)
        self.assertEqual({call.args[0] for call in get.call_args_list}, set(responses))

    def test_release_cold_acquisition_works_after_historical_actions_expiry(self):
        raw = self.cold_fixture()
        future = datetime.datetime(2027, 1, 1, tzinfo=datetime.timezone.utc)
        with mock.patch("datetime.datetime", wraps=datetime.datetime) as clock:
            clock.now.return_value = future
            self.acquire(raw)
        self.assertEqual(self.archive.read_bytes(), raw)
        self.assertEqual({path.name for path in self.root.iterdir()}, {"openssl.zip", "lock.json"})

    def test_valid_generated_archive_is_accepted(self):
        openssl.verify_archive(self.archive, self.lock)

    def test_html_cache_input_is_rejected_without_overwrite(self):
        html = b"<html>HTTP 400 discontinued download</html>"
        self.archive.write_bytes(html)
        with self.assertRaises(openssl.OpenSslError):
            openssl.verify_archive(self.archive, self.lock)
        self.assertEqual(self.archive.read_bytes(), html)

    def test_truncated_or_wrong_hash_archive_is_rejected(self):
        raw = self.archive.read_bytes()
        self.archive.write_bytes(raw[:-1])
        self.reject("size mismatch")
        self.archive.write_bytes(raw[:-1] + bytes([raw[-1] ^ 1]))
        self.reject("SHA-256 mismatch")

    def test_cache_hit_is_verified_even_without_token_and_never_replaced(self):
        self.archive.write_bytes(b"<html>cached transport error</html>")
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(openssl, "metadata") as get:
            with self.assertRaises(openssl.OpenSslError):
                openssl.acquire(self.archive, self.lock)
            get.assert_not_called()
        self.assertEqual(self.archive.read_bytes(), b"<html>cached transport error</html>")

    def test_verified_cache_is_token_free_without_release_metadata(self):
        raw = self.archive.read_bytes()
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(openssl, "metadata") as get:
            openssl.acquire(self.archive, self.lock)
            get.assert_not_called()
        self.assertEqual(self.archive.read_bytes(), raw)

    def test_symlink_fifo_and_reparse_inputs_are_rejected(self):
        alias = self.root / "alias.zip"
        alias.symlink_to(self.archive)
        with self.assertRaisesRegex(openssl.OpenSslError, "regular"):
            openssl.verify_archive(alias, self.lock)
        if hasattr(os, "mkfifo"):
            fifo = self.root / "pipe"
            os.mkfifo(fifo)
            with self.assertRaisesRegex(openssl.OpenSslError, "regular"):
                openssl.verify_archive(fifo, self.lock)
        actual_lstat = pathlib.Path.lstat
        def lstat(path):
            if path == self.archive:
                original = actual_lstat(path)
                return types.SimpleNamespace(st_mode=original.st_mode, st_size=original.st_size,
                                             st_file_attributes=0x400)
            return actual_lstat(path)
        with mock.patch.object(pathlib.Path, "lstat", lstat):
            self.reject("non-reparse")

    def test_strict_lock_schema_types_duplicate_keys_and_nonfinite_values(self):
        for location, key, value in (((), "schema_version", True), ((), "extra", "unknown"),
                                     (("archive",), "size_bytes", True),
                                     (("archive",), "sha256", "A" * 64),
                                     (("limits",), "entries", 262),
                                     (("runtime", "x86"), "unexpected.dll", {}),
                                     ((), "schema_version", 1),
                                     (("transport",), "artifact_id", 789),
                                     (("transport",), "repository", "attacker/klogg"),
                                     (("transport",), "repository_id", True),
                                     (("transport",), "repository_id", 0),
                                     (("transport",), "release_id", "456"),
                                     (("transport",), "release_id", 2 ** 63),
                                     (("transport",), "asset_id", -1),
                                     (("transport",), "asset_id", 789.0),
                                     (("transport",), "tag", "latest"),
                                     (("transport",), "tag", None),
                                     (("transport",), "asset_name", "other.zip"),
                                     (("transport",), "url", "https://attacker.invalid/openssl.zip"),
                                     (("transport",), "url", "https://github.com/ZEACENT/klogg/releases/"
                                      "latest/download/openssl.zip")):
            with self.subTest(key=key, value=value):
                original = copy.deepcopy(self.document)
                target = self.document
                for part in location:
                    target = target[part]
                target[key] = value
                self.save_lock()
                self.reject("lock")
                self.document = original
        self.lock.write_text('{"schema_version":1,"schema_version":1}')
        self.reject("duplicate")
        self.lock.write_text('{"schema_version":NaN}')
        self.reject("non-finite")

    def test_crc_of_unselected_sdk_file_is_validated_before_output(self):
        name = "openssl-1.1/unused.txt"
        self.members[name] = b"unique unselected CRC payload"
        self.write_fixture()
        raw = bytearray(self.archive.read_bytes())
        with zipfile.ZipFile(self.archive) as archive:
            info = archive.getinfo(name)
            offset = info.header_offset
        namesize, extra = struct.unpack_from("<HH", raw, offset + 26)
        raw[offset + 30 + namesize + extra] ^= 1
        self.archive.write_bytes(raw)
        self.pin_fixture()
        self.reject("CRC")
        destination = self.root / "runtime"
        with self.assertRaisesRegex(openssl.OpenSslError, "CRC"):
            openssl.materialize(self.archive, self.lock, "x86", destination)
        self.assertFalse(destination.exists())

    def test_unsafe_windows_paths_and_wrong_roots_are_rejected_below_raw_hash(self):
        for name in ("/openssl-1.1/evil", "openssl-1.1/../evil", "openssl-1.1//evil",
                     "openssl-1.1/./evil", "openssl-1.1\\evil", "openssl-1.1/C:evil",
                     "openssl-1.1/CON.txt", "openssl-1.1/nul", "openssl-1.1/COM1.dll",
                     "openssl-1.1/LPT9.dll", "openssl-1.1/foo.", "openssl-1.1/foo ",
                     "openssl-1.1/bad?name", "openssl-1.1/bad\x01name", "other-root/evil",
                     "openssl-1.1/CON .dll", "openssl-1.1/bad\u0085name",
                     "openssl-1.1/bad" + chr(0xff1a) + "name",
                     "openssl-1.1/COM" + chr(0xb9) + ".dll"):
            with self.subTest(name=name):
                self.write_fixture(entries=list(self.members.items()) + [(name, b"bad")])
                self.reject("unsafe|root")

    def test_nul_path_is_rejected_before_zip_name_truncation(self):
        name = "openssl-1.1/badXname"
        self.write_fixture(entries=list(self.members.items()) + [(name, b"bad")])
        raw = self.archive.read_bytes().replace(name.encode(), name.replace("X", "\0").encode())
        self.archive.write_bytes(raw)
        self.pin_fixture()
        self.reject("NUL")

    def test_casefold_duplicate_and_file_directory_collisions_are_rejected(self):
        cases = [[("openssl-1.1/extra", b"a"), ("openssl-1.1/EXTRA", b"b")],
                 [("openssl-1.1/dir/", b""), ("openssl-1.1/dir", b"a")],
                 [("openssl-1.1/dir", b"a"), ("openssl-1.1/DIR/child", b"b")]]
        for extra in cases:
            with self.subTest(extra=extra):
                self.write_fixture(entries=list(self.members.items()) + extra)
                self.reject("duplicate|collision")

    def test_link_special_and_encrypted_members_are_rejected(self):
        for mode in (stat.S_IFLNK, stat.S_IFIFO, stat.S_IFSOCK, stat.S_IFCHR, stat.S_IFDIR):
            with self.subTest(mode=mode):
                info = zipfile.ZipInfo("openssl-1.1/evil")
                info.create_system = 3
                info.external_attr = (mode | 0o600) << 16
                self.write_fixture(entries=list(self.members.items()) + [(info, b"bad")])
                self.reject("special")
        self.write_fixture()
        raw = bytearray(self.archive.read_bytes())
        for signature, flag_offset in ((b"PK\x03\x04", 6), (b"PK\x01\x02", 8)):
            offset = raw.index(signature)
            struct.pack_into("<H", raw, offset + flag_offset, 1)
        self.archive.write_bytes(raw)
        self.pin_fixture()
        self.reject("encrypted")

    def test_observed_dos_directories_and_unix_type_zero_files_are_accepted(self):
        directory = zipfile.ZipInfo("openssl-1.1/")
        directory.create_system = 0
        directory.external_attr = 0x10
        file = zipfile.ZipInfo("openssl-1.1/extra.txt")
        file.create_system = 3
        file.external_attr = 0o600 << 16
        self.write_fixture(entries=[(directory, b"")] + list(self.members.items()) + [(file, b"safe")])
        openssl.verify_archive(self.archive, self.lock)

    def test_archive_entry_expanded_member_and_marker_limits(self):
        for key, value, pattern in (("entries", 6, "entry count"),
                                     ("expanded_bytes", 128, "expanded size"),
                                     ("member_bytes", 100, "member exceeds"),
                                     ("marker_bytes", 20, "marker")):
            with self.subTest(key=key):
                original = copy.deepcopy(self.document)
                self.document["limits"][key] = value
                self.save_lock()
                self.reject(pattern)
                self.document = original

    def test_streamed_member_checks_do_not_trust_central_directory_size(self):
        member = types.SimpleNamespace(file_size=2)
        archive = mock.Mock()
        archive.open.return_value = io.BytesIO(b"oversized")
        with self.assertRaisesRegex(openssl.OpenSslError, "streamed.*exceeds"):
            openssl._member_bytes(archive, member, 3)
        archive.open.return_value = io.BytesIO(b"x")
        with self.assertRaisesRegex(openssl.OpenSslError, "size mismatch"):
            openssl._member_bytes(archive, member, 3)

    def test_wrong_version_markers_and_missing_layout_reach_specific_checks(self):
        for name in ("openssl-1.1/version.txt", "openssl-1.1/x86/include/openssl/opensslv.h",
                     "openssl-1.1/x64/include/openssl/opensslv.h"):
            with self.subTest(name=name):
                original = self.members[name]
                self.members[name] = original.replace(b"1.1.1w", b"1.1.1v").replace(b"1_1_1w", b"1_1_1v")
                self.write_fixture()
                self.reject("version")
                del self.members[name]
                self.write_fixture()
                self.reject("layout missing")
                self.members[name] = original
        name = next(iter(self.document["runtime"]["x86"]))
        original = self.members.pop(name)
        self.write_fixture()
        self.reject("layout missing")
        self.members[name] = original

    def test_dll_hash_and_size_are_checked_independently(self):
        name = next(iter(self.document["runtime"]["x86"]))
        original = self.members[name]
        for changed in (original + b"extra", original[:-1] + b"X"):
            with self.subTest(size=len(changed)):
                self.members[name] = changed
                self.write_fixture(update_runtime=False)
                self.reject("DLL size or SHA-256")

    def test_pe_machine_magic_and_offset_checks_reached_with_computed_hashes(self):
        name = next(iter(self.document["runtime"]["x86"]))
        for mutation in ("architecture", "dos", "magic", "offset", "optional_size"):
            with self.subTest(mutation=mutation):
                data = bytearray(pe("x64" if mutation == "architecture" else "x86"))
                if mutation == "dos":
                    data[:2] = b"NO"
                elif mutation == "magic":
                    struct.pack_into("<H", data, 88, 0x20b)
                elif mutation == "offset":
                    struct.pack_into("<I", data, 60, 65537)
                elif mutation == "optional_size":
                    struct.pack_into("<H", data, 84, 0)
                self.members[name] = bytes(data)
                self.write_fixture()
                self.reject("PE")

    def test_only_selected_two_dlls_are_materialized_without_execution(self):
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(openssl, "metadata") as get:
            for arch, identities in self.document["runtime"].items():
                destination = self.root / ("runtime-" + arch)
                openssl.materialize(self.archive, self.lock, arch, destination)
                expected = {arch + "/bin/" + pathlib.PurePosixPath(name).name for name in identities}
                outputs = {path.relative_to(destination).as_posix() for path in destination.rglob("*")
                           if path.is_file()}
                self.assertEqual(outputs, expected)
                for name, identity in identities.items():
                    data = (destination / arch / "bin" / pathlib.PurePosixPath(name).name).read_bytes()
                    self.assertEqual(digest(data), identity["sha256"])
            get.assert_not_called()

    def test_existing_runtime_destinations_are_preserved(self):
        destination = self.root / "runtime"
        for kind in ("file", "directory", "symlink", "dangling"):
            with self.subTest(kind=kind):
                if kind == "file":
                    destination.write_bytes(b"existing")
                elif kind == "directory":
                    destination.mkdir()
                    (destination / "keep").write_bytes(b"existing")
                else:
                    destination.symlink_to(self.archive if kind == "symlink" else self.root / "missing")
                with self.assertRaisesRegex(openssl.OpenSslError, "already exists"):
                    openssl.materialize(self.archive, self.lock, "x86", destination)
                if kind == "directory":
                    self.assertEqual((destination / "keep").read_bytes(), b"existing")
                    (destination / "keep").unlink()
                    destination.rmdir()
                else:
                    self.assertTrue(destination.is_symlink() if kind != "file" else destination.read_bytes() == b"existing")
                    destination.unlink()

    def test_interrupted_materialization_removes_only_owned_output(self):
        destination = self.root / "runtime"
        sibling = self.root / "do-not-touch"
        sibling.write_bytes(b"unrelated")
        original_open = pathlib.Path.open
        for failure in (OSError, KeyboardInterrupt):
            with self.subTest(failure=failure):
                def interrupt(path, mode="r", *args, **kwargs):
                    if path.parent == destination / "x86/bin" and path.name == "libssl-1_1.dll":
                        raise failure("simulated second DLL write failure")
                    return original_open(path, mode, *args, **kwargs)
                with mock.patch.object(pathlib.Path, "open", interrupt):
                    with self.assertRaisesRegex(failure, "simulated"):
                        openssl.materialize(self.archive, self.lock, "x86", destination)
                self.assertFalse(destination.exists())
                self.assertEqual(sibling.read_bytes(), b"unrelated")
                self.assertEqual({path.name for path in self.root.iterdir()},
                                 {"openssl.zip", "lock.json", "do-not-touch"})

    def test_cleanup_does_not_remove_unexpected_files_in_owned_root(self):
        destination = self.root / "runtime"
        original_open = pathlib.Path.open
        def interrupt(path, mode="r", *args, **kwargs):
            if path.parent == destination / "x86/bin" and path.name == "libssl-1_1.dll":
                (destination / "other-actor").write_bytes(b"keep")
                raise OSError("interrupted")
            return original_open(path, mode, *args, **kwargs)
        with mock.patch.object(pathlib.Path, "open", interrupt):
            with self.assertRaises(OSError):
                openssl.materialize(self.archive, self.lock, "x86", destination)
        self.assertEqual((destination / "other-actor").read_bytes(), b"keep")
        self.assertEqual(list(destination.iterdir()), [destination / "other-actor"])

    def test_cleanup_preserves_replacements_at_previously_owned_paths(self):
        destination = self.root / "runtime"
        original_open = pathlib.Path.open
        replacement = destination / "x86/bin/libcrypto-1_1.dll"
        def interrupt(path, mode="r", *args, **kwargs):
            if path.parent == destination / "x86/bin" and path.name == "libssl-1_1.dll":
                replacement.rename(self.root / "detached-owned-file")
                replacement.write_bytes(b"other actor replacement")
                raise OSError("interrupted after replacement")
            return original_open(path, mode, *args, **kwargs)
        with mock.patch.object(pathlib.Path, "open", interrupt):
            with self.assertRaises(OSError):
                openssl.materialize(self.archive, self.lock, "x86", destination)
        self.assertTrue(replacement.exists(), "cleanup deleted another actor's replacement")
        self.assertEqual(replacement.read_bytes(), b"other actor replacement")

    def test_cleanup_preserves_directory_replacements(self):
        destination = self.root / "runtime"
        original_open = pathlib.Path.open
        binary_dir = destination / "x86/bin"
        def interrupt(path, mode="r", *args, **kwargs):
            if path.parent == binary_dir and path.name == "libssl-1_1.dll":
                binary_dir.rename(self.root / "detached-owned-directory")
                binary_dir.mkdir()
                raise OSError("interrupted after directory replacement")
            return original_open(path, mode, *args, **kwargs)
        with mock.patch.object(pathlib.Path, "open", interrupt):
            with self.assertRaises(OSError):
                openssl.materialize(self.archive, self.lock, "x86", destination)
        self.assertTrue(binary_dir.is_dir(), "cleanup deleted another actor's directory replacement")

    def test_release_authentication_allows_unrelated_provenance_and_license_sidecars(self):
        responses = self.release_metadata()
        release = next(value for path, value in responses.items() if path.endswith("/456"))
        release["assets"].append({"id": 790, "name": "provenance.json"})
        self.authenticate(responses)

    def test_metadata_substitutions_are_rejected(self):
        original = self.release_metadata()
        repository_path, release_path, asset_path = original
        cases = [(repository_path, ("id",), 124),
                 (repository_path, ("id",), "123"),
                 (repository_path, ("full_name",), "attacker/klogg"),
                 (release_path, ("id",), 457),
                 (release_path, ("id",), "456"),
                 (release_path, ("tag_name",), "latest"),
                 (release_path, ("draft",), True),
                 (release_path, ("draft",), 0)]
        for fields, changed in ((("id",), 790), (("id",), "789"),
                                (("name",), "other.zip"), (("state",), "open"),
                                (("size",), self.document["archive"]["size_bytes"] + 1),
                                (("size",), True), (("digest",), None),
                                (("digest",), "sha256:" + "b" * 64),
                                (("browser_download_url",), "https://attacker.invalid/openssl.zip")):
            cases.append((asset_path, fields, changed))
            cases.append((release_path, ("assets", 0) + fields, changed))
        for path, fields, changed in cases:
            with self.subTest(path=path, fields=fields, changed=changed):
                responses = copy.deepcopy(original)
                target = responses[path]
                for field in fields[:-1]:
                    target = target[field]
                target[fields[-1]] = changed
                with self.assertRaises(openssl.OpenSslError):
                    self.authenticate(responses)

    def test_malformed_metadata_and_missing_identity_fields_fail_closed(self):
        original = self.release_metadata()
        for path, response in original.items():
            for malformed in (None, [], "metadata", 1):
                with self.subTest(path=path, malformed=malformed):
                    responses = copy.deepcopy(original)
                    responses[path] = malformed
                    with self.assertRaises(openssl.OpenSslError):
                        self.authenticate(responses)
            for field in response:
                with self.subTest(path=path, missing=field):
                    responses = copy.deepcopy(original)
                    del responses[path][field]
                    with self.assertRaises(openssl.OpenSslError):
                        self.authenticate(responses)

    def test_missing_malformed_and_duplicate_release_associations_are_rejected(self):
        for mutation in ("missing", "unrelated", "duplicate", "duplicate-id", "duplicate-name",
                         "not-list", "not-object", "bad-id", "bad-name", "missing-id"):
            with self.subTest(mutation=mutation):
                responses = self.release_metadata()
                release = next(value for path, value in responses.items() if path.endswith("/456"))
                asset = release["assets"][0]
                if mutation == "missing":
                    release["assets"] = []
                elif mutation == "unrelated":
                    release["assets"] = [dict(asset, id=790, name="provenance.json")]
                elif mutation == "duplicate":
                    release["assets"] *= 2
                elif mutation == "duplicate-id":
                    release["assets"].append(dict(asset, name="provenance.json"))
                elif mutation == "duplicate-name":
                    release["assets"].append(dict(asset, id=790))
                elif mutation == "not-list":
                    release["assets"] = {"asset": asset}
                elif mutation == "not-object":
                    release["assets"].append(None)
                elif mutation == "bad-id":
                    release["assets"].append({"id": True, "name": "provenance.json"})
                elif mutation == "bad-name":
                    release["assets"].append({"id": 790, "name": []})
                else:
                    del asset["id"]
                with self.assertRaises(openssl.OpenSslError):
                    self.authenticate(responses)

    def test_failed_authentication_never_downloads_or_publishes(self):
        raw = self.cold_fixture()
        responses = self.release_metadata()
        responses["repos/ZEACENT/klogg/releases/456"]["draft"] = True
        download = mock.Mock()
        with self.assertRaises(openssl.OpenSslError):
            self.acquire(raw, downloader=download, responses=responses)
        download.assert_not_called()
        self.assertEqual({path.name for path in self.root.iterdir()}, {"lock.json"})

    def test_cold_acquisition_requires_token_and_linux_without_metadata_calls(self):
        self.archive.unlink()
        with mock.patch.object(openssl.sys, "platform", "linux"), mock.patch.dict(
                os.environ, {}, clear=True), mock.patch.object(openssl, "metadata") as get:
            with self.assertRaisesRegex(openssl.OpenSslError, "GH_TOKEN"):
                openssl.acquire(self.archive, self.lock)
            get.assert_not_called()
        with mock.patch.object(openssl.sys, "platform", "win32"):
            with self.assertRaisesRegex(openssl.OpenSslError, "Ubuntu"):
                openssl.acquire(self.archive, self.lock)

    def test_downloaded_size_hash_and_zip_failures_never_publish_or_retry(self):
        original = self.archive.read_bytes()
        cases = [(original[:-1], "size mismatch", False),
                 (original + b"excess", "byte limit", False),
                 (original[:-1] + bytes([original[-1] ^ 1]), "SHA-256 mismatch", False),
                 (b"<html>transport error</html>", "invalid OpenSSL ZIP", True)]
        for raw, pattern, repin in cases:
            with self.subTest(pattern=pattern):
                if repin:
                    self.archive.write_bytes(raw)
                    self.pin_fixture()
                self.archive.unlink(missing_ok=True)
                download = mock.Mock(side_effect=lambda url, path, **options: path.write_bytes(raw))
                with self.assertRaisesRegex(openssl.OpenSslError, pattern):
                    self.acquire(raw, download)
                download.assert_called_once()
                self.assertEqual({path.name for path in self.root.iterdir()}, {"lock.json"})

    def test_downloaded_archive_crc_is_checked_before_publication(self):
        name = "openssl-1.1/unused.txt"
        self.members[name] = b"unique unselected CRC payload"
        self.write_fixture()
        raw = self.archive.read_bytes().replace(self.members[name], b"X" * len(self.members[name]))
        self.archive.write_bytes(raw)
        self.pin_fixture()
        self.archive.unlink()
        with self.assertRaisesRegex(openssl.OpenSslError, "CRC"):
            self.acquire(raw)
        self.assertEqual({path.name for path in self.root.iterdir()}, {"lock.json"})

    def test_transport_failure_cleanup_and_single_attempt(self):
        raw = self.cold_fixture()
        for failure in (OSError("connection interrupted"),
                        RuntimeError("download exceeds byte limit"),
                        KeyboardInterrupt("cancelled")):
            with self.subTest(failure=failure):
                def fail(url, path, **options):
                    path.write_bytes(raw[:10])
                    raise failure
                failed = mock.Mock(side_effect=fail)
                expected = KeyboardInterrupt if isinstance(failure, KeyboardInterrupt) else openssl.OpenSslError
                with self.assertRaisesRegex(expected, str(failure)):
                    self.acquire(raw, failed)
                failed.assert_called_once()
                self.assertEqual({path.name for path in self.root.iterdir()}, {"lock.json"})

    def test_transfer_deadline_cancels_between_reads_with_openssl_error_and_cleanup(self):
        raw = self.cold_fixture()
        clock = [100.0]
        def download(url, path, **options):
            cancellation = options.get("cancel_event")
            self.assertIsNotNone(cancellation, "download must receive the transfer deadline")
            self.assertFalse(cancellation.is_set())
            path.write_bytes(raw[:10])
            clock[0] = 999.999
            self.assertFalse(cancellation.is_set())
            clock[0] = 1000.0
            self.assertTrue(cancellation.is_set())
            raise RuntimeError("ADB source prefetch cancelled")
        with mock.patch("time.monotonic", side_effect=lambda: clock[0]), mock.patch.object(
                openssl.os, "link") as publish:
            with self.assertRaisesRegex(openssl.OpenSslError, "OpenSSL download exceeded deadline"):
                self.acquire(raw, downloader=download)
            publish.assert_not_called()
        self.assertEqual({path.name for path in self.root.iterdir()}, {"lock.json"})

    def test_elapsed_deadline_after_verification_prevents_publication(self):
        raw = self.cold_fixture()
        clock = [100.0]
        verify = openssl._verify_bytes
        def finish_verification(content, lock):
            result = verify(content, lock)
            clock[0] = 1000.0
            return result
        with mock.patch("time.monotonic", side_effect=lambda: clock[0]), mock.patch.object(
                openssl, "_verify_bytes", side_effect=finish_verification), mock.patch.object(
                openssl.os, "link") as publish:
            with self.assertRaisesRegex(openssl.OpenSslError, "OpenSSL download exceeded deadline"):
                self.acquire(raw)
            publish.assert_not_called()
        self.assertEqual({path.name for path in self.root.iterdir()}, {"lock.json"})

    def test_metadata_transport_failure_stops_without_download_or_fallback(self):
        self.archive.unlink()
        with mock.patch.object(openssl.sys, "platform", "linux"), mock.patch.dict(
                os.environ, {"GH_TOKEN": "synthetic-read-only-token"}), mock.patch.object(
                openssl, "metadata", side_effect=openssl.TransportError("metadata unavailable")) as get, mock.patch.object(
                openssl, "download_source") as download:
            with self.assertRaisesRegex(openssl.OpenSslError, "metadata unavailable"):
                openssl.acquire(self.archive, self.lock)
            get.assert_called_once_with("repos/ZEACENT/klogg")
            download.assert_not_called()
        self.assertEqual({path.name for path in self.root.iterdir()}, {"lock.json"})

    def test_racing_acquisition_destination_is_not_overwritten_or_cleaned(self):
        raw = self.cold_fixture()
        original_link = os.link
        def race(source, target):
            self.archive.write_bytes(b"other actor")
            return original_link(source, target)
        with mock.patch.object(openssl.os, "link", side_effect=race):
            with self.assertRaisesRegex(openssl.OpenSslError, "already exists"):
                self.acquire(raw)
        self.assertEqual(self.archive.read_bytes(), b"other actor")
        self.assertEqual({path.name for path in self.root.iterdir()}, {"openssl.zip", "lock.json"})

    def test_retained_authentic_archive_verifies_and_materializes_both_architectures(self):
        configured = os.environ.get("KLOGG_OPENSSL_TEST_ARCHIVE")
        archive = pathlib.Path(configured) if configured else (
            ROOT / "build_root_crash_collector_draft/openssl-recovery-36309613109/openssl.zip")
        if configured:
            self.assertTrue(archive.is_file(), "KLOGG_OPENSSL_TEST_ARCHIVE must name an existing archive")
        elif not archive.is_file():
            self.skipTest("retained authentic archive is not committed")
        lock = ROOT / "ci/tools/openssl-qt5.json"
        authentic = openssl.load_lock(lock)
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(openssl, "metadata") as get:
            openssl.verify_archive(archive, lock)
            for architecture, identities in authentic["runtime"].items():
                destination = self.root / ("authentic-" + architecture)
                openssl.materialize(archive, lock, architecture, destination)
                for name, identity in identities.items():
                    output = destination / architecture / "bin" / pathlib.PurePosixPath(name).name
                    self.assertEqual(output.stat().st_size, identity["size_bytes"])
                    self.assertEqual(digest(output.read_bytes()), identity["sha256"])
                    self.assertEqual(openssl.architecture_from_pe(output), architecture)
            get.assert_not_called()

    def test_cli_contract_rejects_missing_mode_and_incomplete_materialization(self):
        common = ["--lock", str(self.lock), "--archive", str(self.archive)]
        with mock.patch("sys.stderr", new_callable=io.StringIO):
            for args in ([], ["--verify-only", "--acquire"], ["--materialize"],
                         ["--materialize", "--architecture", "arm64"],
                         ["--verify-only", "--destination", str(self.root / "out")]):
                with self.subTest(args=args), self.assertRaises(SystemExit) as error:
                    openssl.main(common + args)
                self.assertEqual(error.exception.code, 2)
        with mock.patch("sys.stdout", new_callable=io.StringIO), mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(openssl.main(common + ["--verify-only"]), 0)
        self.archive.write_bytes(b"HTML")
        with mock.patch("sys.stderr", new_callable=io.StringIO):
            self.assertEqual(openssl.main(common + ["--verify-only"]), 1)


if __name__ == "__main__":
    unittest.main()
