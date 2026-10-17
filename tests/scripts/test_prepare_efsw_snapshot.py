"""Pure Python snapshot contracts; no Git, CMake or shared coordination state."""
import ctypes
import errno
import hashlib
import os
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import prepare_efsw_snapshot as snapshot


@unittest.skipUnless(os.name == "posix", "Snapshot capture requires POSIX descriptor-relative no-follow IO")
class ApprovedContentSnapshotTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "value.txt").write_bytes(b"approved source\n")
        self.clean = hashlib.sha256(
            ("value.txt:" + hashlib.sha256(b"approved source\n").hexdigest() + "\n").encode()
        ).hexdigest()
        self.destination = self.root / "snapshot"

    def test_captures_readonly_clean_bytes_without_git_metadata_or_unsafe_modes(self):
        git = self.source / ".git"
        git.mkdir()
        (git / "config").write_bytes(b"untrusted cache metadata\n")
        (self.source / "value.txt").chmod(0o444)
        self.source.chmod(0o555)
        before = (self.source / "value.txt").stat()
        snapshot.materialize(self.source, self.destination, self.clean)
        self.assertEqual((self.destination / "value.txt").read_bytes(), b"approved source\n")
        self.assertFalse((self.destination / ".git").exists())
        self.assertEqual(stat.S_IMODE(self.destination.stat().st_mode), 0o700)
        output = (self.destination / "value.txt").stat()
        self.assertEqual(output.st_uid, os.geteuid())
        self.assertEqual(stat.S_IMODE(output.st_mode), 0o600)
        self.assertEqual((self.source / "value.txt").stat(), before)
        self.assertEqual((git / "config").read_bytes(), b"untrusted cache metadata\n")

    def test_rejects_source_symlink_even_when_its_bytes_match_approval(self):
        witness = self.root / "external"
        witness.write_bytes(b"approved source\n")
        (self.source / "value.txt").unlink()
        (self.source / "value.txt").symlink_to(witness)
        with self.assertRaisesRegex(snapshot.SnapshotError, "symlink"):
            snapshot.materialize(self.source, self.destination, self.clean)
        self.assertFalse(self.destination.exists())
        self.assertEqual(witness.read_bytes(), b"approved source\n")

    def test_rejects_special_source_without_opening_a_fifo(self):
        os.mkfifo(self.source / "extra")
        with self.assertRaisesRegex(snapshot.SnapshotError, "special"):
            snapshot.materialize(self.source, self.destination, self.clean)
        self.assertFalse(self.destination.exists())

    def test_rejects_git_metadata_redirect_before_copying_anything(self):
        git = self.source / ".git"
        git.mkdir()
        (git / "commondir").write_bytes(b"../external\n")
        with self.assertRaisesRegex(snapshot.SnapshotError, "metadata redirect"):
            snapshot.materialize(self.source, self.destination, self.clean)
        self.assertFalse(self.destination.exists())

    def test_private_leaf_does_not_authorize_an_ancestor_that_foreign_users_can_rename(self):
        unsafe = self.root / "unsafe-ancestor"
        unsafe.mkdir()
        unsafe.chmod(0o777)
        private = unsafe / "private-parent"
        private.mkdir(mode=0o700)
        destination = private / "snapshot"
        with self.assertRaisesRegex(snapshot.SnapshotError, "namespace ancestor"):
            snapshot.materialize(self.source, destination, self.clean)
        self.assertFalse(destination.exists())
        self.assertEqual(stat.S_IMODE(unsafe.stat().st_mode), 0o777)

    def test_private_parent_creation_preserves_existing_alias_and_permissions(self):
        existing = self.root / "existing"
        existing.mkdir(mode=0o750)
        alias = self.root / "alias"
        alias.symlink_to(existing, target_is_directory=True)
        before = existing.stat().st_mode
        snapshot.prepare_parent(alias / "first" / "second")
        self.assertEqual(existing.stat().st_mode, before)
        self.assertTrue(alias.is_symlink())
        for path in (existing / "first", existing / "first" / "second"):
            self.assertEqual(path.stat().st_mode & 0o777, 0o700)
            self.assertEqual(path.stat().st_uid, os.geteuid())

    def test_private_parent_creation_does_not_repair_an_existing_unsafe_directory(self):
        existing = self.root / "existing"
        existing.mkdir()
        existing.chmod(0o775)
        with self.assertRaises(snapshot.SnapshotError):
            snapshot.prepare_parent(existing / "new")
        self.assertFalse((existing / "new").exists())
        self.assertEqual(existing.stat().st_mode & 0o777, 0o775)

    def test_marker_writer_is_exclusive_and_creates_private_owned_file(self):
        marker = self.root / "marker"
        snapshot.write_marker(marker, "a" * 40, self.clean, "b" * 64)
        before = marker.read_bytes()
        self.assertEqual(marker.stat().st_mode & 0o777, 0o600)
        with self.assertRaises(FileExistsError):
            snapshot.write_marker(marker, "c" * 40, self.clean, "d" * 64)
        self.assertEqual(marker.read_bytes(), before)

    def test_retained_head_with_missing_object_directory_is_not_snapshot_eligible(self):
        git = self.source / ".git"
        git.mkdir()
        (git / "HEAD").write_bytes(b"ref: refs/heads/pinned\n")
        with self.assertRaisesRegex(snapshot.SnapshotError, "retaining HEAD"):
            snapshot.materialize(self.source, self.destination, self.clean)
        self.assertFalse(self.destination.exists())

    def test_never_replaces_existing_destination(self):
        self.destination.mkdir()
        (self.destination / "witness").write_bytes(b"owned by someone else\n")
        with self.assertRaisesRegex(snapshot.SnapshotError, "already exists"):
            snapshot.materialize(self.source, self.destination, self.clean)
        self.assertEqual((self.destination / "witness").read_bytes(), b"owned by someone else\n")

    def test_mutation_during_copy_rejects_and_cleans_owned_staging(self):
        original_read = os.read
        reads_with_content = 0

        def mutate_during_capture(descriptor, size):
            nonlocal reads_with_content
            block = original_read(descriptor, size)
            if block:
                reads_with_content += 1
                if reads_with_content == 2:
                    (self.source / "value.txt").write_bytes(b"mutation during copy\n")
            return block

        with mock.patch.object(snapshot.os, "read", side_effect=mutate_during_capture):
            with self.assertRaisesRegex(snapshot.SnapshotError, "changed during capture"):
                snapshot.materialize(self.source, self.destination, self.clean)
        self.assertFalse(self.destination.exists())

    def test_snapshot_marker_cannot_authorize_dirty_bytes_or_a_new_commit(self):
        revision = "a" * 40
        final = "b" * 64
        snapshot.materialize(self.source, self.destination, self.clean)
        marker = self.root / "marker"
        marker.write_bytes(snapshot.marker_bytes(revision, self.clean, final))
        self.assertEqual(snapshot.verify_snapshot(self.destination, marker, revision, self.clean, final), self.clean)
        (self.destination / "value.txt").write_bytes(b"dirty bytes\n")
        with self.assertRaisesRegex(snapshot.SnapshotError, "dirty/interrupted state rejected"):
            snapshot.verify_snapshot(self.destination, marker, revision, self.clean, final)
        (self.destination / "value.txt").write_bytes(b"approved source\n")
        (self.destination / ".git").mkdir()
        with self.assertRaisesRegex(snapshot.SnapshotError, "metadata/commit to be absent"):
            snapshot.verify_snapshot(self.destination, marker, revision, self.clean, final)

    @unittest.skipUnless(sys.platform == "darwin", "Darwin inherited ACL contract")
    def test_output_does_not_inherit_parent_acl_despite_private_posix_mode(self):
        import pwd
        user = pwd.getpwuid(os.geteuid()).pw_name
        subprocess.run(["/bin/chmod", "+a", f"user:{user} allow write,file_inherit,directory_inherit",
                        str(self.root)], check=True, capture_output=True, timeout=10)
        snapshot.materialize(self.source, self.destination, self.clean)
        native = ctypes.CDLL(None, use_errno=True)
        native.acl_get_fd.argtypes = [ctypes.c_int]
        native.acl_get_fd.restype = ctypes.c_void_p
        native.acl_get_entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)]
        native.acl_get_entry.restype = ctypes.c_int
        native.acl_free.argtypes = [ctypes.c_void_p]
        native.acl_free.restype = ctypes.c_int
        for path in (self.destination, self.destination / "value.txt"):
            descriptor = os.open(path, os.O_RDONLY)
            acl = native.acl_get_fd(descriptor)
            try:
                if not acl:
                    self.assertEqual(ctypes.get_errno(), errno.ENOENT)
                else:
                    entry = ctypes.c_void_p()
                    self.assertNotEqual(native.acl_get_entry(acl, 0, ctypes.byref(entry)), 0,
                                        "Snapshot output retained an inherited ACL entry")
            finally:
                if acl:
                    native.acl_free(acl)
                os.close(descriptor)

    @unittest.skipUnless(sys.platform == "darwin", "Darwin effective marker ACL contract")
    def test_marker_foreign_write_acl_is_rejected_without_clearing_existing_acl(self):
        self.root.chmod(0o755)
        marker = self.root / "marker"
        marker.write_bytes(b"a" * 40 + b"\n")
        marker.chmod(0o644)
        subprocess.run(["/bin/chmod", "+a", "user:daemon allow write", str(marker)],
                       check=True, capture_output=True, timeout=10)
        self.assertEqual(marker.stat().st_mode & 0o777, 0o644)
        native = snapshot._darwin_acl_api()
        native.acl_to_text.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ssize_t)]
        native.acl_to_text.restype = ctypes.c_void_p

        def acl_text():
            descriptor = os.open(marker, os.O_RDONLY)
            acl = native.acl_get_fd(descriptor)
            text = None
            try:
                self.assertTrue(acl, "Foreign grant must be an actual observable ACL")
                length = ctypes.c_ssize_t()
                text = native.acl_to_text(acl, ctypes.byref(length))
                self.assertTrue(text)
                return ctypes.string_at(text, length.value)
            finally:
                if text:
                    native.acl_free(text)
                if acl:
                    native.acl_free(acl)
                os.close(descriptor)

        before = (marker.read_bytes(), marker.stat().st_mode, acl_text())
        with self.assertRaisesRegex(snapshot.SnapshotError, "ACL"):
            snapshot.classify_marker(marker)
        self.assertEqual((marker.read_bytes(), marker.stat().st_mode, acl_text()), before)

    def test_source_xattrs_are_not_copied_into_writable_snapshot(self):
        if not hasattr(os, "setxattr"):
            self.skipTest("Host does not provide extended attribute APIs")
        name = "user.klogg-snapshot-fixture"
        try:
            os.setxattr(self.source / "value.txt", name, b"cache-only attribute")
        except OSError as error:
            self.skipTest("Fixture filesystem does not support user xattrs: " + str(error))
        snapshot.materialize(self.source, self.destination, self.clean)
        self.assertNotIn(name, os.listxattr(self.destination / "value.txt"))
        self.assertEqual(os.getxattr(self.source / "value.txt", name), b"cache-only attribute")

    def test_rejects_mismatch_and_removes_only_owned_staging(self):
        with self.assertRaisesRegex(snapshot.SnapshotError, "SHA-256 mismatch"):
            snapshot.materialize(self.source, self.destination, "0" * 64)
        self.assertFalse(self.destination.exists())
        self.assertEqual((self.source / "value.txt").read_bytes(), b"approved source\n")


if __name__ == "__main__":
    unittest.main()
