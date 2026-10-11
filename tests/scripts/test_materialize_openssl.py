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
        self.document = json.loads((ROOT / "ci/tools/openssl-qt5.json").read_text())
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

    def provenance(self):
        origin = self.document["transport"]
        repository = {"id": origin["repository_id"], "full_name": origin["repository"]}
        run = {"id": origin["run_id"], "head_sha": origin["head_sha"], "head_branch": origin["branch"],
               "event": origin["event"], "path": origin["workflow"], "run_attempt": 2,
               "repository": repository, "head_repository": repository, "conclusion": "failure"}
        attempt = dict(run, run_attempt=1, run_started_at="2026-09-27T09:30:51Z")
        upload = {"name": "Run actions/upload-artifact@" + "a" * 40, "status": "completed",
                  "conclusion": "success", "started_at": origin["upload_started_at"],
                  "completed_at": origin["upload_completed_at"]}
        job = {"id": origin["producer_id"], "name": origin["producer_name"], "run_id": origin["run_id"],
               "run_attempt": 1, "head_sha": origin["head_sha"], "status": "completed",
               "conclusion": "success", "started_at": origin["producer_started_at"],
               "completed_at": origin["producer_completed_at"], "steps": [upload]}
        artifact = {"id": origin["artifact_id"], "name": origin["artifact_name"], "expired": False,
                    "digest": origin["artifact_digest"], "size_in_bytes": origin["artifact_metadata_size"],
                    "created_at": origin["artifact_created_at"], "expires_at": origin["artifact_expires_at"],
                    "workflow_run": {"id": origin["run_id"], "repository_id": origin["repository_id"],
                                     "head_repository_id": origin["repository_id"],
                                     "head_branch": origin["branch"], "head_sha": origin["head_sha"]}}
        prefix = "repos/ZEACENT/klogg/actions/"
        run_path = prefix + "runs/" + str(origin["run_id"])
        return {prefix + "artifacts/" + str(origin["artifact_id"]): artifact,
                run_path: run, run_path + "/attempts/1": attempt,
                run_path + "/attempts/1/jobs?per_page=100": {"total_count": 1, "jobs": [job]},
                prefix + "jobs/" + str(origin["producer_id"]): copy.deepcopy(job)}

    def authenticate(self, responses):
        with mock.patch.object(openssl, "metadata", side_effect=responses.__getitem__):
            return openssl.authenticate_artifact(self.document, now=datetime.datetime(
                2026, 10, 11, tzinfo=datetime.timezone.utc))

    def outer_fixture(self, entries=None):
        raw = self.archive.read_bytes()
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as wrapper:
            for name, data in entries or [("openssl.zip", raw)]:
                wrapper.writestr(name, data)
        self.document["transport"]["artifact_digest"] = "sha256:" + digest(output.getvalue())
        self.save_lock()
        self.archive.unlink()
        return output.getvalue()

    def acquire(self, outer, downloader=None):
        responses = self.provenance()
        authenticate = openssl.authenticate_artifact
        def download(artifact_id, archive, **options):
            self.assertEqual(artifact_id, 10928159537)
            self.assertEqual(options, {"limit": 32 * 1024 * 1024, "timeout": 900})
            archive.write_bytes(outer)
        with mock.patch.object(openssl.sys, "platform", "linux"), mock.patch.dict(
                os.environ, {"GH_TOKEN": "synthetic-read-only-token"}), mock.patch.object(
                openssl, "metadata", side_effect=responses.__getitem__), mock.patch.object(
                openssl, "download_artifact_zip", side_effect=downloader or download), mock.patch.object(
                openssl, "authenticate_artifact", side_effect=lambda lock: authenticate(
                    lock, now=datetime.datetime(2026, 10, 11, tzinfo=datetime.timezone.utc))):
            openssl.acquire(self.archive, self.lock)

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

    def test_verified_cache_is_token_free_and_usable_after_transport_expiry(self):
        self.document["transport"]["artifact_expires_at"] = "2026-09-28T09:30:51Z"
        self.save_lock()
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
                                     (("transport",), "attempt", 2)):
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

    def test_original_producer_succeeds_even_when_run_failed_and_later_attempt_exists(self):
        self.authenticate(self.provenance())

    def test_metadata_substitutions_are_rejected(self):
        original = self.provenance()
        paths = list(original)
        cases = [(paths[0], ("digest",), "sha256:" + "b" * 64),
                 (paths[0], ("id",), 10928159538), (paths[0], ("expired",), True),
                 (paths[0], ("created_at",), "2026-09-27T09:31:38Z"),
                 (paths[0], ("expires_at",), "2026-12-27T09:30:51Z"),
                 (paths[0], ("size_in_bytes",), 15795727),
                 (paths[0], ("workflow_run", "repository_id"), 1),
                 (paths[0], ("workflow_run", "head_repository_id"), 1),
                 (paths[0], ("workflow_run", "head_sha"), "b" * 40),
                 (paths[1], ("repository", "full_name"), "attacker/klogg"),
                 (paths[1], ("head_repository", "id"), 1),
                 (paths[1], ("head_branch",), "master"),
                 (paths[1], ("path",), ".github/workflows/other.yml"),
                 (paths[1], ("event",), "push"),
                 (paths[2], ("run_attempt",), 2),
                 (paths[2], ("head_sha",), "b" * 40),
                 (paths[2], ("run_started_at",), "2026-09-27T09:31:39Z"),
                 (paths[4], ("id",), 108643159517),
                 (paths[4], ("run_attempt",), 2),
                 (paths[4], ("started_at",), "2026-09-27T15:00:00Z"),
                 (paths[4], ("conclusion",), "failure"),
                 (paths[4], ("head_sha",), "b" * 40)]
        for path, fields, changed in cases:
            with self.subTest(fields=fields, changed=changed):
                responses = copy.deepcopy(original)
                target = responses[path]
                for field in fields[:-1]:
                    target = target[field]
                target[fields[-1]] = changed
                with self.assertRaises(openssl.OpenSslError):
                    self.authenticate(responses)

    def test_actual_retained_artifact_metadata_matches_lock_but_carried_job_does_not(self):
        retained = ROOT / "build_root_crash_collector_draft/openssl-recovery-provenance.json"
        if not retained.is_file():
            self.skipTest("retained local provenance is not committed")
        responses = self.provenance()
        artifact_path = next(iter(responses))
        responses[artifact_path] = json.loads(retained.read_text())
        self.authenticate(responses)
        jobs_path = next(path for path in responses if path.endswith("/jobs?per_page=100"))
        responses[jobs_path]["jobs"][0].update(id=108643159517, run_attempt=2)
        with self.assertRaisesRegex(openssl.OpenSslError, "producer"):
            self.authenticate(responses)

    def test_incomplete_duplicate_and_carried_forward_job_lists_are_rejected(self):
        for mutation in ("truncated", "missing", "duplicate", "carried", "malformed"):
            with self.subTest(mutation=mutation):
                responses = self.provenance()
                listing = next(value for path, value in responses.items() if path.endswith("/jobs?per_page=100"))
                if mutation == "truncated":
                    listing["total_count"] = 101
                elif mutation == "missing":
                    listing.update(total_count=0, jobs=[])
                elif mutation == "duplicate":
                    listing["jobs"] *= 2
                    listing["total_count"] = 2
                elif mutation == "carried":
                    listing["jobs"][0].update(id=108643159517, run_attempt=2)
                else:
                    listing["jobs"] = [None]
                with self.assertRaises(openssl.OpenSslError):
                    self.authenticate(responses)

    def test_failed_missing_ambiguous_or_out_of_window_upload_rejected(self):
        for mutation in ("failure", "missing", "duplicate", "time"):
            with self.subTest(mutation=mutation):
                responses = self.provenance()
                producer = responses[next(path for path in responses if "/jobs/" in path)]
                if mutation == "failure":
                    producer["steps"][0]["conclusion"] = "failure"
                elif mutation == "missing":
                    producer["steps"] = []
                elif mutation == "duplicate":
                    producer["steps"] *= 2
                else:
                    producer["steps"][0]["completed_at"] = "2026-09-27T09:31:36Z"
                with self.assertRaisesRegex(openssl.OpenSslError, "upload"):
                    self.authenticate(responses)

    def test_expiry_uses_current_time_not_only_metadata_flag(self):
        with mock.patch.object(openssl, "metadata") as get:
            with self.assertRaisesRegex(openssl.OpenSslError, "expired"):
                openssl.authenticate_artifact(self.document, now=datetime.datetime(
                    2026, 12, 26, 9, 30, 51, tzinfo=datetime.timezone.utc))
            get.assert_not_called()

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

    def test_cold_acquisition_verifies_outer_and_inner_before_no_overwrite_link(self):
        expected = self.archive.read_bytes()
        outer = self.outer_fixture()
        self.assertNotEqual(len(outer), self.document["transport"]["artifact_metadata_size"])
        self.acquire(outer)
        self.assertEqual(self.archive.read_bytes(), expected)
        self.assertEqual({path.name for path in self.root.iterdir()}, {"openssl.zip", "lock.json"})

    def test_unexpected_outer_inventory_is_not_silently_relaxed(self):
        original = self.archive.read_bytes()
        for entries in ([("openssl.zip", original), ("extra.txt", b"extra")],
                        [("directory/", b""), ("openssl.zip", original)],
                        [("other.zip", original)], [("../openssl.zip", original)]):
            with self.subTest(names=[name for name, _ in entries]):
                if not self.archive.exists():
                    self.archive.write_bytes(original)
                outer = self.outer_fixture(entries)
                with self.assertRaises(openssl.OpenSslError):
                    self.acquire(outer)
                self.assertFalse(self.archive.exists())
                self.assertEqual({path.name for path in self.root.iterdir()}, {"lock.json"})

    def test_outer_digest_and_inner_crc_failures_leave_no_input_or_staging(self):
        expected = self.archive.read_bytes()
        outer = self.outer_fixture()
        with self.assertRaisesRegex(openssl.OpenSslError, "outer artifact SHA-256"):
            self.acquire(outer + b"corruption")
        self.assertFalse(self.archive.exists())
        self.archive.write_bytes(expected)
        outer = self.outer_fixture([("openssl.zip", b"<html>transport error</html>")])
        with self.assertRaisesRegex(openssl.OpenSslError, "archive size mismatch"):
            self.acquire(outer)
        self.assertEqual({path.name for path in self.root.iterdir()}, {"lock.json"})

    def test_transport_failure_cleanup_and_single_attempt_no_integrity_retry(self):
        outer = self.outer_fixture()
        def fail(artifact_id, path, **options):
            path.write_bytes(outer[:10])
            raise openssl.TransportError("deadline or actual-byte limit exceeded")
        failed = mock.Mock(side_effect=fail)
        with self.assertRaisesRegex(openssl.OpenSslError, "deadline or actual-byte"):
            self.acquire(outer, failed)
        failed.assert_called_once()
        self.assertFalse(self.archive.exists())
        self.assertEqual({path.name for path in self.root.iterdir()}, {"lock.json"})

    def test_racing_acquisition_destination_is_not_overwritten_or_cleaned(self):
        outer = self.outer_fixture()
        original_link = os.link
        def race(source, target):
            self.archive.write_bytes(b"other actor")
            return original_link(source, target)
        with mock.patch.object(openssl.os, "link", side_effect=race):
            with self.assertRaisesRegex(openssl.OpenSslError, "already exists"):
                self.acquire(outer)
        self.assertEqual(self.archive.read_bytes(), b"other actor")
        self.assertEqual({path.name for path in self.root.iterdir()}, {"openssl.zip", "lock.json"})

    def test_retained_authentic_archive_verifies_and_materializes_both_architectures(self):
        archive = ROOT / "build_root_crash_collector_draft/openssl-recovery-36309613109/openssl.zip"
        if not archive.is_file():
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
