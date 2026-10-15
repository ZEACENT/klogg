import argparse
import gzip
import shutil
import hashlib
import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).parents[2]
PREFETCH_SCRIPT = ROOT / "scripts" / "prefetch_adb_helper_sources.py"
LEGAL_SCRIPT = ROOT / "scripts" / "build_adb_helper_legal_assets.py"
BUILD_SCRIPT = ROOT / "scripts" / "build_adb_helper.py"
SMOKE_SCRIPT = ROOT / "scripts" / "smoke_adb_helper.py"
LOCK = ROOT / "packaging" / "adb" / "adb-helper.lock.json"
CANONICAL_TAR_GZ_IDENTITY = "canonical-tar-gz-v1"


def load_build_module():
    spec = importlib.util.spec_from_file_location("build_adb_helper", BUILD_SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def load_smoke_module():
    spec = importlib.util.spec_from_file_location("smoke_adb_helper", SMOKE_SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def load_prefetch_module():
    spec = importlib.util.spec_from_file_location("prefetch_adb_helper_sources", PREFETCH_SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def load_legal_module():
    with mock.patch.object(sys, "path", [str(ROOT / "scripts"), *sys.path]):
        spec = importlib.util.spec_from_file_location("build_adb_helper_legal_assets", LEGAL_SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    return module


def archive_sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def add_bytes(tar: tarfile.TarFile, name: str, content: bytes) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(content)
    info.mode = 0o644
    tar.addfile(info, io.BytesIO(content))


def canonical_tar_gz_bytes(name: str, content: bytes, mode: int = 0o644) -> bytes:
    output = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as tar:
            info = tarfile.TarInfo(name)
            info.size = len(content)
            info.mode = mode
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            tar.addfile(info, io.BytesIO(content))
    return output.getvalue()


class AdbHelperSourceHardeningContractTest(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tempdir.name).resolve()

    def tearDown(self):
        self.tempdir.cleanup()

    def write_lock(self, record: dict) -> pathlib.Path:
        path = self.root / "lock.json"
        path.write_text(json.dumps({"sources": [record], "dependencies": []}), encoding="utf-8")
        return path

    def test_verified_extractor_accepts_tar_of_current_directory_contents(self):
        artifact_root = self.root / "artifact"
        nested = artifact_root / "bin"
        nested.mkdir(parents=True)
        executable = nested / "adb"
        executable.write_bytes(b"adb helper\n")
        executable.chmod(0o755)
        (artifact_root / "SHA256SUMS").write_text("receipt\n", encoding="utf-8")
        archive = self.root / "artifact.tar.gz"
        subprocess.run(
            ["tar", "-czf", str(archive), "-C", str(artifact_root), "."],
            check=True,
        )

        extract_root = self.root / "extract-artifact"
        load_prefetch_module().safe_extract(archive, extract_root)

        extracted = extract_root / "bin" / "adb"
        self.assertEqual(extracted.read_bytes(), b"adb helper\n")
        if sys.platform != "win32":
            self.assertEqual(extracted.stat().st_mode & 0o777, 0o755)

    def test_verified_extractor_preserves_sole_directory_below_explicit_root(self):
        archive = self.root / "explicit-root-directory.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            root = tarfile.TarInfo(".")
            root.type = tarfile.DIRTYPE
            root.mode = 0o755
            tar.addfile(root)
            directory = tarfile.TarInfo("./helpers")
            directory.type = tarfile.DIRTYPE
            directory.mode = 0o755
            tar.addfile(directory)
            add_bytes(tar, "./helpers/adb", b"adb helper\n")

        extract_root = self.root / "extract-explicit-root-directory"
        load_prefetch_module().safe_extract(archive, extract_root)

        self.assertEqual(
            (extract_root / "helpers" / "adb").read_bytes(), b"adb helper\n"
        )
        self.assertFalse((extract_root / "adb").exists())

    def test_verified_extractor_rejects_root_only_archive(self):
        archive = self.root / "empty-artifact.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            root = tarfile.TarInfo(".")
            root.type = tarfile.DIRTYPE
            root.mode = 0o755
            tar.addfile(root)

        with self.assertRaisesRegex(RuntimeError, "no real members"):
            load_prefetch_module().safe_extract(
                archive, self.root / "extract-empty-artifact"
            )

    def run_prefetch(self, lock: pathlib.Path, download_root: pathlib.Path, extract_root=None):
        command = [
            sys.executable,
            str(PREFETCH_SCRIPT),
            "--lock",
            str(lock),
            "--download-root",
            str(download_root),
            "--offline",
        ]
        if extract_root is not None:
            command.extend(("--extract-root", str(extract_root)))
        return subprocess.run(command, capture_output=True, text=True, timeout=10, check=False)

    def test_gitiles_archives_use_canonical_content_identity_in_the_real_lock(self):
        lock = json.loads(LOCK.read_text(encoding="utf-8"))
        gitiles = [
            record
            for record in lock.get("sources", [])
            if "android.googlesource.com" in str(record.get("archive_url", ""))
        ]
        self.assertTrue(gitiles, "the ADB closure must contain locked Gitiles sources")
        for record in gitiles:
            with self.subTest(source=record.get("id")):
                self.assertEqual(
                    record.get("archive_identity"),
                    CANONICAL_TAR_GZ_IDENTITY,
                    "Gitiles stamps each generated tar member with request time, so raw "
                    "archive bytes are not an immutable source identity",
                )

    def test_prefetch_canonicalizes_volatile_gitiles_metadata_before_hashing(self):
        download_root = self.root / "downloads"
        download_root.mkdir()
        archive = download_root / "gitiles-source.tar.gz"
        content = b"immutable source payload\n"
        canonical = canonical_tar_gz_bytes("source.txt", content)
        record = {
            "id": "gitiles-source",
            "archive_file": archive.name,
            "archive_sha256": hashlib.sha256(canonical).hexdigest(),
            "archive_identity": CANONICAL_TAR_GZ_IDENTITY,
            "build_input": False,
        }
        lock = self.write_lock(record)

        for request_time in (1_700_000_000, 1_800_000_000):
            with self.subTest(request_time=request_time):
                with tarfile.open(archive, "w:gz") as tar:
                    info = tarfile.TarInfo("source.txt")
                    info.size = len(content)
                    info.mode = 0o644
                    info.mtime = request_time
                    info.uid = 123
                    info.gid = 456
                    info.uname = "volatile"
                    info.gname = "metadata"
                    tar.addfile(info, io.BytesIO(content))

                result = self.run_prefetch(lock, download_root)

                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(archive.read_bytes(), canonical)

        with tarfile.open(archive, "w:gz") as tar:
            info = tarfile.TarInfo("source.txt")
            changed = b"different source payload\n"
            info.size = len(changed)
            info.mode = 0o644
            info.mtime = 1_900_000_000
            tar.addfile(info, io.BytesIO(changed))
        result = self.run_prefetch(lock, download_root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sha256 mismatch", (result.stdout + result.stderr).lower())

    def test_prefetch_rejects_unknown_archive_identity_fail_closed(self):
        download_root = self.root / "downloads"
        download_root.mkdir()
        archive = download_root / "source.tar"
        with tarfile.open(archive, "w") as tar:
            add_bytes(tar, "payload", b"payload")
        lock = self.write_lock(
            {
                "id": "source",
                "archive_file": archive.name,
                "archive_sha256": archive_sha256(archive),
                "archive_identity": "unknown-identity-v9",
                "build_input": False,
            }
        )

        result = self.run_prefetch(lock, download_root)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("archive identity", (result.stdout + result.stderr).lower())

    def test_prefetch_rejects_unsafe_record_paths_and_unsupported_archive_members(self):
        download_root = self.root / "downloads"
        download_root.mkdir()

        outside = self.root / "outside.tar"
        with tarfile.open(outside, "w") as tar:
            add_bytes(tar, "payload", b"payload")
        lock = self.write_lock(
            {
                "id": "source",
                "archive_file": "../outside.tar",
                "archive_sha256": archive_sha256(outside),
                "build_input": False,
            }
        )
        result = self.run_prefetch(lock, download_root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsafe", (result.stdout + result.stderr).lower())

        scenarios = ("fifo", "hardlink", "backslash-symlink")
        for scenario in scenarios:
            with self.subTest(member=scenario):
                archive = download_root / f"{scenario}.tar"
                with tarfile.open(archive, "w") as tar:
                    add_bytes(tar, "target", b"target")
                    member = tarfile.TarInfo(scenario)
                    if scenario == "fifo":
                        member.type = tarfile.FIFOTYPE
                    elif scenario == "hardlink":
                        member.type = tarfile.LNKTYPE
                        member.linkname = "target"
                    else:
                        member.type = tarfile.SYMTYPE
                        member.linkname = r"..\outside"
                    tar.addfile(member)
                lock = self.write_lock(
                    {
                        "id": scenario,
                        "archive_file": archive.name,
                        "archive_sha256": archive_sha256(archive),
                        "build_input": True,
                    }
                )
                result = self.run_prefetch(lock, download_root, self.root / f"extract-{scenario}")
                self.assertNotEqual(result.returncode, 0)
                expected_error = "backslash" if scenario == "backslash-symlink" else "unsupported"
                self.assertIn(expected_error, (result.stdout + result.stderr).lower())

    def test_prefetch_rejects_symlink_that_escapes_after_single_root_flattening(self):
        download_root = self.root / "downloads"
        download_root.mkdir()
        archive = download_root / "flatten-escape.tar"
        with tarfile.open(archive, "w") as tar:
            add_bytes(tar, "source/payload", b"payload")
            member = tarfile.TarInfo("source/escape")
            member.type = tarfile.SYMTYPE
            member.linkname = "../outside"
            tar.addfile(member)
        lock = self.write_lock(
            {
                "id": "source",
                "archive_file": archive.name,
                "archive_sha256": archive_sha256(archive),
                "build_input": True,
            }
        )

        result = self.run_prefetch(lock, download_root, self.root / "extract")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("escapes extraction root", (result.stdout + result.stderr).lower())

    def test_prefetch_normalizes_relative_symlink_targets_for_windows(self):
        module = load_prefetch_module()
        self.assertEqual(
            module.platform_symlink_target(
                "../libcutils/include/cutils/", platform_name="nt"
            ),
            r"..\libcutils\include\cutils",
        )
        self.assertEqual(
            module.platform_symlink_target("testdata/", platform_name="nt"),
            "testdata",
        )

    def test_prefetch_marks_directory_symlinks_for_windows_reparse_points(self):
        archive = self.root / "directory-symlink.tar"
        with tarfile.open(archive, "w") as tar:
            for name in (
                "source",
                "source/libcutils",
                "source/libcutils/include",
                "source/libcutils/include/cutils",
                "source/include",
            ):
                directory = tarfile.TarInfo(name)
                directory.type = tarfile.DIRTYPE
                directory.mode = 0o755
                tar.addfile(directory)
            add_bytes(
                tar,
                "source/libcutils/include/cutils/list.h",
                b"directory symlink payload\n",
            )
            link = tarfile.TarInfo("source/include/cutils")
            link.type = tarfile.SYMTYPE
            link.linkname = "../libcutils/include/cutils"
            tar.addfile(link)

        module = load_prefetch_module()
        real_symlink = module.os.symlink
        extract_root = self.root / "extract-directory-link"
        with mock.patch.object(module.os, "symlink", wraps=real_symlink) as symlink:
            module.safe_extract(archive, extract_root)

        symlink.assert_called_once_with(
            module.platform_symlink_target( "../libcutils/include/cutils" ),
            self.root / "extract-directory-link/source/include/cutils",
            target_is_directory=True,
        )
        self.assertEqual(
            (extract_root / "include/cutils/list.h").read_bytes(),
            b"directory symlink payload\n",
        )

    def test_prefetch_preserves_broken_directory_symlink_marked_by_trailing_slash(self):
        archive = self.root / "broken-directory-symlink.tar"
        with tarfile.open(archive, "w") as tar:
            add_bytes(tar, "source/payload", b"payload")
            link = tarfile.TarInfo("source/testdata-link")
            link.type = tarfile.SYMTYPE
            link.linkname = "testdata/"
            tar.addfile(link)

        module = load_prefetch_module()
        real_symlink = module.os.symlink
        extract_root = self.root / "extract-broken-directory-link"
        with mock.patch.object(module.os, "symlink", wraps=real_symlink) as symlink:
            module.safe_extract(archive, extract_root)

        symlink.assert_called_once_with(
            module.platform_symlink_target( "testdata/" ),
            self.root / "extract-broken-directory-link/source/testdata-link",
            target_is_directory=True,
        )
        self.assertTrue((extract_root / "testdata-link").is_symlink())

    def test_prefetch_omits_only_exact_lock_pinned_build_irrelevant_symlink(self):
        download_root = self.root / "downloads"
        download_root.mkdir()
        archive = download_root / "sanitized-symlink.tar"
        with tarfile.open(archive, "w") as tar:
            add_bytes(tar, "source/payload", b"payload")
            member = tarfile.TarInfo("source/rustfmt.toml")
            member.type = tarfile.SYMTYPE
            member.linkname = "../../build/soong/scripts/rustfmt.toml"
            tar.addfile(member)
        record = {
            "id": "source",
            "archive_file": archive.name,
            "archive_sha256": archive_sha256(archive),
            "build_input": True,
            "excluded_build_symlinks": [
                {
                    "path": "source/rustfmt.toml",
                    "target": "../../build/soong/scripts/rustfmt.toml",
                    "reason": "Formatting metadata is not a production build input.",
                }
            ],
        }
        lock = self.write_lock(record)
        extract_root = self.root / "extract"

        result = self.run_prefetch(lock, download_root, extract_root)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((extract_root / "source/payload").is_file())
        self.assertFalse((extract_root / "source/rustfmt.toml").is_symlink())
        manifest = json.loads(
            (download_root / "adb-helper-prefetch-manifest.json").read_text(encoding="utf-8")
        )
        self.assertEqual(
            manifest["archives"][0]["excluded_build_symlinks"],
            record["excluded_build_symlinks"],
        )

        record["excluded_build_symlinks"][0]["target"] = "../../../changed-target"
        lock = self.write_lock(record)
        result = self.run_prefetch(lock, download_root, extract_root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("excluded build symlink", (result.stdout + result.stderr).lower())

    def make_legal_fixture(self, root=None):
        root = root or self.root
        repository = root / "repository"
        archive_root = root / "archives"
        archive_root.mkdir()
        for relative, content in {
            "COPYING": "Klogg license\n",
            "NOTICE": "Klogg notice\n",
            "packaging/adb/README.md": "Build instructions\n",
            "packaging/adb/superbuild/CMakeLists.txt": "project(adb)\n",
            "scripts/prefetch_adb_helper_sources.py": "# prefetch\n",
            "scripts/prefetch_adb_manifest_fallback.py": "# manifest fallback\n",
            "scripts/prefetch_adb_source_context.py": "# source context\n",
            "scripts/prefetch_adb_libusb_fallback.py": "# libusb fallback\n",
            "scripts/prefetch_adb_source_closure.py": "# source closure\n",
            "scripts/ci_adb_source_transport.py": "# ordinary source transport\n",
            "scripts/build_adb_helper.py": "# build\n",
            "scripts/build_adb_helper_legal_assets.py": "# legal\n",
            "scripts/source_publication_identity.py": "# identity\n",
            "scripts/extract_verified_tar.py": "# extract\n",
            "scripts/verify_adb_helper_toolchain.py": "# toolchain\n",
            "scripts/verify_adb_helper_artifact.py": "# verify\n",
            "scripts/verify_adb_helper_envelope.py": "# envelope\n",
            "scripts/smoke_adb_helper.py": "# smoke\n",
        }.items():
            path = repository / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")

        source_archive = archive_root / "libusb-source.tar.gz"
        with tarfile.open(source_archive, "w:gz") as tar:
            add_bytes(tar, "libusb-source/COPYING", b"GNU LESSER GENERAL PUBLIC LICENSE\n")
            add_bytes(tar, "libusb-source/NOTICE", b"libusb authors\n")

        release_assets = [
            ("source-archive", "adb-helper-source-archive.tar.gz"),
            ("licenses", "adb-helper-licenses.tar.gz"),
            ("notices", "adb-helper-notices.tar.gz"),
            ("sbom", "adb-helper-sbom.spdx.json"),
            ("source-offer", "ADB-HELPER-SOURCE-OFFER.txt"),
            ("source-manifest", "adb-helper-source-manifest.json"),
            ("source-set-receipt", "adb-helper-source-set-receipt.json"),
            ("overlay-receipt", "adb-helper-overlay-receipt.json"),
        ]
        lock = {
            "schema_version": 1,
            "helper": {"kind": "complete-adb-executable"},
            "sources": [
                {
                    "id": "aosp-libusb",
                    "repository_url": "https://example.invalid/libusb",
                    "archive_url": "https://example.invalid/libusb.tar.gz",
                    "archive_file": source_archive.name,
                    "archive_sha256": archive_sha256(source_archive),
                    "commit": "1" * 40,
                    "build_input": True,
                    "legal": {
                        "licenses": ["LGPL-2.1-or-later"],
                        "license_files": ["COPYING"],
                        "notices": ["libusb authors"],
                        "notice_files": ["NOTICE"],
                    },
                }
            ],
            "dependencies": [],
            "patches": [],
            "toolchains": {},
            "targets": {},
            "release_assets": [
                {
                    "kind": kind,
                    "required": True,
                    "ownership": "overlay" if kind in ("source-offer", "overlay-receipt") else "core",
                    "distribution": {
                        "package_required": kind != "source-archive",
                        "release_required": True,
                    },
                    "file_name": name,
                    "sha256_file": name + ".sha256",
                }
                for kind, name in release_assets
            ],
        }
        lock_path = repository / "packaging/adb/adb-helper.lock.json"
        lock_path.parent.mkdir(parents=True, exist_ok=True)
        lock_path.write_text(json.dumps(lock), encoding="utf-8")
        return repository, archive_root, lock_path, lock

    def build_legal_assets(
        self, repository, archive_root, lock, output, package_support_output=None,
        version="26.08.27",
    ):
        command = [
            sys.executable,
            str(LEGAL_SCRIPT),
            "--lock",
            str(lock),
            "--archive-root",
            str(archive_root),
            "--repository-root",
            str(repository),
            "--version",
            version,
            "--base-url",
            "https://github.com/ZEACENT/klogg",
            "--output",
            str(output),
        ]
        if package_support_output is not None:
            command.extend(("--package-support-output", str(package_support_output)))
        return subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def test_legal_assets_reject_unsafe_upstream_legal_member_paths(self):
        repository, archive_root, lock_path, lock = self.make_legal_fixture()
        source_archive = archive_root / lock["sources"][0]["archive_file"]
        with tarfile.open(source_archive, "w:gz") as tar:
            add_bytes(tar, "../../COPYING", b"escaped license\n")
        lock["sources"][0]["archive_sha256"] = archive_sha256(source_archive)
        lock_path.write_text(json.dumps(lock), encoding="utf-8")

        result = self.build_legal_assets(
            repository, archive_root, lock_path, self.root / "release-unsafe"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsafe", (result.stdout + result.stderr).lower())

    def test_legal_assets_are_reproducible_and_include_complete_build_material(self):
        repository, archive_root, lock_path, lock = self.make_legal_fixture()
        first = self.root / "release-first"
        second = self.root / "release-second"
        result = self.build_legal_assets(repository, archive_root, lock_path, first)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        time.sleep(1.1)
        result = self.build_legal_assets(repository, archive_root, lock_path, second)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        for asset in lock["release_assets"]:
            name = asset["file_name"]
            self.assertEqual(archive_sha256(first / name), archive_sha256(second / name), name)

        source_archive = first / "adb-helper-source-archive.tar.gz"
        with tarfile.open(source_archive, "r:gz") as tar:
            members = set(tar.getnames())
        required_build_material = {
            "packaging/adb/README.md",
            "packaging/adb/superbuild/CMakeLists.txt",
            "scripts/prefetch_adb_helper_sources.py",
            "scripts/prefetch_adb_manifest_fallback.py",
            "scripts/prefetch_adb_source_context.py",
            "scripts/prefetch_adb_libusb_fallback.py",
            "scripts/prefetch_adb_source_closure.py",
            "scripts/ci_adb_source_transport.py",
            "scripts/build_adb_helper.py",
            "scripts/build_adb_helper_legal_assets.py",
            "scripts/source_publication_identity.py",
            "scripts/extract_verified_tar.py",
            "scripts/verify_adb_helper_toolchain.py",
            "scripts/verify_adb_helper_artifact.py",
            "scripts/verify_adb_helper_envelope.py",
            "scripts/smoke_adb_helper.py",
        }
        self.assertEqual(required_build_material - members, set())

    def test_acquisition_contracts_are_registered_with_ctest(self):
        cmake = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        for name, script in (
            ("adb_libusb_fallback_contract", "test_prefetch_adb_libusb_fallback.py"),
            ("adb_source_closure_contract", "test_prefetch_adb_source_closure.py"),
        ):
            with self.subTest(name=name):
                self.assertIn(
                    f"add_test(\n  NAME {name}\n"
                    f"  COMMAND ${{Python3_EXECUTABLE}} ${{CMAKE_SOURCE_DIR}}/tests/scripts/{script}\n)",
                    cmake,
                )
                timeout_tests = cmake.split("set_tests_properties(\n", 1)[1].split(
                    "PROPERTIES TIMEOUT 60", 1
                )[0]
                self.assertIn(f"  {name}\n", timeout_tests)

    def test_core_legal_assets_are_version_independent(self):
        # Immutable-core reuse requires the binary build contract to survive an
        # application version bump: only the versioned source offer (the
        # overlay) may change; the source-set receipt anchors the core identity
        # and must not embed overlay bytes.
        repository, archive_root, lock_path, lock = self.make_legal_fixture()
        first = self.root / "release-version-one"
        second = self.root / "release-version-two"
        for output, version in ((first, "26.08.27"), (second, "99.01.01")):
            result = self.build_legal_assets(
                repository, archive_root, lock_path, output, version=version
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        versioned = set()
        for asset in lock["release_assets"]:
            name = asset["file_name"]
            if archive_sha256(first / name) != archive_sha256(second / name):
                versioned.add(asset["kind"])
        self.assertEqual(versioned, {"source-offer", "overlay-receipt"})

        receipt = json.loads(
            (first / "adb-helper-source-set-receipt.json").read_text(encoding="utf-8")
        )
        embedded = {asset["kind"] for asset in receipt["package_support_assets"]}
        self.assertNotIn("source-offer", embedded)

        # The binary builder also records only core support hashes; the
        # source-set receipt is identical across these application versions.
        build = load_build_module()
        first_support = build.package_support_receipt_assets(lock, first)
        second_support = build.package_support_receipt_assets(lock, second)
        self.assertEqual(first_support, second_support)
        self.assertTrue(first_support)
        self.assertTrue(all(asset["kind"] not in ("source-offer", "overlay-receipt")
                            for asset in first_support))

    def test_version_overlay_materializes_from_verified_core_without_sources(self):
        repository, archive_root, lock_path, lock = self.make_legal_fixture()
        core = self.root / "immutable-core"
        core_build = subprocess.run(
            [sys.executable, str(LEGAL_SCRIPT), "--core-only", "--lock", str(lock_path),
             "--archive-root", str(archive_root), "--repository-root", str(repository),
             "--output", str(core)],
            capture_output=True, text=True, check=False, timeout=10,
        )
        self.assertEqual(core_build.returncode, 0, core_build.stdout + core_build.stderr)
        self.assertFalse((core / "ADB-HELPER-SOURCE-OFFER.txt").exists())
        self.assertFalse((core / "adb-helper-overlay-receipt.json").exists())
        self.assertTrue((core / "adb-helper-source-set-receipt.json").is_file())

        # The native helper core is already built. An application release
        # regenerates ONLY its versioned overlay, without access to the source
        # archives or the original generator checkout.
        pins = self.overlay_pin_arguments(core, lock_path, lock)
        archive_root.rename(self.root / "unavailable-archives")
        output = self.root / "versioned-assembly"
        support = self.root / "versioned-package-support"
        overlay_build = subprocess.run(
            [sys.executable, str(LEGAL_SCRIPT), "--overlay-only", "--lock", str(lock_path),
             "--core-root", str(core), "--version", "99.01.01",
             "--base-url", "https://github.com/ZEACENT/klogg",
             "--output", str(output), "--package-support-output", str(support), *pins],
            capture_output=True, text=True, check=False, timeout=10,
        )
        self.assertEqual(overlay_build.returncode, 0, overlay_build.stdout + overlay_build.stderr)
        self.assertEqual(
            (core / "adb-helper-source-set-receipt.json").read_bytes(),
            (output / "adb-helper-source-set-receipt.json").read_bytes(),
        )
        self.assertIn("klogg-v99.01.01-adb-helper-source-",
                      (output / "ADB-HELPER-SOURCE-OFFER.txt").read_text())
        self.assertTrue((support / "adb-helper-overlay-receipt.json").is_file())
        self.assertFalse((support / "adb-helper-source-archive.tar.gz").exists())

        # Both paths must assemble identical package and release bytes; no
        # receipt may silently change its identity during the cutover.
        (self.root / "unavailable-archives").rename(archive_root)
        direct = self.root / "direct-full-build"
        result = self.build_legal_assets(
            repository, archive_root, lock_path, direct, version="99.01.01"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            {path.name: archive_sha256(path) for path in direct.iterdir()},
            {path.name: archive_sha256(path) for path in output.iterdir()},
        )

    def write_original_build_receipt(self, core, lock_path, lock, changes=None):
        build_path = core.parent / "original-build.json"
        source_set = core / "adb-helper-source-set-receipt.json"
        receipt = {
            "schema_version": 1, "receipt_kind": "binary-build",
            "lock_sha256": archive_sha256(lock_path),
            "source_set_receipt_sha256": archive_sha256(source_set),
            "release_assets": load_build_module().package_support_receipt_assets(lock, core),
            "qualification": {"fixture": "not-native-qualified"},
        }
        receipt.update(changes or {})
        build_path.write_text(json.dumps(receipt), encoding="utf-8")
        return build_path, archive_sha256(source_set), archive_sha256(build_path)

    def overlay_pin_arguments(self, core, lock_path, lock):
        build, source_pin, build_pin = self.write_original_build_receipt(core, lock_path, lock)
        return ["--build-receipt", str(build), "--source-set-sha256", source_pin,
                "--build-receipt-sha256", build_pin]

    def make_overlay_fixture(self, build_changes=None, *, name=None):
        root = self.root / name if name else self.root
        root.mkdir(exist_ok=True)
        repository, archives, lock_path, lock = self.make_legal_fixture(root)
        core = root / "immutable-core"
        result = subprocess.run(
            [sys.executable, str(LEGAL_SCRIPT), "--core-only", "--lock", str(lock_path),
             "--archive-root", str(archives), "--repository-root", str(repository),
             "--output", str(core)],
            capture_output=True, text=True, check=False, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        build_path, source_pin, build_pin = self.write_original_build_receipt(
            core, lock_path, lock, build_changes)
        args = argparse.Namespace(
            lock=lock_path, archive_root=None, repository_root=None,
            core_only=False, overlay_only=True, core_root=core,
            build_receipt=build_path, source_set_sha256=source_pin,
            build_receipt_sha256=build_pin,
            output=root / "assembly", package_support_output=root / "support",
            version="99.01.01", base_url="https://github.com/ZEACENT/klogg",
        )
        return args, lock

    def materialize_fixture_overlay(self, args, module=None):
        module = module or load_legal_module()
        # Supply the parsed caller inputs directly to isolate materialization
        # from parser plumbing in mutation and failure-injection tests.
        with mock.patch.object(sys, "path", [str(ROOT / "scripts"), *sys.path]), \
                mock.patch.object(module.argparse.ArgumentParser, "parse_args", return_value=args):
            return module.main()

    def test_overlay_requires_external_pins_before_creating_output(self):
        args, _ = self.make_overlay_fixture()
        result = subprocess.run(
            [sys.executable, str(LEGAL_SCRIPT), "--overlay-only", "--lock", str(args.lock),
             "--core-root", str(args.core_root), "--version", args.version,
             "--base-url", args.base_url, "--output", str(args.output)],
            capture_output=True, text=True, check=False, timeout=10,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("external", result.stderr)
        self.assertFalse(args.output.exists())

    def test_overlay_rejects_wrong_external_pins_before_any_output(self):
        args, _ = self.make_overlay_fixture()
        for field in ("source_set_sha256", "build_receipt_sha256"):
            with self.subTest(pin=field):
                invalid = argparse.Namespace(**{**vars(args), field: "a" * 64,
                                               "output": self.root / f"assembly-{field}",
                                               "package_support_output": self.root / f"support-{field}"})
                with self.assertRaisesRegex(RuntimeError, "external.*digest"):
                    self.materialize_fixture_overlay(invalid)
                self.assertFalse(invalid.output.exists())
                self.assertFalse(invalid.package_support_output.exists())

    def test_overlay_rejects_coherent_support_and_source_set_rewrite(self):
        args, _ = self.make_overlay_fixture()
        asset = args.core_root / "adb-helper-licenses.tar.gz"
        asset.write_bytes(b"coherently forged legal bytes\n")
        asset.with_name(asset.name + ".sha256").write_text(
            f"{archive_sha256(asset)}  {asset.name}\n", encoding="utf-8")
        source_set_path = args.core_root / "adb-helper-source-set-receipt.json"
        source_set = json.loads(source_set_path.read_text())
        next(item for item in source_set["package_support_assets"]
             if item["kind"] == "licenses")["sha256"] = archive_sha256(asset)
        source_set_path.write_text(json.dumps(source_set), encoding="utf-8")
        load_legal_module().write_hash(source_set_path)
        with self.assertRaisesRegex(RuntimeError, "external.*digest"):
            self.materialize_fixture_overlay(args)
        self.assertFalse(args.output.exists())

    def test_overlay_uses_original_build_source_set_binding(self):
        args, _ = self.make_overlay_fixture({"source_set_receipt_sha256": "a" * 64})
        with self.assertRaisesRegex(RuntimeError, "binary-build receipt source-set"):
            self.materialize_fixture_overlay(args)
        self.assertFalse(args.output.exists())

    def test_overlay_rejects_support_overlap_before_destructive_projection(self):
        args, _ = self.make_overlay_fixture()
        before = {p.name: p.read_bytes() for p in args.core_root.iterdir()}
        args.package_support_output = args.core_root
        with self.assertRaisesRegex(RuntimeError, "separate|disjoint"):
            self.materialize_fixture_overlay(args)
        self.assertEqual(before, {p.name: p.read_bytes() for p in args.core_root.iterdir()})
        self.assertFalse(args.output.exists())

    def test_overlay_detects_core_mutation_after_copy_and_cleans_owned_outputs(self):
        args, _ = self.make_overlay_fixture()
        module = load_legal_module()
        real_copy = module.copy_checked_asset
        changed = args.core_root / "adb-helper-licenses.tar.gz"
        original_build = args.build_receipt.read_bytes()

        def mutate_after_copy(source, destination, expected, created, **kwargs):
            real_copy(source, destination, expected, created, **kwargs)
            if source == changed:
                changed.write_bytes(b"mutated after its checked copy\n")

        with mock.patch.object(module, "copy_checked_asset", side_effect=mutate_after_copy):
            with self.assertRaisesRegex(RuntimeError, "sha256"):
                self.materialize_fixture_overlay(args, module)
        self.assertEqual(changed.read_bytes(), b"mutated after its checked copy\n")
        self.assertEqual(args.build_receipt.read_bytes(), original_build)
        self.assertFalse(args.output.exists())
        self.assertFalse(args.package_support_output.exists())

    def test_overlay_cleanup_preserves_unowned_files_in_empty_caller_output(self):
        args, _ = self.make_overlay_fixture()
        args.output.mkdir()
        stranger = args.output / "peer-owned.txt"
        before = {path.name: path.read_bytes() for path in args.core_root.iterdir()}
        module = load_legal_module()
        real_copy = module.copy_checked_asset

        def fail_after_copy(source, destination, expected, created, **kwargs):
            real_copy(source, destination, expected, created, **kwargs)
            if destination.parent == args.output:
                stranger.write_bytes(b"peer data\n")
                raise OSError("injected copy failure")

        with mock.patch.object(module, "copy_checked_asset", side_effect=fail_after_copy):
            with self.assertRaisesRegex(OSError, "injected copy failure"):
                self.materialize_fixture_overlay(args, module)
        self.assertTrue(stranger.is_file(), "cleanup must remove only owned files")
        self.assertEqual(stranger.read_bytes(), b"peer data\n")
        self.assertEqual(list(args.output.iterdir()), [stranger])
        self.assertFalse(args.package_support_output.exists())
        self.assertEqual(before, {path.name: path.read_bytes() for path in args.core_root.iterdir()})

    def test_overlay_revalidates_generated_overlay_sidecars(self):
        args, _ = self.make_overlay_fixture()
        module = load_legal_module()
        real_hashes = module.write_release_hashes

        def corrupt_overlay_sidecar(output, assets, **kwargs):
            real_hashes(output, assets, **kwargs)
            (output / "ADB-HELPER-SOURCE-OFFER.txt.sha256").write_text("forged sidecar\n")

        with mock.patch.object(module, "write_release_hashes", side_effect=corrupt_overlay_sidecar):
            with self.assertRaisesRegex(RuntimeError, "sidecar"):
                self.materialize_fixture_overlay(args, module)
        self.assertFalse(args.output.exists())
        self.assertFalse(args.package_support_output.exists())

    def test_overlay_rejects_unplanned_files_and_forged_release_index(self):
        args, _ = self.make_overlay_fixture()
        module = load_legal_module()
        real_hashes = module.write_release_hashes
        for scenario in ("extra-file", "release-index"):
            with self.subTest(scenario=scenario):
                args.output = self.root / f"assembly-{scenario}"
                args.package_support_output = None

                def corrupt_release(output, assets, **kwargs):
                    real_hashes(output, assets, **kwargs)
                    if scenario == "extra-file":
                        (output / "unplanned.txt").write_bytes(b"not in the locked closure\n")
                    else:
                        (output / "adb-helper-release-assets.json").write_text("[]\n")

                with mock.patch.object(module, "write_release_hashes", side_effect=corrupt_release):
                    with self.assertRaisesRegex(RuntimeError, "coverage|release.*index"):
                        self.materialize_fixture_overlay(args, module)

    def test_overlay_preserves_verified_core_sidecar_bytes(self):
        args, _ = self.make_overlay_fixture()
        # The existing verifier accepts whitespace-separated checksum fields.
        # A checked projection must retain those verified bytes, not normalize
        # a core asset that was never owned by the new overlay.
        sidecar = args.core_root / "adb-helper-licenses.tar.gz.sha256"
        fields = sidecar.read_text().split()
        sidecar.write_text(f"{fields[0]}    {fields[1]}\n")
        before = {path.name: path.read_bytes() for path in args.core_root.iterdir()}
        self.materialize_fixture_overlay(args)
        self.assertEqual(before, {path.name: path.read_bytes() for path in args.core_root.iterdir()})
        for name, content in before.items():
            if name != "adb-helper-release-assets.json":
                self.assertEqual((args.output / name).read_bytes(), content, name)

    def test_overlay_rejects_pinned_non_build_evidence(self):
        for index, changes in enumerate(({"schema_version": True}, {"receipt_kind": "package-verification"})):
            with self.subTest(changes=changes):
                args, _ = self.make_overlay_fixture(changes, name=f"invalid-receipt-{index}")
                with self.assertRaisesRegex(RuntimeError, "binary-build.*schema|not binary-build"):
                    self.materialize_fixture_overlay(args)
                self.assertFalse(args.output.exists())

    def test_overlay_preserves_original_receipt_support_and_external_binary_without_acquisition(self):
        args, _ = self.make_overlay_fixture()
        external_binary = self.root / "external-helper"
        external_binary.write_bytes(b"synthetic external binary fixture; never executed\n")
        frozen_binary = external_binary.read_bytes()
        frozen_build = args.build_receipt.read_bytes()
        frozen_core = {path.name: path.read_bytes() for path in args.core_root.iterdir()}
        current_lock = self.root / "available-lock.json"
        args.lock.rename(current_lock)
        args.lock = current_lock
        shutil.rmtree(self.root / "repository")
        shutil.rmtree(self.root / "archives")
        offers = []
        for version, url in (("99.01.01", "https://github.com/ZEACENT/klogg/"),
                             ("99.01.02", "https://example.invalid/project")):
            args.version, args.base_url = version, url
            args.output = self.root / f"assembly-{version}"
            args.package_support_output = self.root / f"support-{version}"
            self.materialize_fixture_overlay(args)
            self.assertEqual(args.build_receipt.read_bytes(), frozen_build)
            self.assertEqual(external_binary.read_bytes(), frozen_binary)
            self.assertEqual(frozen_core, {path.name: path.read_bytes() for path in args.core_root.iterdir()})
            for name, content in frozen_core.items():
                if name != "adb-helper-release-assets.json":
                    self.assertEqual((args.output / name).read_bytes(), content, name)
            offer = (args.output / "ADB-HELPER-SOURCE-OFFER.txt").read_text()
            self.assertIn(f"klogg-v{version}-adb-helper-source-", offer)
            self.assertIn(f"{url.rstrip('/')}/releases", offer)
            offers.append(offer)
        self.assertNotEqual(*offers)

    def test_overlay_rejects_all_output_input_overlaps_before_writing(self):
        args, _ = self.make_overlay_fixture()
        frozen = {path.relative_to(self.root).as_posix(): path.read_bytes()
                  for path in self.root.rglob("*") if path.is_file()}
        cases = (
            (args.core_root, self.root / "support"),
            (args.core_root / "child", self.root / "support"),
            (self.root, self.root / "support"),
            (self.root / "assembly", args.core_root / "child"),
            (self.root / "assembly", args.lock.parent),
            (self.root / "assembly", args.build_receipt),
            (self.root / "assembly", self.root / "assembly"),
            (self.root / "assembly", self.root / "assembly" / "child"),
            (self.root / "assembly" / "child", self.root / "assembly"),
        )
        for output, support in cases:
            with self.subTest(output=output, support=support):
                args.output, args.package_support_output = output, support
                with self.assertRaisesRegex(RuntimeError, "separate|disjoint|parent"):
                    self.materialize_fixture_overlay(args)
                self.assertEqual(frozen, {path.relative_to(self.root).as_posix(): path.read_bytes()
                                          for path in self.root.rglob("*") if path.is_file()})
        self.assertFalse((self.root / "assembly").exists())
        self.assertFalse((self.root / "support").exists())

    def test_overlay_refuses_nonempty_symlink_and_missing_parent_outputs(self):
        args, _ = self.make_overlay_fixture()
        sentinel_root = self.root / "occupied"
        sentinel_root.mkdir()
        sentinel = sentinel_root / "sentinel.txt"
        sentinel.write_bytes(b"preserve caller data\n")
        alias = self.root / "alias"
        alias.symlink_to(sentinel_root, target_is_directory=True)
        cases = ((sentinel_root, "empty"), (alias, "symlink"),
                 (alias / "child", "symlink"), (self.root / "missing" / "child", "parent"),
                 (sentinel, "empty"))
        for bad_path, message in cases:
            for field in ("output", "package_support_output"):
                with self.subTest(path=bad_path, field=field):
                    args.output = self.root / "assembly"
                    args.package_support_output = self.root / "support"
                    setattr(args, field, bad_path)
                    with self.assertRaisesRegex(RuntimeError, message):
                        self.materialize_fixture_overlay(args)
                    self.assertEqual(sentinel.read_bytes(), b"preserve caller data\n")
                    self.assertFalse((self.root / "assembly").exists())
                    self.assertFalse((self.root / "support").exists())

    def test_overlay_rejects_changed_build_bytes_and_missing_original_support_binding(self):
        args, _ = self.make_overlay_fixture()
        original = args.build_receipt.read_bytes()
        args.build_receipt.write_bytes(original + b" \n")
        with self.assertRaisesRegex(RuntimeError, "external.*digest"):
            self.materialize_fixture_overlay(args)
        args.build_receipt.write_bytes(original)
        self.assertFalse(args.output.exists())
        args, _ = self.make_overlay_fixture({"release_assets": []}, name="missing-build-support")
        with self.assertRaisesRegex(RuntimeError, "coverage"):
            self.materialize_fixture_overlay(args)
        self.assertFalse(args.output.exists())

    def test_overlay_rejects_symlinked_release_index_without_deleting_its_target(self):
        args, _ = self.make_overlay_fixture()
        module = load_legal_module()
        real_hashes = module.write_release_hashes
        target = self.root / "foreign-index.json"

        def replace_index(output, assets, **kwargs):
            real_hashes(output, assets, **kwargs)
            index = output / "adb-helper-release-assets.json"
            target.write_bytes(index.read_bytes())
            index.unlink()
            index.symlink_to(target)

        with mock.patch.object(module, "write_release_hashes", side_effect=replace_index):
            with self.assertRaisesRegex(RuntimeError, "symlink"):
                self.materialize_fixture_overlay(args, module)
        self.assertTrue(target.is_file())
        self.assertFalse(args.output.exists())
        self.assertFalse(args.package_support_output.exists())

    def test_checked_copy_streams_and_rejects_mid_read_mutation(self):
        module = load_legal_module()
        source = self.root / "large-synthetic-input"
        destination = self.root / "checked-copy"
        chunk_size = 1024 * 1024
        source.write_bytes(b"a" * (chunk_size + 128))
        expected = archive_sha256(source)
        real_open = pathlib.Path.open
        reads = []

        class MutatingReader:
            def __init__(self, stream):
                self.stream = stream

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.stream.close()

            def read(self, size):
                reads.append(size)
                chunk = self.stream.read(size)
                if len(reads) == 1:
                    with real_open(source, "r+b") as changed:
                        changed.seek(chunk_size)
                        changed.write(b"b" * 128)
                return chunk

        def open_stream(path, mode="r", *args, **kwargs):
            stream = real_open(path, mode, *args, **kwargs)
            return MutatingReader(stream) if path == source and mode == "rb" else stream

        with mock.patch.object(sys, "path", [str(ROOT / "scripts"), *sys.path]), \
                mock.patch.object(pathlib.Path, "open", new=open_stream):
            with self.assertRaisesRegex(RuntimeError, "changed while copying"):
                module.copy_checked_asset(source, destination, expected, {})
        self.assertTrue(reads)
        self.assertTrue(all(size == chunk_size for size in reads))
        self.assertEqual(source.read_bytes()[-128:], b"b" * 128)

    def test_overlay_rejects_late_input_alias_and_late_output_index_symlink(self):
        for scenario in ("input-alias", "output-index"):
            with self.subTest(scenario=scenario):
                args, _ = self.make_overlay_fixture(name=scenario)
                module = load_legal_module()
                real_copy = module.copy_checked_asset
                original_build = args.build_receipt.read_bytes()
                foreign_target = args.output.parent / "foreign-target"
                changed = False

                def replace_after_final_copy(source, destination, expected, created, **kwargs):
                    nonlocal changed
                    real_copy(source, destination, expected, created, **kwargs)
                    if destination.parent != args.output or changed:
                        return
                    if scenario == "input-alias":
                        args.core_root.rename(foreign_target)
                        args.core_root.symlink_to(foreign_target, target_is_directory=True)
                        changed = True
                    elif destination.name == "adb-helper-release-assets.json":
                        foreign_target.write_bytes(destination.read_bytes())
                        destination.unlink()
                        destination.symlink_to(foreign_target)
                        changed = True

                with mock.patch.object(module, "copy_checked_asset", side_effect=replace_after_final_copy):
                    with self.assertRaisesRegex(RuntimeError, "symlink"):
                        self.materialize_fixture_overlay(args, module)
                self.assertTrue(changed)
                self.assertTrue(foreign_target.exists())
                self.assertEqual(args.build_receipt.read_bytes(), original_build)

    def test_overlay_cli_rejects_missing_and_malformed_external_inputs(self):
        args, _ = self.make_overlay_fixture()
        base = [sys.executable, str(LEGAL_SCRIPT), "--overlay-only", "--lock", str(args.lock),
                "--core-root", str(args.core_root), "--version", args.version,
                "--base-url", args.base_url, "--output", str(args.output)]
        original = {"--build-receipt": str(args.build_receipt),
                    "--source-set-sha256": args.source_set_sha256,
                    "--build-receipt-sha256": args.build_receipt_sha256}
        cases = [{key: value for key, value in original.items() if key != omitted}
                 for omitted in original]
        cases.extend({**original, flag: malformed}
                     for flag in ("--source-set-sha256", "--build-receipt-sha256")
                     for malformed in ("a" * 63, "A" * 64, "0" * 64))
        for index, inputs in enumerate(cases):
            with self.subTest(case=index):
                result = subprocess.run(
                    base + [item for pair in inputs.items() for item in pair],
                    capture_output=True, text=True, check=False, timeout=10,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("external", result.stderr)
                self.assertFalse(args.output.exists())
                self.assertFalse(args.package_support_output.exists())

    def test_overlay_partial_projection_failure_cleans_only_owned_output(self):
        args, _ = self.make_overlay_fixture()
        args.output.mkdir()
        args.package_support_output.mkdir()
        before = {path.relative_to(self.root).as_posix(): path.read_bytes()
                  for path in self.root.rglob("*") if path.is_file()}
        module = load_legal_module()
        real_copy = module.copy_checked_asset
        real_open = pathlib.Path.open

        class FailingWriter:
            def __init__(self, stream):
                self.stream = stream

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.stream.close()

            def fileno(self):
                return self.stream.fileno()

            def write(self, chunk):
                self.stream.write(chunk[:8])
                raise OSError("injected partial support write")

        def fail_support_write(source, destination, expected, created, **kwargs):
            def open_stream(path, mode="r", *arguments, **kwargs):
                stream = real_open(path, mode, *arguments, **kwargs)
                return FailingWriter(stream) if path == destination and mode == "xb" else stream

            if destination.parent == args.package_support_output:
                with mock.patch.object(pathlib.Path, "open", new=open_stream):
                    return real_copy(source, destination, expected, created, **kwargs)
            return real_copy(source, destination, expected, created, **kwargs)

        with mock.patch.object(module, "copy_checked_asset", side_effect=fail_support_write):
            with self.assertRaisesRegex(OSError, "partial support write"):
                self.materialize_fixture_overlay(args, module)
        self.assertTrue(args.output.is_dir())
        self.assertTrue(args.package_support_output.is_dir())
        self.assertEqual(list(args.output.iterdir()), [])
        self.assertEqual(list(args.package_support_output.iterdir()), [])
        self.assertEqual(before, {path.relative_to(self.root).as_posix(): path.read_bytes()
                                 for path in self.root.rglob("*") if path.is_file()})
        self.assertEqual(list(args.output.parent.glob(".adb-overlay-*")), [])

    def test_overlay_refuses_support_alias_inserted_during_release_copy_without_foreign_writes(self):
        for initially_empty in (False, True):
            with self.subTest(initially_empty=initially_empty):
                args, _ = self.make_overlay_fixture(name=f"support-alias-{initially_empty}")
                if initially_empty:
                    args.package_support_output.mkdir()
                foreign = args.output.parent / "foreign-support"
                foreign.mkdir()
                (foreign / "unrelated.txt").write_bytes(b"preserve unrelated caller material\n")
                frozen_foreign = {path.name: path.read_bytes() for path in foreign.iterdir()}
                frozen_core = {path.name: path.read_bytes() for path in args.core_root.iterdir()}
                frozen_build = args.build_receipt.read_bytes()
                module = load_legal_module()
                real_copy = module.copy_checked_asset
                replaced = False

                def replace_support_during_release_copy(source, destination, expected, created, **kwargs):
                    nonlocal replaced
                    real_copy(source, destination, expected, created, **kwargs)
                    if destination.parent == args.output and not replaced:
                        if initially_empty:
                            args.package_support_output.rmdir()
                        args.package_support_output.symlink_to(foreign, target_is_directory=True)
                        replaced = True

                with mock.patch.object(module, "copy_checked_asset", side_effect=replace_support_during_release_copy):
                    with self.assertRaisesRegex(RuntimeError, "symlink"):
                        self.materialize_fixture_overlay(args, module)
                self.assertTrue(replaced, "the alias must be inserted after preflight and during release copy")
                self.assertEqual(
                    frozen_foreign, {path.name: path.read_bytes() for path in foreign.iterdir()},
                    "rejecting a late support alias must not leave projection files in its foreign target",
                )
                self.assertEqual(frozen_core, {path.name: path.read_bytes() for path in args.core_root.iterdir()})
                self.assertEqual(args.build_receipt.read_bytes(), frozen_build)
                self.assertTrue(args.package_support_output.is_symlink())
                self.assertFalse(args.output.exists())

    def test_overlay_binds_later_copies_to_the_acquired_output_directory(self):
        args, _ = self.make_overlay_fixture()
        foreign = args.output.parent / "foreign-release"
        foreign.mkdir()
        (foreign / "unrelated.txt").write_bytes(b"foreign release material\n")
        frozen_foreign = {path.name: path.read_bytes() for path in foreign.iterdir()}
        moved_owned = args.output.parent / "moved-owned-release"
        module = load_legal_module()
        real_copy = module.copy_checked_asset
        replaced = False

        def replace_acquired_release(source, destination, expected, created, **kwargs):
            nonlocal replaced
            real_copy(source, destination, expected, created, **kwargs)
            if destination.parent == args.output and not replaced:
                args.output.rename(moved_owned)
                foreign.rename(args.output)
                replaced = True

        with mock.patch.object(module, "copy_checked_asset", side_effect=replace_acquired_release):
            with self.assertRaises(RuntimeError):
                self.materialize_fixture_overlay(args, module)
        self.assertTrue(replaced)
        self.assertEqual(frozen_foreign, {path.name: path.read_bytes() for path in args.output.iterdir()})
        self.assertTrue(moved_owned.is_dir())
        self.assertFalse(args.package_support_output.exists())

    def test_core_only_refuses_stale_overlay_in_its_output(self):
        repository, archive_root, lock_path, _ = self.make_legal_fixture()
        output = self.root / "stale-core"
        output.mkdir()
        (output / "ADB-HELPER-SOURCE-OFFER.txt").write_text("old release offer\n")
        result = subprocess.run(
            [sys.executable, str(LEGAL_SCRIPT), "--core-only", "--lock", str(lock_path),
             "--archive-root", str(archive_root), "--repository-root", str(repository),
             "--output", str(output)],
            capture_output=True, text=True, check=False, timeout=10,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("empty", (result.stdout + result.stderr).lower())

    def test_overlay_only_rejects_core_bytes_even_with_forged_sidecar(self):
        repository, archive_root, lock_path, lock = self.make_legal_fixture()
        core = self.root / "immutable-core"
        build = subprocess.run(
            [sys.executable, str(LEGAL_SCRIPT), "--core-only", "--lock", str(lock_path),
             "--archive-root", str(archive_root), "--repository-root", str(repository),
             "--output", str(core)],
            capture_output=True, text=True, check=False, timeout=10,
        )
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        pins = self.overlay_pin_arguments(core, lock_path, lock)
        asset = core / "adb-helper-licenses.tar.gz"
        asset.write_bytes(b"forged licenses\n")
        asset.with_name(asset.name + ".sha256").write_text(
            f"{archive_sha256(asset)}  {asset.name}\n", encoding="utf-8"
        )
        result = subprocess.run(
            [sys.executable, str(LEGAL_SCRIPT), "--overlay-only", "--lock", str(lock_path),
             "--core-root", str(core), "--version", "99.01.01",
             "--base-url", "https://github.com/ZEACENT/klogg",
             "--output", str(self.root / "staged"), *pins],
            capture_output=True, text=True, check=False, timeout=10,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sha256", (result.stdout + result.stderr).lower())

    def test_legal_assets_reject_core_overlay_ownership_swaps(self):
        repository, archive_root, lock_path, lock = self.make_legal_fixture()
        for kind in ("source-offer", "licenses"):
            with self.subTest(kind=kind):
                record = next(asset for asset in lock["release_assets"] if asset["kind"] == kind)
                original = record["ownership"]
                record["ownership"] = "core" if kind == "source-offer" else "overlay"
                lock_path.write_text(json.dumps(lock), encoding="utf-8")
                result = self.build_legal_assets(
                    repository, archive_root, lock_path, self.root / f"invalid-{kind}"
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("ownership", (result.stdout + result.stderr).lower())
                record["ownership"] = original

    def test_legal_assets_materialize_only_locked_package_support_projection(self):
        repository, archive_root, lock_path, lock = self.make_legal_fixture()
        release_output = self.root / "release"
        support_output = self.root / "package-support"

        result = self.build_legal_assets(
            repository,
            archive_root,
            lock_path,
            release_output,
            support_output,
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        expected_support = {
            name
            for asset in lock["release_assets"]
            if asset["distribution"]["package_required"] is True
            for name in (asset["file_name"], asset["sha256_file"])
        }
        self.assertEqual(
            {path.name for path in support_output.iterdir()}, expected_support
        )
        self.assertEqual(
            {path.name for path in release_output.iterdir()},
            {
                name
                for asset in lock["release_assets"]
                for name in (asset["file_name"], asset["sha256_file"])
            }
            | {"adb-helper-release-assets.json"},
        )
        source_archive = next(
            asset
            for asset in lock["release_assets"]
            if asset["kind"] == "source-archive"
        )
        self.assertFalse((support_output / source_archive["file_name"]).exists())
        self.assertFalse((support_output / source_archive["sha256_file"]).exists())
        for name in expected_support:
            self.assertEqual(
                (support_output / name).read_bytes(),
                (release_output / name).read_bytes(),
            )

    def test_legal_assets_reject_nested_release_asset_paths_before_receipt_generation(self):
        repository, archive_root, lock_path, lock = self.make_legal_fixture()
        lock["release_assets"][1]["file_name"] = "legal/adb-helper-licenses.tar.gz"
        lock["release_assets"][1]["sha256_file"] = (
            "legal/adb-helper-licenses.tar.gz.sha256"
        )
        lock_path.write_text(json.dumps(lock), encoding="utf-8")

        result = self.build_legal_assets(
            repository,
            archive_root,
            lock_path,
            self.root / "release-nested-path",
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("flat file name", (result.stdout + result.stderr).lower())

    def test_legal_assets_reject_non_boolean_distribution_metadata(self):
        repository, archive_root, lock_path, lock = self.make_legal_fixture()
        lock["release_assets"][1]["distribution"]["package_required"] = "true"
        lock_path.write_text(json.dumps(lock), encoding="utf-8")

        result = self.build_legal_assets(
            repository,
            archive_root,
            lock_path,
            self.root / "release-invalid-distribution",
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("distribution", (result.stdout + result.stderr).lower())

    def test_legal_assets_do_not_claim_canonical_archives_are_raw_upstream_downloads(self):
        repository, archive_root, lock_path, lock = self.make_legal_fixture()
        record = lock["sources"][0]
        record["archive_identity"] = CANONICAL_TAR_GZ_IDENTITY
        lock_path.write_text(json.dumps(lock), encoding="utf-8")
        output = self.root / "release-canonical"

        result = self.build_legal_assets(repository, archive_root, lock_path, output)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        package = json.loads(
            (output / "adb-helper-sbom.spdx.json").read_text(encoding="utf-8")
        )["packages"][0]
        self.assertEqual(package["downloadLocation"], "NOASSERTION")
        for marker in (
            CANONICAL_TAR_GZ_IDENTITY,
            record["archive_url"],
            record["archive_file"],
        ):
            self.assertIn(marker, package.get("sourceInfo", ""))
        closure_identity = [
            {
                "id": record["id"],
                "archive_file": record["archive_file"],
                "archive_sha256": record["archive_sha256"],
                "archive_identity": CANONICAL_TAR_GZ_IDENTITY,
                "revision": record["commit"],
            }
        ]
        expected_tree_hash = hashlib.sha256(
            json.dumps(closure_identity, sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest()
        receipt = json.loads(
            (output / "adb-helper-source-set-receipt.json").read_text(encoding="utf-8")
        )
        self.assertEqual(
            receipt["source_identity"]["final_tree_sha256"], expected_tree_hash
        )

    def test_legal_assets_include_license_texts_valid_spdx_and_lgpl_replacement_terms(self):
        repository, archive_root, lock_path, _ = self.make_legal_fixture()
        output = self.root / "release"
        result = self.build_legal_assets(repository, archive_root, lock_path, output)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        with tarfile.open(output / "adb-helper-licenses.tar.gz", "r:gz") as tar:
            members = set(tar.getnames())
            license_text = tar.extractfile("upstream/aosp-libusb/libusb-source/COPYING").read().decode("utf-8")
        self.assertIn("upstream/aosp-libusb/libusb-source/COPYING", members)
        self.assertIn("LESSER GENERAL PUBLIC LICENSE", license_text)

        sbom = json.loads((output / "adb-helper-sbom.spdx.json").read_text(encoding="utf-8"))
        self.assertRegex(sbom["creationInfo"]["created"], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
        self.assertEqual(sbom["documentDescribes"], ["SPDXRef-Package-aosp-libusb"])
        self.assertEqual(sbom["packages"][0]["licenseDeclared"], "LGPL-2.1-or-later")
        self.assertTrue(sbom["relationships"])

        offer = (output / "ADB-HELPER-SOURCE-OFFER.txt").read_text(encoding="utf-8").lower()
        source_archive = output / "adb-helper-source-archive.tar.gz"
        archive_hash = archive_sha256(source_archive)
        published_name = f"klogg-v26.08.27-adb-helper-source-{archive_hash[:12]}.tar.gz"
        for term in (
            "libusb",
            "replace",
            "relink",
            "not included in the installer",
            published_name,
            archive_hash,
            "/releases",
            "shasum -a 256",
        ):
            self.assertIn(term, offer)
        self.assertNotIn(f"/releases/download/v26.08.27/{published_name}", offer)
        self.assertIn("/releases/tag/continuous", offer)
        self.assertNotIn(f"/releases/download/continuous/{published_name}", offer)
        self.assertIn("rolling and mutable", offer)


class AdbHelperBinaryInspectionContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_build_module()
        cls.smoke_module = load_smoke_module()

    def test_locked_patch_application_requires_explicit_gnu_patch_dispatch(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = pathlib.Path(tempdir)
            source = root / "source"
            source.mkdir()
            patch = root / "change.patch"
            patch.write_text("fixture\n", encoding="utf-8")
            record = {"path": "change.patch", "apply": True}

            with self.assertRaisesRegex(RuntimeError, "apply_tool"):
                self.module.apply_locked_patch(source, patch, record)

            record["apply_tool"] = "gnu-patch"
            with mock.patch.dict(
                self.module.os.environ,
                {"KLOGG_ADB_PATCH_EXECUTABLE": "/locked/usr/bin/patch"},
            ), mock.patch.object(self.module, "run") as run:
                self.module.apply_locked_patch(source, patch, record)
            run.assert_called_once_with(
                [
                    "/locked/usr/bin/patch",
                    "--directory",
                    str(source),
                    "--strip",
                    "1",
                    "--batch",
                    "--forward",
                    "--input",
                    str(patch),
                ]
            )

    def test_smoke_cleanup_uses_the_owned_child_status_instead_of_windows_pid_probing(self):
        process = mock.Mock()
        process.poll.return_value = 0
        with mock.patch.object(self.smoke_module.os, "kill") as kill:
            self.assertTrue(self.smoke_module.process_terminated(process))
            process.poll.return_value = None
            self.assertFalse(self.smoke_module.process_terminated(process))
        kill.assert_not_called()

    def test_smoke_failure_surfaces_the_structured_report_error(self):
        with tempfile.TemporaryDirectory() as tempdir:
            report = pathlib.Path(tempdir) / "smoke.json"
            report.write_text(
                json.dumps({"error": "libwinpthread-1.dll is missing"}),
                encoding="utf-8",
            )
            with mock.patch.object(
                self.module,
                "run",
                side_effect=RuntimeError("smoke command failed"),
            ), self.assertRaisesRegex(
                RuntimeError,
                "ADB helper smoke failed: libwinpthread-1.dll is missing",
            ):
                self.module.run_smoke(["adb-smoke"], report)

    def test_delayed_runtime_load_contract_binds_sidecar_to_pinned_source(self):
        with tempfile.TemporaryDirectory() as tempdir:
            source_root = pathlib.Path(tempdir)
            source = source_root / "windows-platform-development/AdbWinApi.cpp"
            source.parent.mkdir(parents=True)
            source.write_text(
                'handle = LoadLibrary(L"AdbWinUsbApi.dll");\n', encoding="utf-8"
            )
            record = {
                "runtime_file": "AdbWinUsbApi.dll",
                "loaded_by": "AdbWinApi.dll",
                "source": "windows-platform-development/AdbWinApi.cpp",
                "expression": 'LoadLibrary(L"AdbWinUsbApi.dll")',
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "loader_symbol": "LoadLibraryW",
            }

            evidence = self.module.verify_required_runtime_loads(
                source_root,
                {
                    "usb": {
                        "runtime_files": ["AdbWinApi.dll", "AdbWinUsbApi.dll"],
                        "required_imports": ["AdbWinApi.dll"],
                        "required_delayed_runtime_loads": [record],
                    }
                },
            )

            self.assertEqual(len(evidence), 1)
            self.assertEqual(
                {key: evidence[0][key] for key in record},
                record,
            )
            self.assertRegex(evidence[0]["source_sha256"], r"^[0-9a-f]{64}$")

    def test_delayed_runtime_load_contract_fails_when_pinned_source_stops_loading_sidecar(self):
        with tempfile.TemporaryDirectory() as tempdir:
            source_root = pathlib.Path(tempdir)
            source = source_root / "windows-platform-development/AdbWinApi.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("return false;\n", encoding="utf-8")

            with self.assertRaisesRegex(RuntimeError, "no longer loads required sidecar"):
                self.module.verify_required_runtime_loads(
                    source_root,
                    {
                        "usb": {
                            "runtime_files": ["AdbWinApi.dll", "AdbWinUsbApi.dll"],
                            "required_imports": ["AdbWinApi.dll"],
                            "required_delayed_runtime_loads": [
                                {
                                    "runtime_file": "AdbWinUsbApi.dll",
                                    "loaded_by": "AdbWinApi.dll",
                                    "source": "windows-platform-development/AdbWinApi.cpp",
                                    "expression": 'LoadLibrary(L"AdbWinUsbApi.dll")',
                                    "source_sha256": hashlib.sha256(
                                        source.read_bytes()
                                    ).hexdigest(),
                                    "loader_symbol": "LoadLibraryW",
                                }
                            ],
                        }
                    },
                )

    def test_dumpbin_dependency_parser_rejects_unrepresentable_import_names(self):
        valid = """
Dump of file adb.exe

  Image has the following dependencies:

    KERNEL32.dll
    AdbWinApi.dll

  Summary
"""
        self.assertEqual(
            self.module.parse_dumpbin_dependencies(valid, "adb.exe"),
            ["KERNEL32.dll", "AdbWinApi.dll"],
        )

        invalid = valid.replace("AdbWinApi.dll", "vendor\\host-only.dll")
        with self.assertRaisesRegex(RuntimeError, "unsupported PE dependency name"):
            self.module.parse_dumpbin_dependencies(invalid, "adb.exe")

    def test_windows_import_partition_rejects_unpackaged_host_dlls(self):
        with tempfile.TemporaryDirectory() as tempdir:
            system_directory = pathlib.Path(tempdir) / "System32"
            system_directory.mkdir()
            (system_directory / "KERNEL32.dll").write_bytes(b"system")
            target_plan = {
                "usb": {
                    "runtime_files": ["AdbWinApi.dll"],
                    "allowed_system_imports_by_binary": {
                        "adb.exe": ["KERNEL32.dll"],
                    },
                }
            }
            imports = {
                "adb.exe": ["AdbWinApi.dll", "KERNEL32.dll"],
            }

            self.assertEqual(
                self.module.classify_windows_system_imports(
                    imports, target_plan, system_directory
                ),
                {"adb.exe": ["KERNEL32.dll"]},
            )

            imports["adb.exe"].append("host-only-vendor.dll")
            with self.assertRaisesRegex(RuntimeError, "unlocked system import"):
                self.module.classify_windows_system_imports(
                    imports, target_plan, system_directory
                )

    def test_delayed_runtime_binary_evidence_binds_loader_symbol_and_sidecar_name(self):
        with tempfile.TemporaryDirectory() as tempdir:
            helper_dir = pathlib.Path(tempdir)
            loader = helper_dir / "AdbWinApi.dll"
            loader.write_bytes(b"prefix" + "AdbWinUsbApi.dll".encode("utf-16le") + b"suffix")
            record = {
                "runtime_file": "AdbWinUsbApi.dll",
                "loaded_by": "AdbWinApi.dll",
                "loader_symbol": "LoadLibraryW",
            }
            with mock.patch.object(self.module, "run", return_value="  LoadLibraryW\n"):
                evidence = self.module.verify_runtime_load_binaries(
                    helper_dir, [record]
                )
            self.assertEqual(
                evidence,
                [{**record, "runtime_name_encoding": "utf-16le"}],
            )

    def test_delayed_runtime_binary_evidence_rejects_missing_sidecar_name(self):
        with tempfile.TemporaryDirectory() as tempdir:
            helper_dir = pathlib.Path(tempdir)
            (helper_dir / "AdbWinApi.dll").write_bytes(b"LoadLibraryW only")
            with mock.patch.object(self.module, "run", return_value="  LoadLibraryW\n"):
                with self.assertRaisesRegex(RuntimeError, "does not embed delayed sidecar"):
                    self.module.verify_runtime_load_binaries(
                        helper_dir,
                        [
                            {
                                "runtime_file": "AdbWinUsbApi.dll",
                                "loaded_by": "AdbWinApi.dll",
                                "loader_symbol": "LoadLibraryW",
                            }
                        ],
                    )

    def test_dependency_resolution_rejects_dynamic_pinned_build_dependencies(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = pathlib.Path(tempdir)
            prefix = root / "install"
            library = prefix / "lib/libz.dylib"
            library.parent.mkdir(parents=True)
            library.write_bytes(b"fixture")
            cache = (
                root
                / "superbuild/adb_android_tools-prefix/src/adb_android_tools-build/CMakeCache.txt"
            )
            cache.parent.mkdir(parents=True)
            cache.write_text(
                f"ZLIB_LIBRARY_RELEASE:FILEPATH={library}\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(RuntimeError, "static"):
                self.module.verify_dependency_resolution(root / "superbuild", prefix)

    def test_dependency_audit_rejects_uncontrolled_usr_local_paths(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = pathlib.Path(tempdir)
            prefix = root / "install"
            library = prefix / "lib/libz.a"
            library.parent.mkdir(parents=True)
            library.write_bytes(b"fixture")
            build = root / "superbuild/adb_android_tools-prefix/src/adb_android_tools-build"
            build.mkdir(parents=True)
            (build / "CMakeCache.txt").write_text(
                f"ZLIB_LIBRARY_RELEASE:FILEPATH={library}\n", encoding="utf-8"
            )
            (build / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build),
                            "command": f"c++ -I{prefix}/include -I/usr/local/include -c source.cpp",
                            "file": "source.cpp",
                        }
                    ]
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "/usr/local"):
                self.module.verify_dependency_resolution(root / "superbuild", prefix)

    def test_linux_runpath_must_be_exactly_origin_only(self):
        dynamic = """
 0x0000000000000001 (NEEDED)             Shared library: [libusb-1.0.so.0]
 0x000000000000001d (RUNPATH)            Library runpath: [$ORIGIN:/tmp/uncontrolled]
"""
        resolved = "libusb-1.0.so.0 => /artifact/helpers/libusb-1.0.so.0 (0x1)\n"
        with mock.patch.object(self.module, "run", side_effect=[dynamic, resolved]):
            with self.assertRaisesRegex(RuntimeError, "RUNPATH"):
                self.module.inspect_binary(
                    "linux-x86_64", pathlib.Path("/artifact/helpers/adb"), pathlib.Path("/artifact/helpers")
                )

    def test_linux_inspector_rejects_unlocked_dynamic_imports(self):
        dynamic = """
 0x0000000000000001 (NEEDED)             Shared library: [libusb-1.0.so.0]
 0x0000000000000001 (NEEDED)             Shared library: [libunexpected.so.1]
 0x000000000000001d (RUNPATH)            Library runpath: [$ORIGIN]
"""
        header = "  Machine:                           Advanced Micro Devices X86-64\n"
        resolved = "libusb-1.0.so.0 => /artifact/helpers/libusb-1.0.so.0 (0x1)\n"
        versions = "  0x0010:   Name: GLIBC_2.17  Flags: none  Version: 5\n"
        target_plan = {
            "glibc_baseline": "2.17",
            "allowed_dynamic_imports": ["libusb-1.0.so.0", "libc.so.6"],
            "usb": {"runtime_files": []},
        }
        with mock.patch.object(
            self.module, "run", side_effect=[dynamic, header, resolved, versions]
        ):
            with self.assertRaisesRegex(RuntimeError, "dynamic import"):
                self.module.inspect_binary(
                    "linux-x86_64",
                    pathlib.Path("/artifact/helpers/adb"),
                    pathlib.Path("/artifact/helpers"),
                    target_plan,
                )

    def test_linux_inspector_rejects_newer_glibcxx_symbol_contract(self):
        dynamic = """
 0x0000000000000001 (NEEDED)             Shared library: [libusb-1.0.so.0]
 0x000000000000001d (RUNPATH)            Library runpath: [$ORIGIN]
"""
        header = "  Machine:                           Advanced Micro Devices X86-64\n"
        resolved = "libusb-1.0.so.0 => /artifact/helpers/libusb-1.0.so.0 (0x1)\n"
        versions = """
  0x0010:   Name: GLIBC_2.17  Flags: none  Version: 5
  0x0020:   Name: GLIBCXX_3.4.30  Flags: none  Version: 6
"""
        target_plan = {
            "glibc_baseline": "2.17",
            "symbol_version_maximums": {"GLIBC": "2.17", "GLIBCXX": "3.4.29"},
            "usb": {"runtime_files": []},
        }
        with mock.patch.object(
            self.module, "run", side_effect=[dynamic, header, resolved, versions]
        ):
            with self.assertRaisesRegex(RuntimeError, "GLIBCXX"):
                self.module.inspect_binary(
                    "linux-x86_64",
                    pathlib.Path("/artifact/helpers/adb"),
                    pathlib.Path("/artifact/helpers"),
                    target_plan,
                )

    def test_linux_inspector_rejects_glibc_newer_than_locked_baseline(self):
        dynamic = """
 0x0000000000000001 (NEEDED)             Shared library: [libusb-1.0.so.0]
 0x000000000000001d (RUNPATH)            Library runpath: [$ORIGIN]
"""
        header = "  Machine:                           Advanced Micro Devices X86-64\n"
        resolved = "libusb-1.0.so.0 => /artifact/helpers/libusb-1.0.so.0 (0x1)\n"
        versions = "  0x0010:   Name: GLIBC_2.36  Flags: none  Version: 5\n"
        with mock.patch.object(
            self.module, "run", side_effect=[dynamic, header, resolved, versions]
        ):
            with self.assertRaisesRegex(RuntimeError, "GLIBC"):
                self.module.inspect_binary(
                    "linux-x86_64",
                    pathlib.Path("/artifact/helpers/adb"),
                    pathlib.Path("/artifact/helpers"),
                    {"glibc_baseline": "2.35"},
                )

    def test_macos_inspector_rejects_unbundled_non_system_dynamic_import(self):
        imports = """/artifact/helpers/adb:
/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation (compatibility version 1.0.0, current version 1.0.0)
/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit (compatibility version 1.0.0, current version 1.0.0)
/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1.0.0)
/usr/local/lib/libz.1.dylib (compatibility version 1.0.0, current version 1.3.1)
"""
        load_commands = """Load command 1
          cmd LC_BUILD_VERSION
      cmdsize 32
     platform 1
        minos 15.0
          sdk 15.0
"""
        with mock.patch.object(
            self.module, "run", side_effect=[imports, "x86_64\n", load_commands]
        ):
            with self.assertRaisesRegex(RuntimeError, "unbundled"):
                self.module.inspect_binary(
                    "macos-x86_64",
                    pathlib.Path("/artifact/helpers/adb"),
                    pathlib.Path("/artifact/helpers"),
                    {"deployment_target": "15.0"},
                )

    def test_macos_inspector_rejects_unlocked_system_framework_import(self):
        imports = """/artifact/helpers/adb:
/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation (compatibility version 1.0.0, current version 1.0.0)
/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit (compatibility version 1.0.0, current version 1.0.0)
/System/Library/Frameworks/Security.framework/Versions/A/Security (compatibility version 1.0.0, current version 1.0.0)
/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1.0.0)
"""
        load_commands = """Load command 1
          cmd LC_BUILD_VERSION
      cmdsize 32
     platform 1
        minos 14.0
          sdk 15.0
"""
        target_plan = {
            "deployment_target": "14.0",
            "allowed_dynamic_imports": [
                "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation",
                "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit",
                "/usr/lib/libSystem.B.dylib",
            ],
        }
        with mock.patch.object(
            self.module, "run", side_effect=[imports, "arm64\n", load_commands]
        ):
            with self.assertRaisesRegex(RuntimeError, "dynamic import"):
                self.module.inspect_binary(
                    "macos-arm64",
                    pathlib.Path("/artifact/helpers/adb"),
                    pathlib.Path("/artifact/helpers"),
                    target_plan,
                )

    def test_macos_inspector_rejects_actual_architecture_mismatch(self):
        imports = """/artifact/helpers/adb:
/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation (compatibility version 1.0.0, current version 1.0.0)
/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit (compatibility version 1.0.0, current version 1.0.0)
/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1.0.0)
"""
        with mock.patch.object(self.module, "run", side_effect=[imports, "arm64\n"]):
            with self.assertRaisesRegex(RuntimeError, "architecture"):
                self.module.inspect_binary(
                    "macos-x86_64", pathlib.Path("/artifact/helpers/adb"), pathlib.Path("/artifact/helpers")
                )


if __name__ == "__main__":
    unittest.main()
