"""Read-only source-proof tests using synthetic archives, not upstream licenses."""
from __future__ import annotations

import hashlib
import io
import json
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = pathlib.Path(__file__).parents[2]
VERIFIER = ROOT / "scripts" / "verify_tsan_qt_sources.py"
VERSION = "5.15.19"
MODULES = ("qtbase", "qtsvg", "qttools", "qttranslations")
ARGUMENTS = ("QTBASE_SHA256", "QTSVG_SHA256", "QTTOOLS_SHA256", "QTTRANSLATIONS_SHA256")
PATCH = "fix_qt5_qobject_tsan_publication.patch"
APT_MANIFESTS = ("bootstrap-inputs.json", "source-inputs.json", "qt-builder-inputs.json", "runtime-inputs.json")


def digest(data):
    return hashlib.sha256(data).hexdigest()


class QtSourceProofTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="qt-source-proof-")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.recipe = self.repo / "docker" / "ubuntu22.04-tsan"
        self.sources = self.root / "qt-sources"
        self.expected_licenses = {}
        self.recipe.mkdir(parents=True)
        for directory in ("archives", "licenses", "recipe", "patches", "apt"):
            (self.sources / directory).mkdir(parents=True)
        arguments = ["ARG QT_VERSION=" + VERSION]
        for module, argument in zip(MODULES, ARGUMENTS):
            filename = module + "-everywhere-opensource-src-" + VERSION + ".tar.xz"
            archive = self.sources / "archives" / filename
            license_text = ("Synthetic license fixture for " + module + "\n").encode("ascii")
            with tarfile.open(archive, "w:xz", format=tarfile.USTAR_FORMAT) as output:
                for name, data in (("LICENSE.FIXTURE", license_text),
                                   ("src/not-executed.sh", b"#!/bin/sh\ntouch SHOULD_NOT_EXECUTE\n")):
                    member = tarfile.TarInfo(module + "-everywhere-src-" + VERSION + "/" + name)
                    member.size = len(data)
                    member.mtime = 0
                    output.addfile(member, io.BytesIO(data))
            arguments.append("ARG " + argument + "=" + digest(archive.read_bytes()))
            license_path = self.sources / "licenses" / module / "LICENSE.FIXTURE"
            license_path.parent.mkdir()
            license_path.write_bytes(license_text)
            self.expected_licenses[license_path.relative_to(self.sources).as_posix()] = digest(license_text)
        self.repo_file("Dockerfile", ("\n".join(arguments) + "\n").encode("ascii"), "recipe/Dockerfile")
        self.repo_file("README.md", b"Fixture build and license instructions.\n", "README.md")
        self.repo_file("patches/" + PATCH, b"Fixture patch bytes, never executed.\n", "patches/" + PATCH)
        for helper in ("apt_snapshot_retry.sh", "verify_elf_runtime_closure.sh"):
            self.repo_file(helper, b"#!/bin/sh\nexit 99\n", "recipe/" + helper)
        (self.repo / "scripts").mkdir()
        (self.repo / "scripts" / "ci_install_locked_apt.sh").write_bytes(b"#!/bin/sh\nexit 98\n")

    def repo_file(self, relative, data, payload):
        origin = self.recipe / relative
        origin.parent.mkdir(parents=True, exist_ok=True)
        origin.write_bytes(data)
        (self.sources / payload).write_bytes(data)

    def locked_provenance(self):
        shutil.copyfile(self.repo / "scripts" / "ci_install_locked_apt.sh",
                        self.sources / "recipe" / "ci_install_locked_apt.sh")
        for name in APT_MANIFESTS:
            document = {"schema_version": 1, "kind": "apt-inputs", "stage": name[:-len("-inputs.json")],
                        "platform": "linux/amd64", "base_image": "example.invalid/base@sha256:" + "a" * 64,
                        "packages": [{"package": "fixture", "version": "1"}]}
            (self.sources / "apt" / name).write_text(json.dumps(document) + "\n", encoding="ascii")

    def verify(self, *options):
        return subprocess.run(
            [sys.executable, str(VERIFIER), "--sources", str(self.sources), "--repo-root", str(self.repo), *options],
            cwd=self.root, capture_output=True, text=True, check=False, timeout=20,
        )

    def test_complete_online_source_payload_is_verified_without_execution_or_extraction(self):
        before = {path.relative_to(self.sources).as_posix(): path.read_bytes()
                  for path in self.sources.rglob("*") if path.is_file()}
        result = self.verify()
        self.assertEqual(result.returncode, 0, result.stderr)
        proof = json.loads(result.stdout)
        self.assertEqual(proof["qt_version"], VERSION)
        self.assertEqual(len(proof["archives"]), 4)
        self.assertEqual(proof["license_files"], self.expected_licenses)
        self.assertFalse(proof["locked_provenance"])
        after = {path.relative_to(self.sources).as_posix(): path.read_bytes()
                 for path in self.sources.rglob("*") if path.is_file()}
        self.assertEqual(before, after)
        self.assertFalse((self.root / "SHOULD_NOT_EXECUTE").exists())
        self.assertFalse(any(self.root.glob("*-everywhere-src-*")), "verifier extracted source code")

    def test_locked_qualification_requires_actual_provenance_and_offline_helper_identity(self):
        rejected = self.verify("--require-locked")
        self.assertNotEqual(rejected.returncode, 0)
        self.locked_provenance()
        accepted = self.verify("--require-locked")
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertTrue(json.loads(accepted.stdout)["locked_provenance"])
        for relative in ("apt/qt-builder-inputs.json", "recipe/ci_install_locked_apt.sh"):
            path = self.sources / relative
            original = path.read_bytes()
            path.unlink()
            with self.subTest(missing=relative):
                self.assertNotEqual(self.verify("--require-locked").returncode, 0)
            path.write_bytes(original)
        (self.sources / "recipe" / "ci_install_locked_apt.sh").write_bytes(b"changed installer")
        self.assertNotEqual(self.verify("--require-locked").returncode, 0)

    def test_every_original_archive_is_required_and_hash_bound_to_current_recipe(self):
        for module in MODULES:
            path = self.sources / "archives" / (module + "-everywhere-opensource-src-" + VERSION + ".tar.xz")
            original = path.read_bytes()
            for mutation in ("missing", "tampered"):
                with self.subTest(module=module, mutation=mutation):
                    if mutation == "missing":
                        path.unlink()
                    else:
                        path.write_bytes(original + b"changed")
                    self.assertNotEqual(self.verify().returncode, 0)
                    path.write_bytes(original)

    def test_patch_recipe_instructions_and_helpers_must_match_trusted_checkout(self):
        for relative in ("recipe/Dockerfile", "README.md", "patches/" + PATCH,
                         "recipe/apt_snapshot_retry.sh", "recipe/verify_elf_runtime_closure.sh"):
            with self.subTest(relative=relative):
                path = self.sources / relative
                original = path.read_bytes()
                path.write_bytes(original + b"unreviewed change\n")
                self.assertNotEqual(self.verify().returncode, 0)
                path.write_bytes(original)

    def test_license_copies_must_be_exact_original_archive_members(self):
        path = self.sources / "licenses" / "qtbase" / "LICENSE.FIXTURE"
        original = path.read_bytes()
        for mutation in ("missing", "tampered", "invented"):
            with self.subTest(mutation=mutation):
                extra = path.with_name("LICENSE.INVENTED")
                if mutation == "missing":
                    path.unlink()
                elif mutation == "tampered":
                    path.write_bytes(b"not the upstream license bytes")
                else:
                    extra.write_bytes(b"invented license")
                self.assertNotEqual(self.verify().returncode, 0)
                path.write_bytes(original)
                extra.unlink(missing_ok=True)

    def test_links_extra_archives_and_expanded_source_trees_are_rejected(self):
        helper = self.sources / "recipe" / "apt_snapshot_retry.sh"
        original = helper.read_bytes()
        helper.unlink()
        helper.symlink_to(self.recipe / "apt_snapshot_retry.sh")
        self.assertNotEqual(self.verify().returncode, 0)
        helper.unlink()
        helper.write_bytes(original)
        extra = self.sources / "archives" / "unverified.tar.xz"
        extra.write_bytes(b"extra")
        self.assertNotEqual(self.verify().returncode, 0)
        extra.unlink()
        expanded = self.sources / "qtbase-everywhere-src-5.15.19"
        expanded.mkdir()
        (expanded / "implementation.cpp").write_bytes(b"not a compressed source payload")
        self.assertNotEqual(self.verify().returncode, 0)

    def test_partial_or_inconsistent_locked_provenance_is_not_reported_as_verified(self):
        self.locked_provenance()
        path = self.sources / "apt" / "source-inputs.json"
        path.write_text("{}\n", encoding="ascii")
        self.assertNotEqual(self.verify().returncode, 0)
        self.locked_provenance()
        document = json.loads(path.read_text())
        document["base_image"] = "example.invalid/other@sha256:" + "b" * 64
        path.write_text(json.dumps(document), encoding="ascii")
        self.assertNotEqual(self.verify().returncode, 0)

    def test_conflicting_recipe_version_defaults_fail_closed(self):
        path = self.recipe / "Dockerfile"
        path.write_bytes(path.read_bytes() + b"ARG QT_VERSION=5.15.18\n")
        shutil.copyfile(path, self.sources / "recipe" / "Dockerfile")
        self.assertNotEqual(self.verify().returncode, 0)


if __name__ == "__main__":
    unittest.main()
