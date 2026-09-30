"""Materialized iOS build tools are byte-bound snapshots, never core receipts."""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import stat
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts/materialize_ios_host_tools.py"


class IosHostMaterializationTest(unittest.TestCase):
    def setUp(self):
        self.assertTrue(MODULE_PATH.is_file(), "missing offline iOS host tool materializer")
        sys.path.insert(0, str(ROOT / "scripts"))
        import materialize_ios_host_tools as materializer
        self.addCleanup(sys.path.remove, str(ROOT / "scripts"))
        self.materializer = materializer
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)

    def fixture(self, root, *, reverse=False):
        files = {
            "bin/perl": (b"#!/bin/sh\nexit 0\n", 0o755),
            "bin/aclocal": (b"#!/bin/sh\nexit 0\n", 0o755),
            "share/aclocal/host.m4": (b"AC_DEFUN([HOST],[ok])\n", 0o644),
            "lib/perl/Config.pm": (b"package Config; 1;\n", 0o644),
            "lib/libtool.dylib": (b"synthetic dylib\n", 0o644),
        }
        for name, (content, mode) in sorted(files.items(), reverse=reverse):
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
            path.chmod(mode)
            os.utime(path, (100 if reverse else 200, 100 if reverse else 200))
        (root / "bin" / "perl-alias").symlink_to("perl")
        (root / "bin" / "macro-alias").symlink_to("../share/aclocal/host.m4")
        return files

    def materialize(self, source, name="output", arch="x86_64"):
        destination = self.root / name
        manifest = destination / "manifest.json"
        document = self.materializer.materialize(source, destination, manifest, arch)
        return destination, manifest, document

    def test_unsupported_host_fails_with_typed_error_before_filesystem_access(self):
        with mock.patch.object(self.materializer.os, "name", "nt"):
            with self.assertRaisesRegex(self.materializer.MaterializationError, "POSIX"):
                self.materializer.materialize("absent", "absent-output", "absent-output/manifest.json", "arm64")
            with self.assertRaisesRegex(self.materializer.MaterializationError, "POSIX"):
                self.materializer.verify_materialized("absent", "absent/manifest.json", "arm64")

    def test_cli_materializes_and_independently_verifies_exact_snapshot(self):
        source = self.root / "source"
        source.mkdir()
        self.fixture(source)
        destination = self.root / "output"
        manifest = destination / "manifest.json"
        common = ["--destination", str(destination), "--manifest", str(manifest),
                  "--architecture", "arm64"]
        materialized = subprocess.run([sys.executable, str(MODULE_PATH), "materialize",
                                       "--source", str(source), *common],
                                      capture_output=True, text=True, timeout=15)
        self.assertEqual(materialized.returncode, 0, materialized.stderr)
        checked = subprocess.run([sys.executable, str(MODULE_PATH), "verify", *common],
                                 capture_output=True, text=True, timeout=15)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.assertEqual(materialized.stdout, checked.stdout)
        self.assertEqual(materialized.stdout.strip(), json.loads(manifest.read_text())["identity"])

    def test_both_architectures_have_stable_exact_byte_manifests(self):
        for arch in ("x86_64", "arm64"):
            with self.subTest(arch=arch):
                source_a = self.root / (arch + "-a")
                source_b = self.root / (arch + "-b")
                source_a.mkdir()
                source_b.mkdir()
                files = self.fixture(source_a)
                self.fixture(source_b, reverse=True)
                first, manifest_a, document_a = self.materialize(source_a, arch + "-out-a", arch)
                second, manifest_b, document_b = self.materialize(source_b, arch + "-out-b", arch)
                self.assertEqual(manifest_a.read_bytes(), manifest_b.read_bytes())
                self.assertEqual(document_a, document_b)
                self.assertEqual(document_a["architecture"], arch)
                self.assertEqual(document_a["kind"], "unreviewed-ios-host-tool-tree")
                entries = {entry["path"]: entry for entry in document_a["entries"]}
                self.assertEqual(set(files) | {"bin", "share", "share/aclocal", "lib", "lib/perl",
                                               "bin/perl-alias", "bin/macro-alias"}, set(entries))
                for name, (content, mode) in files.items():
                    self.assertEqual(entries[name]["sha256"], hashlib.sha256(content).hexdigest())
                    self.assertEqual(entries[name]["mode"], mode)
                    self.assertEqual(entries[name]["size"], len(content))
                self.assertEqual(entries["bin/perl-alias"], {
                    "path": "bin/perl-alias", "type": "symlink", "target": "perl"})
                self.assertEqual(entries["bin/macro-alias"], {
                    "path": "bin/macro-alias", "type": "symlink",
                    "target": "../share/aclocal/host.m4"})
                self.assertEqual(document_a["complete_host_closure"], False)
                self.materializer.verify_materialized(first, manifest_a, arch)
                self.materializer.verify_materialized(second, manifest_b, arch)

    def test_missing_extra_or_changed_module_and_macro_fail_verification(self):
        for changed in ("missing", "extra", "module", "macro"):
            with self.subTest(changed=changed):
                source = self.root / (changed + "-source")
                source.mkdir()
                self.fixture(source)
                destination, manifest, _ = self.materialize(source, changed)
                if changed == "missing":
                    (destination / "lib/perl/Config.pm").unlink()
                elif changed == "extra":
                    (destination / "lib/perl/extra.pm").write_text("extra", encoding="utf-8")
                elif changed == "module":
                    (destination / "lib/perl/Config.pm").write_text("changed", encoding="utf-8")
                else:
                    (destination / "share/aclocal/host.m4").write_text("changed", encoding="utf-8")
                with self.assertRaises(self.materializer.MaterializationError):
                    self.materializer.verify_materialized(destination, manifest, "x86_64")

    def test_wrong_architecture_or_changed_manifest_cannot_rebind_tree(self):
        source = self.root / "source"
        source.mkdir()
        self.fixture(source)
        destination, manifest, document = self.materialize(source)
        with self.assertRaises(self.materializer.MaterializationError):
            self.materializer.verify_materialized(destination, manifest, "arm64")
        document["entries"][0]["mode"] = 0o777
        manifest.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaises(self.materializer.MaterializationError):
            self.materializer.verify_materialized(destination, manifest, "x86_64")

    def test_escaping_dangling_and_cyclic_links_are_rejected(self):
        for kind, target in (("escape", "../../../outside"), ("absolute", "/usr/bin/perl"),
                             ("dangling", "missing"), ("cycle", "perl-alias")):
            with self.subTest(kind=kind):
                source = self.root / (kind + "-source")
                source.mkdir()
                self.fixture(source)
                alias = source / "bin/perl-alias"
                alias.unlink()
                alias.symlink_to(target)
                with self.assertRaises(self.materializer.MaterializationError):
                    self.materialize(source, kind)

    def test_read_access_time_change_does_not_invalidate_stable_file_bytes(self):
        file = self.root / "module.pm"
        file.write_bytes(b"package Module; 1;\n")
        attributes = {"st_mode": stat.S_IFREG | 0o644, "st_nlink": 1,
                      "st_size": file.stat().st_size, "st_dev": 1, "st_ino": 2,
                      "st_mtime_ns": 3, "st_ctime_ns": 4}
        before = types.SimpleNamespace(**attributes, st_atime_ns=10)
        after = types.SimpleNamespace(**attributes, st_atime_ns=11)
        with mock.patch.object(self.materializer.os, "fstat", side_effect=(before, after)):
            content, _ = self.materializer._regular_bytes(file)
        self.assertEqual(content, b"package Module; 1;\n")

    def test_long_alias_chain_is_validated_in_linear_work(self):
        records = [{"path": "bin/perl", "type": "file"}]
        for index in range(80):
            next_name = f"link{index + 1}" if index < 79 else "perl"
            records.append({"path": f"bin/link{index}", "type": "symlink",
                            "target": next_name})
        with mock.patch.object(self.materializer, "_link_target",
                               wraps=self.materializer._link_target) as resolution:
            self.materializer._validate_links(records)
        self.assertLessEqual(resolution.call_count, 2 * len(records))

    def test_mode_change_between_stat_and_open_cannot_authenticate_file(self):
        source = self.root / "source"
        source.mkdir()
        self.fixture(source)
        destination, manifest, _ = self.materialize(source)
        original = os.stat
        changed = False

        def mode_swap(path, *args, **kwargs):
            nonlocal changed
            info = original(path, *args, **kwargs)
            if (path == "perl" and kwargs.get("dir_fd") is not None
                    and not kwargs.get("follow_symlinks", True) and not changed):
                changed = True
                (destination / "bin/perl").chmod(0o711)
            return info

        with mock.patch.object(self.materializer.os, "stat", side_effect=mode_swap):
            with self.assertRaises(self.materializer.MaterializationError):
                self.materializer.verify_materialized(destination, manifest, "x86_64")
        self.assertTrue(changed)

    def test_symlink_with_missing_intermediate_component_is_not_normalized_away(self):
        for target in ("missing/../perl", "perl/../perl"):
            with self.subTest(target=target):
                source = self.root / ("source-" + target.split("/")[0])
                source.mkdir()
                self.fixture(source)
                alias = source / "bin/perl-alias"
                alias.unlink()
                alias.symlink_to(target)
                with self.assertRaises(self.materializer.MaterializationError):
                    self.materialize(source, "bad-intermediate-" + target.split("/")[0])

    def test_reentered_source_symlink_is_rejected_before_creating_output(self):
        source = self.root / "source"
        source.mkdir()
        self.fixture(source)
        alias = source / "bin/perl-alias"
        alias.unlink()
        alias.symlink_to("../../source/bin/perl")
        with self.assertRaises(self.materializer.MaterializationError):
            self.materialize(source, "nonportable")
        self.assertFalse((self.root / "nonportable").exists())
        self.assertFalse((self.root / "nonportable/manifest.json").exists())

    def test_output_file_directory_root_and_manifest_modes_are_verified(self):
        for kind in ("file", "directory", "root", "manifest"):
            with self.subTest(kind=kind):
                source = self.root / (kind + "-source")
                source.mkdir()
                self.fixture(source)
                destination, manifest, _ = self.materialize(source, kind + "-out")
                changed = {"file": destination / "bin/perl", "directory": destination / "bin",
                           "root": destination, "manifest": manifest}[kind]
                changed.chmod(0o711 if kind == "file" else 0o777)
                with self.assertRaises(self.materializer.MaterializationError):
                    self.materializer.verify_materialized(destination, manifest, "x86_64")

    def test_manifest_alias_into_output_is_rejected_before_staging(self):
        source = self.root / "source"
        source.mkdir()
        self.fixture(source)
        destination = self.root / "output"
        alias = self.root / "alias"
        alias.symlink_to("output")
        with self.assertRaises(self.materializer.MaterializationError):
            self.materializer.materialize(source, destination, alias / "snapshot.json", "x86_64")
        self.assertFalse(destination.exists())

    def test_canonical_manifest_rejects_boolean_or_float_numeric_substitution(self):
        for kind in ("schema", "mode", "size"):
            with self.subTest(kind=kind):
                source = self.root / (kind + "-source")
                source.mkdir()
                self.fixture(source)
                destination, manifest, document = self.materialize(source, kind + "-out")
                if kind == "schema":
                    document["schema_version"] = True
                else:
                    entry = next(record for record in document["entries"]
                                 if record["type"] == "file")
                    entry[kind] = float(entry[kind])
                manifest.write_bytes(self.materializer._canonical(document))
                with self.assertRaises(self.materializer.MaterializationError):
                    self.materializer.verify_materialized(destination, manifest, "x86_64")

    def test_directory_listing_stops_at_count_budget_before_sorting_all_entries(self):
        source = self.root / "source"
        source.mkdir()
        self.fixture(source)
        for index in range(8):
            (source / f"extra-{index}").write_bytes(b"x")
        original = self.materializer.os.scandir
        seen = 0

        class CountingIterator:
            def __init__(self, iterator):
                self.iterator = iterator

            def __iter__(self):
                return self

            def __next__(self):
                nonlocal seen
                entry = next(self.iterator)
                seen += 1
                return entry

            def __enter__(self):
                return self

            def __exit__(self, *_):
                self.iterator.close()

        def counted(path):
            return CountingIterator(original(path))

        with mock.patch.object(self.materializer, "MAX_FILES", 3), \
                mock.patch.object(self.materializer.os, "scandir", side_effect=counted):
            with self.assertRaises(self.materializer.MaterializationError):
                self.materialize(source, "budget")
        self.assertLessEqual(seen, 4)

    def test_racing_empty_destination_is_not_replaced(self):
        source = self.root / "source"
        source.mkdir()
        self.fixture(source)
        destination = self.root / "output"
        manifest = destination / "manifest.json"
        original = self.materializer._publish_no_replace

        def race(staged, final):
            final.mkdir()
            original(staged, final)

        with mock.patch.object(self.materializer, "_publish_no_replace", side_effect=race):
            with self.assertRaises(self.materializer.MaterializationError):
                self.materializer.materialize(source, destination, manifest, "x86_64")
        self.assertTrue(destination.is_dir())
        self.assertEqual(list(destination.iterdir()), [])

    def test_failed_manifest_write_does_not_publish_tree(self):
        source = self.root / "source"
        source.mkdir()
        self.fixture(source)
        destination = self.root / "output"
        manifest = destination / "manifest.json"
        original = pathlib.Path.open

        def fail_manifest(path, mode="r", *args, **kwargs):
            if path.name == "manifest.json" and mode == "xb":
                raise OSError("synthetic manifest write failure")
            return original(path, mode, *args, **kwargs)

        with mock.patch.object(pathlib.Path, "open", fail_manifest):
            with self.assertRaises(self.materializer.MaterializationError):
                self.materializer.materialize(source, destination, manifest, "x86_64")
        self.assertFalse(destination.exists())
        self.assertFalse(manifest.exists())

    def test_directory_swap_cannot_read_files_outside_source_root(self):
        source = self.root / "source"
        source.mkdir()
        self.fixture(source)
        (source / "sub").mkdir()
        (source / "sub/host").write_bytes(b"approved")
        external = self.root / "external"
        external.mkdir()
        (external / "host").write_bytes(b"outside")
        original_lstat = os.lstat
        original_stat = os.stat
        swapped = False

        def swap():
            nonlocal swapped
            if not swapped:
                swapped = True
                (source / "sub").rename(source / "sub-saved")
                (source / "sub").symlink_to(external, target_is_directory=True)

        def lstat(path, *args, **kwargs):
            info = original_lstat(path, *args, **kwargs)
            if pathlib.Path(path) == source / "sub" and stat.S_ISDIR(info.st_mode):
                swap()
            return info

        def stat_relative(path, *args, **kwargs):
            info = original_stat(path, *args, **kwargs)
            if path == "sub" and kwargs.get("dir_fd") is not None and not kwargs.get("follow_symlinks", True):
                swap()
            return info

        with mock.patch.object(self.materializer.os, "lstat", side_effect=lstat), \
                mock.patch.object(self.materializer.os, "stat", side_effect=stat_relative):
            with self.assertRaises(self.materializer.MaterializationError):
                self.materialize(source, "swapped")
        self.assertTrue(swapped)
        self.assertFalse((self.root / "swapped").exists())

    def test_hardlink_special_file_size_and_destination_collision_fail_closed(self):
        for kind in ("hardlink", "fifo", "oversize", "destination", "manifest", "reserved"):
            with self.subTest(kind=kind):
                source = self.root / (kind + "-source")
                source.mkdir()
                self.fixture(source)
                name = kind + "-output"
                if kind == "hardlink":
                    os.link(source / "bin/perl", source / "bin/perl-hardlink")
                elif kind == "fifo":
                    os.mkfifo(source / "bin/fifo")
                elif kind == "destination":
                    (self.root / name).mkdir()
                elif kind == "manifest":
                    (self.root / name).mkdir()
                    (self.root / name / "manifest.json").write_text("sentinel", encoding="utf-8")
                elif kind == "reserved":
                    (source / "manifest.json").write_text("untrusted", encoding="utf-8")
                if kind == "oversize":
                    with mock.patch.object(self.materializer, "MAX_FILE_BYTES", 4):
                        with self.assertRaises(self.materializer.MaterializationError):
                            self.materialize(source, name)
                else:
                    with self.assertRaises(self.materializer.MaterializationError):
                        self.materialize(source, name)
                if kind == "manifest":
                    self.assertEqual((self.root / name / "manifest.json").read_text(), "sentinel")


if __name__ == "__main__":
    unittest.main()
