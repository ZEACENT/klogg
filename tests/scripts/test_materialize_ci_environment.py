"""Producer acquisition contracts with fake network/APT and hostile tiny archives."""
from __future__ import annotations

import copy
import hashlib
import importlib
import io
import json
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_environment as ci
import ci_environment_inputs as apt
import build_ci_environment as build

FAMILY = "jammy-qt5"
BASE = "example.invalid/ubuntu@sha256:" + "a" * 64  # Synthetic test identity only.
PAYLOAD = b"verified upstream installer\n"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def archive_bytes(members):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w") as archive:
        for name, value in members:
            member = tarfile.TarInfo(name)
            if isinstance(value, tuple):
                member.type = tarfile.SYMTYPE
                member.linkname = value[0]
                archive.addfile(member)
            else:
                member.size = len(value)
                member.mode = 0o755 if name.endswith("/ninja") else 0o644
                archive.addfile(member, io.BytesIO(value))
    return output.getvalue()


class MaterializeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (ROOT / "scripts/materialize_ci_environment.py").is_file():
            raise AssertionError("producer materializer implementation is missing")
        cls.module = importlib.import_module("materialize_ci_environment")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="materializer-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.output = self.root / "output"
        (self.repo / "ci/environments").mkdir(parents=True)
        (self.repo / "docker").mkdir()
        (self.repo / "docker/Dockerfile").write_text(
            "ARG UBUNTU_IMAGE\nARG KLOGG_APT_STAGE=online\nFROM ${UBUNTU_IMAGE}\n"
            "ARG APT_BOOTSTRAP_LOCK_SHA256\nARG APT_RUNTIME_LOCK_SHA256\n"
            "ARG APT_BUILDER_LOCK_SHA256\nARG TOOLS_ARCHIVE_SHA256\n")
        self.catalog = {"schema_version": 1, "registry": ci.REGISTRY, "families": {FAMILY: {
            "platform": "linux/amd64", "dockerfile": "docker/Dockerfile", "context": "docker",
            "recipe_files": ["docker/Dockerfile"], "build_args": {"KLOGG_APT_STAGE": "locked"},
            "profiles": ["deb"]}}}
        self.definition = {"schema_version": 1, "assets": {"installer": {
            "url": "https://example.invalid/installer.sh", "sha256": sha(PAYLOAD),
            "checksum_source": "https://example.invalid/SHA256SUMS", "license": "BSD-3-Clause",
            "source_url": "https://example.invalid/source.tar.gz"}}, "families": {FAMILY: {
                "base_image": BASE, "build_args": {}, "apt_stages": [
                    self.stage("bootstrap", "inputs/bootstrap", "APT_BOOTSTRAP_LOCK_SHA256"),
                    self.stage("runtime", "inputs/apt", "APT_RUNTIME_LOCK_SHA256", ["bootstrap"]),
                    self.stage("qt-builder", "inputs/qt-builder", "APT_BUILDER_LOCK_SHA256", ["bootstrap"])],
                "downloads": [{"asset": "installer", "path": "installer.sh"}], "tools": []}}}
        self.downloads = []
        self.resolutions = []
        self.save()

    def stage(self, name, path, argument, prerequisites=()):
        return {"stage": name, "path": path, "build_arg": argument,
                "sources": ["deb http://archive.ubuntu.com/ubuntu jammy main"],
                "requested_packages": ["example"], "acquisition": {"mode": "apt-signed"},
                "prerequisites": list(prerequisites)}

    def save(self):
        for name, document in (("recipes", self.catalog), ("materials", self.definition)):
            (self.repo / "ci/environments" / (name + ".json")).write_bytes(apt.encoded(document))

    def download(self, url, destination, *, timeout):
        self.downloads.append(url)
        destination.write_bytes(PAYLOAD)

    def resolve(self, stage, destination, **kwargs):
        self.resolutions.append((copy.deepcopy(stage), kwargs))
        apt.validate_stage(stage)
        apt.validate_prerequisites(stage, kwargs["prerequisite_bundles"])
        destination.mkdir()
        (destination / "debs").mkdir()
        (destination / "lists").mkdir()
        (destination / "sources.list").write_text(apt.render_sources(stage))
        (destination / "base-packages.tsv").write_text("base-files\t1.0\tamd64\tinstalled\n")
        targets = []
        for index, _ in enumerate(stage["sources"]):
            prefix = "repo" + str(index) + "_dists_jammy"
            (destination / "lists" / (prefix + "_InRelease")).write_bytes(b"signed test release\n")
            target = prefix + "_main_binary-amd64_Packages"
            targets.append(target)
            (destination / "lists" / target).write_bytes(b"signed test index\n")
        (destination / "index-targets.txt").write_text("\n".join(targets) + "\n")
        rows = []
        for package in stage["requested_packages"]:
            name = package.split("=", 1)[0]
            filename = name + "_1.0_amd64.deb"
            (destination / "debs" / filename).write_bytes(b"authenticated test package")
            rows.append(name + "\t1.0\tamd64\t" + filename)
        (destination / "resolution.tsv").write_text("\n".join(rows) + "\n")
        if kwargs["keyring_files"]:
            (destination / "keys").mkdir()
            for name, source in kwargs["keyring_files"].items():
                shutil.copyfile(source, destination / "keys" / name)
        return apt.finalize(destination, stage)

    def materialize(self, **kwargs):
        return self.module.materialize(self.repo, FAMILY, self.output, timeout=37,
                                       downloader=self.download, resolver=self.resolve, **kwargs)

    def test_atomic_envelope_embeds_validated_manifests_and_preserves_branch_dependencies(self):
        inputs = self.materialize()
        self.assertEqual(set(inputs), {"schema_version", "family", "platform", "build_args", "files", "apt_bundles"})
        self.assertEqual(ci.load_json(self.output / "inputs.json"), inputs)
        self.assertEqual(inputs["build_args"]["UBUNTU_IMAGE"], BASE)
        self.assertEqual(inputs["build_args"]["KLOGG_APT_STAGE"], "locked")
        self.assertEqual(self.downloads, ["https://example.invalid/installer.sh"])
        self.assertEqual([stage["stage"] for stage, _ in self.resolutions], ["bootstrap", "runtime", "qt-builder"])
        for stage, kwargs in self.resolutions[1:]:
            self.assertEqual([item["stage"] for item in stage["prerequisites"]], ["bootstrap"])
            self.assertEqual(set(kwargs["prerequisite_bundles"]), {"bootstrap"})
            self.assertEqual(kwargs["timeout"], 37)
        for bundle in inputs["apt_bundles"]:
            directory = self.output / "materials" / bundle["path"]
            manifest = apt.validate_materialized_inputs(directory)
            self.assertEqual(bundle["manifest"], manifest)
            self.assertEqual(bundle["manifest_sha256"], apt.sha256(directory / "manifest.json"))
        args, _ = build._validate_inputs(inputs, FAMILY, self.catalog["families"][FAMILY],
                                        self.output / "materials", self.repo / "docker/Dockerfile")
        self.assertEqual(args["APT_RUNTIME_LOCK_SHA256"], inputs["apt_bundles"][1]["manifest"]["runtime_lock"]["sha256"])
        provenance = ci.load_json(self.output / "materials/inputs/material-manifest.json")
        self.assertEqual(provenance["definition"], self.definition["families"][FAMILY])
        self.assertEqual(provenance["assets"]["installer"], self.definition["assets"]["installer"])
        self.assertFalse(any(path.name.startswith(".materialize-") for path in self.root.iterdir()))

    def test_wrong_download_hash_or_mutated_resolver_bytes_never_publish_partial_output(self):
        for mode in ("download", "apt"):
            with self.subTest(mode=mode):
                def bad_download(url, destination, **kwargs):
                    destination.write_bytes(b"wrong bytes")
                def bad_resolve(stage, destination, **kwargs):
                    result = self.resolve(stage, destination, **kwargs)
                    (destination / "runtime.lock").write_bytes(b"tampered")
                    return result
                with self.assertRaises(ci.ContractError):
                    self.module.materialize(self.repo, FAMILY, self.output,
                        downloader=bad_download if mode == "download" else self.download,
                        resolver=bad_resolve if mode == "apt" else self.resolve)
                self.assertFalse(self.output.exists())

    def test_invalid_definitions_fail_before_any_acquisition(self):
        original = copy.deepcopy(self.definition)
        changes = [
            lambda d: d["families"][FAMILY]["build_args"].update(KLOGG_APT_STAGE="online"),
            lambda d: d["families"][FAMILY].update(base_image="ubuntu:jammy"),
            lambda d: d["assets"]["installer"].update(sha256="0" * 64),
            lambda d: d["assets"]["installer"].update(url="http://example.invalid/file"),
            lambda d: d["assets"]["installer"].update(url="https://example.invalid:invalid/file"),
            lambda d: d["families"][FAMILY]["apt_stages"][1].update(acquisition=None),
            lambda d: d["families"][FAMILY]["downloads"][0].update(path="../escape"),
            lambda d: d["families"][FAMILY]["apt_stages"][0].update(prerequisites=["runtime"]),
            lambda d: d["families"][FAMILY]["downloads"].append({"asset": "installer", "path": "inputs/apt/manifest.json"}),
        ]
        for change in changes:
            with self.subTest(change=changes.index(change)):
                self.definition = copy.deepcopy(original)
                change(self.definition)
                self.save()
                with self.assertRaises(ci.ContractError):
                    self.materialize()
                self.assertEqual(self.downloads, [])
                self.assertEqual(self.resolutions, [])

    def test_download_uses_verified_https_and_bounds_source_bytes(self):
        class Response(io.BytesIO):
            def geturl(self):
                return "https://example.invalid/verified-source"
        opener = mock.Mock()
        opener.open.return_value = Response(PAYLOAD)
        with mock.patch.object(self.module.urllib.request, "build_opener", return_value=opener):
            self.module.download("https://example.invalid/source", self.root / "download", timeout=11)
        self.assertEqual((self.root / "download").read_bytes(), PAYLOAD)
        self.assertEqual(opener.open.call_args.kwargs["timeout"], 11)
        redirect = self.module._SecureRedirect()
        request = self.module.urllib.request.Request("https://example.invalid/source")
        with self.assertRaises(ci.ContractError):
            redirect.redirect_request(request, None, 302, "Found", {}, "http://example.invalid/source")
        opener.open.return_value = Response(PAYLOAD)
        with mock.patch.object(self.module, "MAX_ARCHIVE_BYTES", 2), \
                mock.patch.object(self.module.urllib.request, "build_opener", return_value=opener):
            with self.assertRaises(ci.ContractError):
                self.module.download("https://example.invalid/source", self.root / "oversized", timeout=11)

    def test_output_reuse_and_symlink_are_rejected_without_acquisition(self):
        self.output.mkdir()
        with self.assertRaises(ci.ContractError):
            self.materialize()
        self.output.rmdir()
        self.output.symlink_to(self.root / "absent", target_is_directory=True)
        with self.assertRaises(ci.ContractError):
            self.materialize()
        self.assertFalse(self.downloads or self.resolutions)

    def test_scoped_keys_are_verified_locally_not_fetched_or_globally_imported(self):
        key = b"-----BEGIN PGP PUBLIC KEY BLOCK-----\nfixture\n"
        directory = self.repo / "ci/environments/keys"
        directory.mkdir()
        (directory / "toolchain.asc").write_bytes(key)
        runtime = self.definition["families"][FAMILY]["apt_stages"][1]
        runtime["sources"].append("deb https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu focal main")
        runtime["source_keyrings"] = [{"source_index": 1, "keys": [{"name": "toolchain.asc",
            "sha256": sha(key), "fingerprints": ["A" * 40]}]}]
        self.save()
        self.materialize()
        self.assertEqual(set(self.resolutions[1][1]["keyring_files"]), {"toolchain.asc"})
        self.assertEqual(len(self.downloads), 1)

    def test_safe_tar_rejects_traversal_links_special_files_and_duplicate_paths(self):
        cases = [[("../escape", b"bad")], [("root/link", ("../../escape",))],
                 [("same", b"one"), ("same", b"two")],
                 [("link", ("target",)), ("link/payload", b"bad")]]
        for index, members in enumerate(cases):
            with self.subTest(index=index):
                archive = self.root / (str(index) + ".tar")
                archive.write_bytes(archive_bytes(members))
                with self.assertRaises(ci.ContractError):
                    self.module.extract_archive(archive, self.root / ("tree" + str(index)), "tar", timeout=10)
        self.assertFalse((self.root / "escape").exists())

    def test_safe_zip_rejects_duplicate_and_parent_traversal(self):
        archive = self.root / "bad.zip"
        with zipfile.ZipFile(archive, "w") as stream:
            stream.writestr("../escape", b"bad")
        with self.assertRaises(ci.ContractError):
            self.module.extract_archive(archive, self.root / "tree", "zip", timeout=10)
        self.assertFalse((self.root / "escape").exists())

    def test_7z_preflight_rejects_link_ancestors_before_extractor_runs(self):
        calls = []
        archive = self.root / "archive.7z"
        archive.write_bytes(b"fake archive")
        listing = ("Path = link\nSize = 4\nAttributes = A_ lrwxrwxrwx\nEncrypted = -\n\n"
                   "Path = link/payload\nSize = 1\nAttributes = A_ -rw-r--r--\nEncrypted = -\n\n")
        def runner(command, **kwargs):
            calls.append(command)
            return subprocess.CompletedProcess(command, 0, listing, "")
        with self.assertRaises(ci.ContractError):
            self.module.extract_archive(archive, self.root / "tree", "7z", timeout=10, runner=runner)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][1], "l")

    def test_deterministic_tools_tar_preserves_executables_licenses_and_internal_links(self):
        tree = self.root / "tree"
        (tree / "bin").mkdir(parents=True)
        (tree / "bin/ninja").write_bytes(b"ELF fixture")
        (tree / "bin/ninja").chmod(0o755)
        (tree / "LICENSE").write_bytes(b"license text")
        (tree / "bin/alias").symlink_to("ninja")
        first, second = self.root / "first.tar", self.root / "second.tar"
        self.module.write_tools_tar(tree, first)
        os.utime(tree / "bin/ninja", (1234, 1234))
        self.module.write_tools_tar(tree, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.module.extract_archive(first, self.root / "restored", "tar", timeout=10)
        self.assertEqual((self.root / "restored/bin/ninja").stat().st_mode & 0o777, 0o755)
        self.assertTrue((self.root / "restored/bin/alias").is_symlink())
        self.assertEqual((self.root / "restored/LICENSE").read_bytes(), b"license text")

    def test_qt_relocation_is_target_fixed_and_preserves_other_configuration(self):
        qt = self.root / "qt/6.9.3/gcc_64"
        for relative in ("bin", "mkspecs", "lib/pkgconfig"):
            (qt / relative).mkdir(parents=True, exist_ok=True)
        (qt / "bin/qmake").write_bytes(b"not executable on the host")
        # The real SHA-locked Qt 6.9.3 kit has neither Qt5 edition nor licheck fields.
        configuration = "QT_ARCH = x86_64\nQT_VERSION = 6.9.3\nQT_CONFIG += shared openssl release\n"
        (qt / "mkspecs/qconfig.pri").write_text(configuration)
        (qt / "lib/pkgconfig/Qt6Core.pc").write_text("prefix=/home/qt/work/install\nlibdir=${prefix}/lib\n")
        self.module.relocate_qt(qt)
        self.assertEqual((qt / "bin/qt.conf").read_text(), "[Paths]\nPrefix=..\n")
        self.assertEqual((qt / "mkspecs/qconfig.pri").read_text(), configuration)
        self.assertIn("prefix=/opt/klogg-tools/qt/6.9.3/gcc_64", (qt / "lib/pkgconfig/Qt6Core.pc").read_text())
        self.assertNotIn(str(self.root), (qt / "lib/pkgconfig/Qt6Core.pc").read_text())

    def test_full_tool_pipeline_retains_provenance_and_repeats_identical_archive_bytes(self):
        kit = [
            ("bin/qmake", b"ELF qmake fixture"),
            ("mkspecs/qconfig.pri", b"QT_ARCH = x86_64\nQT_VERSION = 6.9.3\n"),
            ("lib/cmake/Qt6/Qt6Config.cmake", b"relative CMake config"),
            ("lib/cmake/Qt6Core5Compat/Qt6Core5CompatConfig.cmake", b"compat config"),
            ("lib/pkgconfig/Qt6Core.pc", b"prefix=/home/qt/work/install\n"),
            ("LICENSE", b"vendor license must be retained"),
            ("lib/alias", ("../LICENSE",)),
        ]
        payloads = {
            "cmake": archive_bytes([("vendor/bin/cmake", b"ELF cmake fixture"), ("vendor/LICENSE", b"BSD license")]),
            "boost": archive_bytes([("boost_1_86_0/boost/version.hpp", b"#define BOOST_VERSION 108600\n")]),
            "qt": archive_bytes(kit),
        }
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, "w") as archive:
            member = zipfile.ZipInfo("ninja")
            member.external_attr = (stat.S_IFREG | 0o755) << 16
            archive.writestr(member, b"ELF ninja fixture")
        payloads["ninja"] = stream.getvalue()
        family = self.definition["families"][FAMILY]
        family["downloads"] = []
        family["tools"] = []
        for name, source_root, destination, format_name in [
            ("cmake", "vendor", "cmake", "tar"), ("ninja", ".", "bin", "zip"),
            ("boost", "boost_1_86_0", "boost", "tar"), ("qt", ".", "qt/6.9.3/gcc_64", "tar"),
        ]:
            self.definition["assets"][name] = dict(self.definition["assets"]["installer"],
                url="https://example.invalid/" + name, sha256=sha(payloads[name]))
            family["tools"].append({"asset": name, "source_root": source_root,
                                    "destination": destination, "format": format_name})
        self.save()
        def fetch(url, destination, **kwargs):
            destination.write_bytes(payloads[url.rsplit("/", 1)[-1]])
        first = self.module.materialize(self.repo, FAMILY, self.output, downloader=fetch, resolver=self.resolve)
        second_path = self.root / "second"
        second = self.module.materialize(self.repo, FAMILY, second_path, downloader=fetch, resolver=self.resolve)
        self.assertEqual(first["build_args"]["TOOLS_ARCHIVE_SHA256"], second["build_args"]["TOOLS_ARCHIVE_SHA256"])
        self.assertEqual((self.output / "materials/inputs/tools.tar").read_bytes(),
                         (second_path / "materials/inputs/tools.tar").read_bytes())
        with tarfile.open(self.output / "materials/inputs/tools.tar") as archive:
            self.assertEqual(archive.extractfile("qt/6.9.3/gcc_64/LICENSE").read(), b"vendor license must be retained")
            self.assertEqual(archive.getmember("bin/ninja").mode, 0o755)
            self.assertTrue(archive.getmember("qt/6.9.3/gcc_64/lib/alias").issym())
        from prefetch_adb_helper_sources import safe_extract
        safe_extract(self.output / "materials/inputs/tools.tar", self.root / "consumer-tools")
        self.assertEqual((self.root / "consumer-tools/qt/6.9.3/gcc_64/bin/qt.conf").read_text(), "[Paths]\nPrefix=..\n")
        manifest = ci.load_json(self.output / "materials/inputs/material-manifest.json")
        relocation = next(item for item in manifest["transforms"] if item["operation"] == "qt6-relocation-v1")
        self.assertTrue(relocation["changes"])
        self.assertNotIn(str(self.root), json.dumps(manifest))

    @unittest.skipUnless(shutil.which("7z"), "7z is an analysis materialization prerequisite")
    def test_real_7z_extracts_tiny_valid_unix_archive_with_internal_link(self):
        source = self.root / "source"
        source.mkdir()
        (source / "file").write_bytes(b"payload")
        (source / "link").symlink_to("file")
        archive = self.root / "tiny.7z"
        subprocess.run(["7z", "a", "-snl", str(archive), str(source)],
                       check=True, capture_output=True, timeout=10)
        self.module.extract_archive(archive, self.root / "unpacked", "7z", timeout=10)
        self.assertEqual((self.root / "unpacked/source/file").read_bytes(), b"payload")
        self.assertEqual(os.readlink(self.root / "unpacked/source/link"), "file")

    def test_7z_final_directory_record_can_have_empty_metadata_values(self):
        archive = self.root / "directory-last.7z"
        archive.write_bytes(b"fake archive")
        listing = ("Path = bin\nSize = 0\nAttributes = D_ drwxr-xr-x\n"
                   "CRC = \nEncrypted = -\nMethod = \nBlock = \n\n")
        def runner(command, **kwargs):
            if command[1] == "x":
                output = pathlib.Path(next(arg[2:] for arg in command if arg.startswith("-o")))
                (output / "bin").mkdir()
            return subprocess.CompletedProcess(command, 0, listing, "")
        self.module.extract_archive(archive, self.root / "unpacked", "7z", timeout=10, runner=runner)
        self.assertTrue((self.root / "unpacked/bin").is_dir())

    def test_7z_extracted_link_target_and_actual_types_are_checked_before_merge(self):
        archive = self.root / "tiny.7z"
        archive.write_bytes(b"fake archive")
        listing = "Path = link\nSize = 10\nAttributes = A_ lrwxrwxrwx\nEncrypted = -\n\n"
        def runner(command, **kwargs):
            if command[1] == "x":
                output = pathlib.Path(next(arg[2:] for arg in command if arg.startswith("-o")))
                (output / "link").symlink_to("../outside")
            return subprocess.CompletedProcess(command, 0, listing, "")
        with self.assertRaises(ci.ContractError):
            self.module.extract_archive(archive, self.root / "unpacked", "7z", timeout=10, runner=runner)
        self.assertFalse((self.root / "outside").exists())

    def test_conflicting_tool_modules_and_link_destinations_fail_closed(self):
        source, destination = self.root / "source", self.root / "tree"
        source.mkdir()
        destination.mkdir()
        (source / "file").write_bytes(b"new")
        (destination / "file").write_bytes(b"old")
        with self.assertRaises(ci.ContractError):
            self.module.merge_tree(source, destination)
        self.assertEqual((destination / "file").read_bytes(), b"old")
        linked = self.root / "alias"
        linked.symlink_to(destination, target_is_directory=True)
        with self.assertRaises(ci.ContractError):
            self.module.merge_tree(source, linked)

    def test_dependent_stage_declarations_are_fully_validated_before_downloads(self):
        family = self.definition["families"][FAMILY]
        family["apt_stages"][1]["sources"] = ["deb [trusted=yes] https://example.invalid jammy main"]
        self.save()
        with self.assertRaises(ci.ContractError):
            self.materialize()
        self.assertFalse(self.downloads or self.resolutions)

    def test_changed_material_declarations_do_not_publish(self):
        def fetch(url, destination, **kwargs):
            self.download(url, destination, **kwargs)
            self.definition["assets"]["installer"]["license"] = "modified during acquisition"
            self.save()
        with self.assertRaisesRegex(ci.ContractError, "declarations changed"):
            self.module.materialize(self.repo, FAMILY, self.output, downloader=fetch, resolver=self.resolve)
        self.assertFalse(self.output.exists())

    def test_checked_in_definitions_cover_six_families_without_codeql_or_unpinned_assets(self):
        catalog = ci.load_json(ROOT / "ci/environments/recipes.json")
        definitions = ci.load_json(ROOT / "ci/environments/materials.json")
        self.assertEqual(set(definitions["families"]), set(catalog["families"]))
        self.assertNotIn("codeql", json.dumps(definitions).lower())
        for family in catalog["families"]:
            self.module.load_definition(ROOT, family)
        stages = definitions["families"]["jammy-qt5-tsan"]["apt_stages"]
        self.assertEqual(stages[0]["requested_packages"], ["ca-certificates"])
        self.assertEqual([stage["prerequisites"] for stage in stages[1:]], [["bootstrap"]] * 3)
        for stage in stages:
            self.assertEqual(stage["acquisition"]["snapshot"]["timestamp"], "20260731T000000Z")
        self.assertEqual(definitions["families"]["resolute-qt6"]["downloads"], [])


if __name__ == "__main__":
    unittest.main()
