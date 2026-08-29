import gzip
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
        self.root = pathlib.Path(self.tempdir.name)

    def tearDown(self):
        self.tempdir.cleanup()

    def write_lock(self, record: dict) -> pathlib.Path:
        path = self.root / "lock.json"
        path.write_text(json.dumps({"sources": [record], "dependencies": []}), encoding="utf-8")
        return path

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
            "../libcutils/include/cutils",
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
            "testdata/",
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

    def make_legal_fixture(self):
        repository = self.root / "repository"
        archive_root = self.root / "archives"
        archive_root.mkdir()
        for relative, content in {
            "COPYING": "Klogg license\n",
            "NOTICE": "Klogg notice\n",
            "packaging/adb/README.md": "Build instructions\n",
            "packaging/adb/superbuild/CMakeLists.txt": "project(adb)\n",
            "scripts/prefetch_adb_helper_sources.py": "# prefetch\n",
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

    def build_legal_assets(self, repository, archive_root, lock, output):
        return subprocess.run(
            [
                sys.executable,
                str(LEGAL_SCRIPT),
                "--lock",
                str(lock),
                "--archive-root",
                str(archive_root),
                "--repository-root",
                str(repository),
                "--version",
                "26.08.27",
                "--base-url",
                "https://github.com/ZEACENT/klogg",
                "--output",
                str(output),
            ],
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
            f"/releases/download/v26.08.27/{published_name}",
            f"/releases/download/continuous/{published_name}",
            "rolling",
            "shasum -a 256",
        ):
            self.assertIn(term, offer)


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
