"""Pinned APT closure contracts; all Docker/APT interactions are isolated fakes."""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).parents[2]
SCRIPT = ROOT / "scripts" / "ci_environment_inputs.py"
INSTALLER = ROOT / "scripts" / "ci_install_locked_apt.sh"
# Synthetic fixture identity, never a proposed production image digest.
BASE = "example.invalid/ubuntu@sha256:" + "a" * 64
LOCAL_IMAGE = "sha256:" + "b" * 64  # Fake buildx output, never a proposed pin.
BASE_PACKAGES = "base-files\t1.0\tamd64\tinstalled\n"


def descriptor():
    return {"schema_version": 1, "stage": "build", "platform": "linux/amd64",
            "base_image": BASE, "sources": ["deb http://archive.ubuntu.com/ubuntu jammy main"],
            "requested_packages": ["example"], "acquisition": {"mode": "apt-signed"}}


def payload(root, package="example", repositories=1):
    (root / "debs").mkdir(exist_ok=True)
    (root / "lists").mkdir(exist_ok=True)
    (root / "debs" / (package + "_1.2_amd64.deb")).write_bytes(b"authenticated fake deb\n")
    targets = []
    for index in range(repositories):
        prefix = "repo_dists_jammy" if index == 0 else "repo" + str(index) + "_dists_jammy"
        (root / "lists" / (prefix + "_InRelease")).write_bytes(b"authenticated release\n")
        target = prefix + "_main_binary-amd64_Packages.lz4"
        (root / "lists" / target).write_bytes(b"authenticated index\n")
        targets.append(target)
    (root / "base-packages.tsv").write_text(BASE_PACKAGES)
    (root / "index-targets.txt").write_text("\n".join(targets) + "\n")
    (root / "resolution.tsv").write_text(package + "\t1.2\tamd64\t" + package + "_1.2_amd64.deb\n")


class AptInputsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SCRIPT.is_file():
            raise AssertionError("producer APT input closure implementation is missing")
        sys.path.insert(0, str(SCRIPT.parent))
        try:
            spec = importlib.util.spec_from_file_location("ci_environment_inputs", SCRIPT)
            cls.module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(cls.module)
        finally:
            sys.path.pop(0)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="apt-inputs-")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.commands = []
        self.prepared_files = []

    def docker(self, command, **kwargs):
        self.commands.append(command)
        self.assertEqual(command[0], "docker")
        if command[1] in ("rm", "pull"):
            return subprocess.CompletedProcess(command, 0, "", "")
        if command[1:3] == ["buildx", "build"]:
            self.assertIn("--load", command)
            self.assertIn("--network=none", command)
            dockerfile = pathlib.Path(command[command.index("--file") + 1]).read_text()
            self.prepared_files.append(dockerfile)
            pathlib.Path(command[command.index("--iidfile") + 1]).write_text(LOCAL_IMAGE + "\n")
            return subprocess.CompletedProcess(command, 0, "", "")
        if command[1:3] == ["image", "inspect"]:
            return subprocess.CompletedProcess(command, 0, LOCAL_IMAGE + "\n", "")
        self.assertIn("--platform=linux/amd64", command)
        self.assertTrue(BASE in command or LOCAL_IMAGE in command)
        mounts = [command[i + 1] for i, token in enumerate(command[:-1]) if token == "--mount"]
        bundle = next(mount for mount in mounts if ",target=/inputs" in mount)
        directory = pathlib.Path(bundle.split("source=", 1)[1].split(",", 1)[0])
        if "--network=none" not in command:
            package = command[-1].split("=", 1)[0].split(":", 1)[0]
            repositories = 1
            if (directory / "keys").is_dir():
                sources = set()
                for line in (directory / "sources.list").read_text().splitlines():
                    fields = line.split()
                    if fields[1].startswith("["):
                        fields.pop(1)
                    sources.add((fields[1], fields[2]))
                repositories = len(sources)
            payload(directory, package, repositories)
        else:
            self.assertIn("readonly", bundle)
            self.assertTrue((directory / "manifest.json").is_file())
        return subprocess.CompletedProcess(command, 0, "", "")

    def resolve(self):
        return self.module.resolve_stage(descriptor(), self.root / "bundle", runner=self.docker)

    def prerequisite(self, name, references=(), bundles=None, snapshot=False):
        stage = descriptor()
        stage["stage"] = name
        if references:
            stage["prerequisites"] = list(references)
        if snapshot:
            stage["sources"] = ["deb https://snapshot.ubuntu.com/ubuntu/20260731T000000Z/ jammy main"]
            stage["requested_packages"] = ["ca-certificates"]
            stage["acquisition"] = {"mode": "apt-signed", "snapshot": {
                "timestamp": "20260731T000000Z", "check_valid_until": False},
                "tls_bootstrap": {"host": "snapshot.ubuntu.com"}}
        path = self.root / name
        manifest = self.module.resolve_stage(stage, path, runner=self.docker,
                                             prerequisite_bundles=bundles or {})
        reference = {"stage": name, "manifest_sha256": hashlib.sha256((path / "manifest.json").read_bytes()).hexdigest(),
                     "runtime_lock_sha256": manifest["runtime_lock"]["sha256"]}
        return reference, path

    def test_prepared_base_loads_actual_image_and_offline_proof_replays_original_chain(self):
        first, first_path = self.prerequisite("first")
        second, second_path = self.prerequisite("second", [first], {"first": first_path})
        stage = descriptor()
        stage["prerequisites"] = [first, second]
        self.commands.clear()
        manifest = self.module.resolve_stage(stage, self.root / "bundle", runner=self.docker,
                                             prerequisite_bundles={"first": first_path, "second": second_path})
        prepared = self.prepared_files[-1]
        self.assertTrue(prepared.startswith("FROM " + BASE + "\n"))
        runs = [line for line in prepared.splitlines() if line.startswith("RUN ")]
        self.assertEqual(len(runs), 2)
        self.assertTrue(all(line.startswith("RUN --network=none ") for line in runs))
        self.assertIn(first["runtime_lock_sha256"], runs[0])
        self.assertIn(second["runtime_lock_sha256"], runs[1])
        acquisition, offline = [command for command in self.commands if command[1] == "run"]
        self.assertIn(LOCAL_IMAGE, acquisition)
        self.assertIn("--pull=never", acquisition)
        self.assertIn(BASE, offline)
        self.assertNotIn(LOCAL_IMAGE, offline)
        self.assertIn("--network=none", offline)
        proof = offline[offline.index("-ec") + 1]
        positions = [proof.index(ref["runtime_lock_sha256"]) for ref in (first, second)]
        positions.append(proof.index(manifest["runtime_lock"]["sha256"]))
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(manifest["base_image"], BASE)
        self.assertEqual(manifest["prerequisites"], [first, second])
        self.assertFalse(any("--tag" in command or command[1:3] == ["image", "rm"] for command in self.commands))
        lock = (self.root / "bundle" / "runtime.lock").read_text()
        self.assertIn("prerequisite\tfirst\t" + first["manifest_sha256"], lock)

    def test_prerequisite_substitution_and_nonprefix_chains_fail_before_docker(self):
        first, path = self.prerequisite("first")
        independent, independent_path = self.prerequisite("independent")
        stage = descriptor()
        stage["prerequisites"] = [first]
        cases = []
        for field in ("manifest_sha256", "runtime_lock_sha256"):
            changed = copy.deepcopy(stage)
            changed["prerequisites"][0][field] = "c" * 64
            cases.append((changed, {"first": path}))
        cases.extend([
            (stage, {}),
            (stage, {"first": path, "unexpected": path}),
            ({**stage, "base_image": "example.invalid/other@sha256:" + "d" * 64}, {"first": path}),
            ({**stage, "prerequisites": [first, first]}, {"first": path}),
            ({**stage, "stage": "first"}, {"first": path}),
            ({**stage, "prerequisites": [first, independent]}, {"first": path, "independent": independent_path}),
        ])
        self.commands.clear()
        for invalid, bundles in cases:
            with self.subTest(invalid=invalid, bundles=bundles):
                with self.assertRaises(self.module.InputError):
                    self.module.resolve_stage(invalid, self.root / "bad", runner=self.docker,
                                              prerequisite_bundles=bundles)
        self.assertEqual(self.commands, [])
        (path / "debs" / "example_1.2_amd64.deb").write_bytes(b"tampered")
        with self.assertRaises(self.module.InputError):
            self.module.resolve_stage(stage, self.root / "bad", runner=self.docker,
                                      prerequisite_bundles={"first": path})
        self.assertEqual(self.commands, [])

    def test_prepared_image_must_be_loaded_and_identified_not_cache_only(self):
        reference, path = self.prerequisite("first")
        stage = {**descriptor(), "prerequisites": [reference]}
        for broken in ("missing-iid", "invalid-iid", "unloaded"):
            with self.subTest(broken=broken):
                def runner(command, **kwargs):
                    result = self.docker(command, **kwargs)
                    if command[1:3] == ["buildx", "build"]:
                        iid = pathlib.Path(command[command.index("--iidfile") + 1])
                        if broken == "missing-iid":
                            iid.unlink()
                        elif broken == "invalid-iid":
                            iid.write_text("mutable:tag\n")
                    elif command[1:3] == ["image", "inspect"] and broken == "unloaded":
                        raise subprocess.CalledProcessError(1, command, stderr="image was not loaded")
                    return result
                with self.assertRaises(self.module.InputError):
                    self.module.resolve_stage(stage, self.root / "bad", runner=runner,
                                              prerequisite_bundles={"first": path})
                self.assertFalse((self.root / "bad").exists())

    def test_snapshot_bootstrap_is_explicit_and_downstream_tls_is_normal(self):
        reference, path = self.prerequisite("ca", snapshot=True)
        bootstrap = [command for command in self.commands if command[1] == "run"][0]
        self.assertIn("KLOGG_TLS_BOOTSTRAP=snapshot.ubuntu.com", bootstrap)
        self.assertIn("KLOGG_CHECK_VALID_UNTIL=false", bootstrap)
        stage = descriptor()
        stage["sources"] = ["deb https://snapshot.ubuntu.com/ubuntu/20260731T000000Z/ jammy main"]
        stage["prerequisites"] = [reference]
        stage["acquisition"] = {"mode": "apt-signed", "snapshot": {
            "timestamp": "20260731T000000Z", "check_valid_until": False}}
        self.commands.clear()
        self.module.resolve_stage(stage, self.root / "bundle", runner=self.docker,
                                  prerequisite_bundles={"ca": path})
        acquisition = next(command for command in self.commands if command[1] == "run")
        self.assertIn("KLOGG_TLS_BOOTSTRAP=", acquisition)
        self.assertNotIn("KLOGG_TLS_BOOTSTRAP=snapshot.ubuntu.com", acquisition)
        self.assertIn("https://snapshot.ubuntu.com/ubuntu/20260731T000000Z/",
                      (self.root / "bundle" / "sources.list").read_text())

    def test_snapshot_requires_matching_ca_bundle_not_merely_any_prefix(self):
        unrelated, unrelated_path = self.prerequisite("unrelated")
        ca, ca_path = self.prerequisite("ca", snapshot=True)
        for reference, path, stamp in ((unrelated, unrelated_path, "20260731T000000Z"),
                                       (ca, ca_path, "20260730T000000Z")):
            with self.subTest(reference=reference, stamp=stamp):
                stage = descriptor()
                stage["sources"] = ["deb https://snapshot.ubuntu.com/ubuntu/" + stamp + "/ jammy main"]
                stage["prerequisites"] = [reference]
                stage["acquisition"] = {"mode": "apt-signed", "snapshot": {
                    "timestamp": stamp, "check_valid_until": False}}
                self.commands.clear()
                with self.assertRaises(self.module.InputError):
                    self.module.resolve_stage(stage, self.root / "bad", runner=self.docker,
                                              prerequisite_bundles={reference["stage"]: path})
                self.assertEqual(self.commands, [])

    def test_snapshot_policy_rejects_scope_expansion_and_missing_ca_chain(self):
        stage = descriptor()
        stage["sources"] = ["deb https://snapshot.ubuntu.com/ubuntu/20260731T000000Z/ jammy main"]
        stage["requested_packages"] = ["ca-certificates"]
        stage["acquisition"] = {"mode": "apt-signed", "snapshot": {
            "timestamp": "20260731T000000Z", "check_valid_until": False},
            "tls_bootstrap": {"host": "snapshot.ubuntu.com"}}
        self.module.validate_stage(stage)
        bad = []
        for source in ("deb http://snapshot.ubuntu.com/ubuntu/20260731T000000Z/ jammy main",
                       "deb https://snapshot.ubuntu.com/ubuntu/20260730T000000Z/ jammy main",
                       "deb https://archive.ubuntu.com/ubuntu/ jammy main"):
            bad.append({**stage, "sources": [source]})
        bad.append({**stage, "requested_packages": ["ca-certificates", "curl"]})
        for key, value in (("tls_bootstrap", {"host": "archive.ubuntu.com"}),
                           ("snapshot", {"timestamp": "20261331T000000Z", "check_valid_until": False}),
                           ("snapshot", {"timestamp": "20260731T000000Z", "check_valid_until": "false"})):
            changed = copy.deepcopy(stage)
            changed["acquisition"][key] = value
            bad.append(changed)
        downstream = copy.deepcopy(stage)
        del downstream["acquisition"]["tls_bootstrap"]
        bad.append(downstream)
        for invalid in bad:
            with self.subTest(invalid=invalid):
                with self.assertRaises(self.module.InputError):
                    self.module.validate_stage(invalid)

    def keyed_stage(self):
        # Synthetic parser fixtures, not public signing keys or production pins.
        keys = []
        paths = {}
        for name, fingerprint in (("current.asc", "A" * 40), ("legacy.asc", "B" * 40)):
            path = self.root / name
            path.write_bytes(("synthetic armored fixture " + name + "\n").encode("ascii"))
            paths[name] = path
            keys.append({"name": name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                         "fingerprints": [fingerprint]})
        stage = descriptor()
        stage["sources"] = ["deb http://archive.ubuntu.com/ubuntu focal main",
                            "deb https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu focal main"]
        stage["source_keyrings"] = [{"source_index": 1, "keys": keys}]
        return stage, paths

    def test_scoped_keys_are_retained_and_only_the_selected_source_gains_trust(self):
        stage, keys = self.keyed_stage()
        manifest = self.module.resolve_stage(stage, self.root / "bundle", runner=self.docker, keyring_files=keys)
        bundle = self.root / "bundle"
        sources = (bundle / "sources.list").read_text().splitlines()
        self.assertEqual(sources[0], stage["sources"][0])
        self.assertTrue(sources[1].startswith("deb [signed-by="))
        selectors = sources[1].split("[signed-by=", 1)[1].split("]", 1)[0].split(",")
        self.assertCountEqual(selectors, ["/inputs/keys/current.asc", "/inputs/keys/legacy.asc", "A" * 40, "B" * 40])
        self.assertTrue(sources[1].endswith(stage["sources"][1][4:]))
        records = {record["path"]: record for record in manifest["files"]}
        for key in stage["source_keyrings"][0]["keys"]:
            path = "keys/" + key["name"]
            self.assertEqual(records[path]["sha256"], key["sha256"])
            self.assertEqual((bundle / path).read_bytes(), keys[key["name"]].read_bytes())
            self.assertIn(path, (bundle / "runtime.lock").read_text())
        self.assertEqual(self.module.validate_materialized_inputs(bundle), manifest)
        commands = "\n".join(" ".join(command) for command in self.commands)
        self.assertNotIn("apt-key", commands)
        self.assertNotIn("keyserver", commands)
        self.assertNotIn("trusted=yes", commands)

    def test_scoped_public_keys_are_readable_by_apt_under_private_umask(self):
        stage, keys = self.keyed_stage()
        def runner(command, **kwargs):
            if command[1] == "run" and "--network=none" not in command:
                mount = next(command[index + 1] for index, value in enumerate(command[:-1])
                             if value == "--mount" and ",target=/inputs" in command[index + 1])
                root = pathlib.Path(mount.split("source=", 1)[1].split(",", 1)[0])
                self.assertEqual(root.stat().st_mode & 0o005, 0o005)
                self.assertEqual((root / "sources.list").stat().st_mode & 0o004, 0o004)
                for path in (root / "keys").iterdir():
                    self.assertEqual(path.stat().st_mode & 0o004, 0o004)
            return self.docker(command, **kwargs)
        previous = os.umask(0o077)
        try:
            self.module.resolve_stage(stage, self.root / "bundle", runner=runner, keyring_files=keys)
        finally:
            os.umask(previous)

    def test_cli_accepts_separately_supplied_named_key_files(self):
        stage, keys = self.keyed_stage()
        stage_file = self.root / "stage.json"
        stage_file.write_text(json.dumps(stage))
        arguments = ["resolve", "--stage", str(stage_file), "--output", str(self.root / "bundle")]
        for name, path in keys.items():
            arguments.extend(["--keyring", name + "=" + str(path)])
        with mock.patch.object(self.module, "resolve_stage", return_value={}) as resolve:
            self.assertEqual(self.module.main(arguments), 0)
        self.assertEqual(resolve.call_args.kwargs["keyring_files"], keys)

    def test_keyring_scope_schema_rejects_wrong_indexes_fingerprints_and_raw_options(self):
        stage, _ = self.keyed_stage()
        mutations = []
        for index in (-1, 2, True, "1", 0):
            changed = copy.deepcopy(stage)
            changed["source_keyrings"][0]["source_index"] = index
            mutations.append(changed)
        for name in ("../current.asc", "/current.asc", "current.gpg", "current asc.asc"):
            changed = copy.deepcopy(stage)
            changed["source_keyrings"][0]["keys"][0]["name"] = name
            mutations.append(changed)
        for fingerprints in ([], ["a" * 40], ["A" * 39], ["A" * 40 + "!"], ["0" * 40], ["A" * 40, "A" * 40]):
            changed = copy.deepcopy(stage)
            changed["source_keyrings"][0]["keys"][0]["fingerprints"] = fingerprints
            mutations.append(changed)
        mutations.append({**stage, "source_keyrings": stage["source_keyrings"] * 2})
        mutations.append({**stage, "source_keyrings": [{"source_index": 1, "keys": []}]})
        mutations.append({**stage, "sources": [stage["sources"][0],
                         "deb [trusted=yes] https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu focal main"]})
        mutations.append({**stage, "sources": [stage["sources"][0],
                         "deb https://ppa.launchpadcontent.net/unapproved/test/ubuntu focal main"]})
        for changed in mutations:
            with self.subTest(changed=changed):
                with self.assertRaises(self.module.InputError):
                    self.module.validate_stage(changed)
        self.assertEqual(self.commands, [])

    def test_missing_extra_tampered_or_linked_supplied_keys_fail_before_docker(self):
        stage, keys = self.keyed_stage()
        link = self.root / "link.asc"
        link.symlink_to(keys["current.asc"])
        changed = self.root / "changed.asc"
        changed.write_bytes(b"different key")
        for supplied in ({}, {"current.asc": keys["current.asc"]},
                         {**keys, "extra.asc": keys["current.asc"]},
                         {**keys, "current.asc": link}, {**keys, "current.asc": self.root},
                         {**keys, "current.asc": changed}):
            with self.subTest(supplied=supplied):
                with self.assertRaises(self.module.InputError):
                    self.module.resolve_stage(stage, self.root / "bad", runner=self.docker, keyring_files=supplied)
        self.assertEqual(self.commands, [])

    def test_retained_key_tampering_and_rehashed_arbitrary_source_options_are_rejected(self):
        stage, keys = self.keyed_stage()
        manifest = self.module.resolve_stage(stage, self.root / "bundle", runner=self.docker, keyring_files=keys)
        bundle = self.root / "bundle"
        for mutation in ("missing", "extra", "tampered", "linked"):
            with self.subTest(mutation=mutation):
                target = self.root / mutation
                shutil.copytree(bundle, target)
                key = target / "keys" / "current.asc"
                if mutation == "missing":
                    key.unlink()
                elif mutation == "extra":
                    (target / "keys" / "extra.asc").write_bytes(b"extra")
                elif mutation == "tampered":
                    key.write_bytes(b"tampered")
                else:
                    key.unlink()
                    key.symlink_to(keys["current.asc"])
                with self.assertRaises(self.module.InputError):
                    self.module.validate_materialized_inputs(target)
        source = bundle / "sources.list"
        source.write_text(stage["sources"][0] + "\n" + stage["sources"][1].replace("deb ", "deb [trusted=yes] ", 1) + "\n")
        # Internally consistent hashes must not replace the declared source policy.
        manifest["files"] = [self.module.file_record(bundle, item["path"]) for item in manifest["files"]]
        (bundle / "runtime.lock").write_bytes(self.module.runtime_bytes(manifest))
        manifest["runtime_lock"] = self.module.file_record(bundle, "runtime.lock")
        (bundle / "manifest.json").write_bytes(self.module.encoded(manifest))
        with self.assertRaises(self.module.InputError):
            self.module.validate_materialized_inputs(bundle)

    def test_scoped_ppa_preserves_independent_offline_prerequisite_replay(self):
        reference, prerequisite = self.prerequisite("base-tools")
        stage, keys = self.keyed_stage()
        stage["prerequisites"] = [reference]
        self.commands.clear()
        self.module.resolve_stage(stage, self.root / "bundle", runner=self.docker,
                                  prerequisite_bundles={"base-tools": prerequisite}, keyring_files=keys)
        offline = next(command for command in self.commands if command[1] == "run" and "--network=none" in command)
        self.assertIn(BASE, offline)
        self.assertNotIn(LOCAL_IMAGE, offline)
        self.assertIn(reference["runtime_lock_sha256"], offline[offline.index("-ec") + 1])

    def test_signature_rejection_for_a_wrong_valid_fingerprint_never_publishes(self):
        stage, keys = self.keyed_stage()
        stage["source_keyrings"][0]["keys"][0]["fingerprints"] = ["C" * 40]
        def rejecting_apt(command, **kwargs):
            raise subprocess.CalledProcessError(100, command, output="APT rejected repository signature: NO_PUBKEY")
        with self.assertRaises(self.module.InputError) as caught:
            self.module.resolve_stage(stage, self.root / "bad", runner=rejecting_apt, keyring_files=keys)
        self.assertIn("100", str(caught.exception))
        self.assertIn("NO_PUBKEY", str(caught.exception))
        self.assertFalse((self.root / "bad").exists())

    def test_resolve_authenticates_then_verifies_offline_before_publication(self):
        manifest = self.resolve()
        self.assertEqual(len(self.commands), 2)
        self.assertNotIn("--network=none", self.commands[0])
        self.assertIn("--network=none", self.commands[1])
        self.assertIn("--pull=never", self.commands[1])
        for token in ("APT::Update::Error-Mode=any", "Acquire::AllowInsecureRepositories=false",
                      "APT::Get::AllowUnauthenticated=false", "--download-only", "--reinstall",
                      "indextargets"):
            self.assertIn(token, self.commands[0][self.commands[0].index("-ec") + 1])
        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(manifest["stage"], "build")
        self.assertEqual(manifest["base_image"], BASE)
        self.assertEqual(manifest["requested_packages"], ["example"])
        self.assertEqual(manifest["packages"][0]["version"], "1.2")
        self.assertEqual(self.module.validate_materialized_inputs(self.root / "bundle"), manifest)
        self.assertFalse((self.root / "bundle" / "resolution.tsv").exists())

    def test_repeated_resolution_is_byte_identical(self):
        first = self.resolve()
        second = self.module.resolve_stage(descriptor(), self.root / "other", runner=self.docker)
        self.assertEqual(first, second)
        for name in ("manifest.json", "runtime.lock"):
            self.assertEqual((self.root / "bundle" / name).read_bytes(),
                             (self.root / "other" / name).read_bytes())

    def test_unsafe_or_unsupported_descriptors_fail_before_docker(self):
        mutations = [
            {"base_image": "ubuntu:22.04"},
            {"base_image": "ubuntu@sha256:" + "0" * 64},
            {"platform": "linux/arm64"}, {"schema_version": True},
            {"stage": "../build"}, {"requested_packages": ["--allow-unauthenticated"]},
            {"requested_packages": ["example;id"]}, {"requested_packages": ["example", "example"]},
            {"acquisition": {"mode": "apt-signed", "bootstrap_ca": True}},
            {"prerequisites": ["prior-stage"]},
        ]
        for source in ("deb [trusted=yes] http://example.org jammy main",
                       "deb http://example.org jammy main;id",
                       "deb http://ppa.launchpad.net/owner/repo/ubuntu focal main",
                       "deb https://ppa.launchpadcontent.net./owner/repo/ubuntu focal main",
                       "deb https://user:password@example.org/ubuntu jammy main"):
            mutations.append({"sources": [source]})
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                stage = descriptor()
                stage.update(mutation)
                with self.assertRaises(self.module.InputError):
                    self.module.resolve_stage(stage, self.root / "bad", runner=self.docker)
        self.assertEqual(self.commands, [])

    def test_https_sources_are_never_downgraded(self):
        stage = descriptor()
        stage["sources"] = ["deb https://archive.ubuntu.com/ubuntu jammy main"]
        self.module.resolve_stage(stage, self.root / "bundle", runner=self.docker)
        self.assertEqual((self.root / "bundle" / "sources.list").read_text(), stage["sources"][0] + "\n")

    def test_network_or_offline_failure_never_publishes_output(self):
        for fail_offline in (False, True):
            with self.subTest(offline=fail_offline):
                def failing(command, **kwargs):
                    if command[1] == "run" and ("--network=none" in command) == fail_offline:
                        raise subprocess.CalledProcessError(73, command, stderr="synthetic failure")
                    return self.docker(command, **kwargs)
                with self.assertRaises(self.module.InputError):
                    self.module.resolve_stage(descriptor(), self.root / "bundle", runner=failing)
                self.assertFalse((self.root / "bundle").exists())

    def test_failure_diagnostics_keep_phase_status_and_both_redacted_streams(self):
        def failing(command, **kwargs):
            raise subprocess.CalledProcessError(
                73, command, output="APT failed fetching https://alice:secret@example.invalid/index?token=secret",
                stderr="Image is up to date",
            )
        with self.assertRaises(self.module.InputError) as caught:
            self.module.resolve_stage(descriptor(), self.root / "bundle", runner=failing)
        message = str(caught.exception)
        for token in ("acquisition", "73", "APT failed fetching", "Image is up to date"):
            self.assertIn(token, message)
        self.assertNotIn("secret", message)
        self.assertNotIn("alice", message)

    def test_diagnostics_are_bounded_and_redact_case_insensitive_url_credentials(self):
        value = "x" * 8000 + " HTTPS://alice:secret@example.invalid/path?token=secret"
        message = self.module.bounded_diagnostics(value)
        self.assertLessEqual(len(message), 4110)
        self.assertTrue(message.startswith("[truncated]"))
        self.assertNotIn("secret", message)
        self.assertNotIn("alice", message)

    def test_timeout_cleanup_failure_keeps_both_operation_diagnostics(self):
        def fail(command, **kwargs):
            if command[1] == "run":
                raise subprocess.TimeoutExpired(command, 480, output="APT transfer still active")
            return subprocess.CompletedProcess(command, 17, "daemon refused container removal", "")
        with self.assertRaises(self.module.InputError) as caught:
            self.module.resolve_stage(descriptor(), self.root / "bundle", runner=fail, timeout=480)
        message = str(caught.exception)
        for token in ("acquisition", "timeout", "APT transfer still active", "container cleanup", "17", "daemon refused"):
            self.assertIn(token, message)

    def test_timeout_is_one_shared_budget_not_a_fresh_budget_per_child(self):
        timeouts = []
        def run(command, **kwargs):
            timeouts.append(kwargs["timeout"])
            return self.docker(command, **kwargs)
        with mock.patch.object(self.module.time, "monotonic", side_effect=[0, 10, 30]):
            self.module.resolve_stage(descriptor(), self.root / "bundle", runner=run, timeout=480)
        self.assertEqual(timeouts, [470, 450])

    def test_cli_exposes_bounded_local_timeout(self):
        stage_file = self.root / "stage.json"
        stage_file.write_text(json.dumps(descriptor()))
        with mock.patch.object(self.module, "resolve_stage", return_value={}) as resolve:
            self.assertEqual(self.module.main([
                "resolve", "--stage", str(stage_file), "--output", str(self.root / "bundle"),
                "--timeout", "480",
            ]), 0)
        self.assertEqual(resolve.call_args.kwargs["timeout"], 480)

    def test_client_timeout_removes_the_named_container_before_cleanup(self):
        commands = []
        def timeout(command, **kwargs):
            commands.append(command)
            if command[1] == "run":
                raise subprocess.TimeoutExpired(command, 1800)
            return subprocess.CompletedProcess(command, 0, "", "")
        with self.assertRaises(self.module.InputError):
            self.module.resolve_stage(descriptor(), self.root / "bundle", runner=timeout)
        self.assertEqual(len(commands), 2)
        name = commands[0][commands[0].index("--name") + 1]
        self.assertEqual(commands[1], ["docker", "rm", "--force", name])
        self.assertFalse((self.root / "bundle").exists())

    def test_missing_release_index_or_unreported_deb_cannot_be_published(self):
        for mutation in ("release", "index", "extra-deb", "missing-deb", "symlink"):
            with self.subTest(mutation=mutation):
                def malformed(command, **kwargs):
                    result = self.docker(command, **kwargs)
                    mount = next(command[i + 1] for i, value in enumerate(command[:-1])
                                 if value == "--mount" and ",target=/inputs" in command[i + 1])
                    root = pathlib.Path(mount.split("source=", 1)[1].split(",", 1)[0])
                    if mutation == "release":
                        (root / "lists" / "repo_dists_jammy_InRelease").unlink()
                    elif mutation == "index":
                        (root / "lists" / "repo_dists_jammy_main_binary-amd64_Packages.lz4").unlink()
                    elif mutation == "extra-deb":
                        (root / "debs" / "extra.deb").write_bytes(b"extra")
                    elif mutation == "missing-deb":
                        (root / "debs" / "example_1.2_amd64.deb").unlink()
                    else:
                        (root / "lists" / "outside").symlink_to(self.root)
                    return result
                with self.assertRaises(self.module.InputError):
                    self.module.resolve_stage(descriptor(), self.root / "bundle", runner=malformed)
                self.assertFalse((self.root / "bundle").exists())

    def test_every_declared_repository_requires_retained_release_metadata(self):
        stage = descriptor()
        stage["sources"].append("deb http://security.ubuntu.com/ubuntu jammy-security main")
        with self.assertRaises(self.module.InputError):
            self.module.resolve_stage(stage, self.root / "bundle", runner=self.docker)
        self.assertFalse((self.root / "bundle").exists())

    def run_fake_resolver(self, architecture="amd64", *, bootstrap=False, apt_status=91, update_warning=""):
        probe = pathlib.Path(tempfile.mkdtemp(prefix="probe-", dir=self.root))
        inputs = probe / "inputs"
        inputs.mkdir()
        (inputs / "lists").mkdir()
        (inputs / "lists" / "stale_Packages").write_bytes(b"stale")
        source = ("deb https://snapshot.ubuntu.com/ubuntu/20260731T000000Z/ jammy main"
                  if bootstrap else descriptor()["sources"][0])
        (inputs / "sources.list").write_text(source + "\n")
        fake_bin = probe / "bin"
        fake_bin.mkdir()
        log = probe / "calls"
        stubs = {
            "apt-get": r'''#!/bin/sh
printf '%s\n' "$@" >> "$FAKE_LOG"
if [ -f "$FAKE_INPUTS/.bootstrap-apt.conf" ]; then
    while IFS= read -r line; do printf 'bootstrap-config:%s\n' "$line" >> "$FAKE_LOG"; done < "$FAKE_INPUTS/.bootstrap-apt.conf"
fi
[ "$FAKE_APT_STATUS" -eq 0 ] || exit "$FAKE_APT_STATUS"
operation=
for argument do case "$argument" in update|install|indextargets) operation=$argument ;; esac; done
case "$operation" in
    update)
        if [ -n "$FAKE_UPDATE_WARNING" ]; then printf '%s\n' "$FAKE_UPDATE_WARNING" >&2; fi
        printf 'signed release\n' > "$FAKE_INPUTS/lists/repo_dists_jammy_InRelease"
        printf 'authenticated index\n' > "$FAKE_INPUTS/lists/repo_dists_jammy_main_binary-amd64_Packages"
        ;;
    install) printf 'authenticated deb\n' > "$FAKE_INPUTS/debs/${FAKE_PACKAGE}_1.2_amd64.deb" ;;
    indextargets) printf '%s\n' "$FAKE_INPUTS/lists/repo_dists_jammy_main_binary-amd64_Packages" ;;
    *) exit 99 ;;
esac
''',
            "dpkg-deb": '#!/bin/sh\ncase "$3" in Package) printf "%s\\n" "$FAKE_PACKAGE";; Version) printf "1.2\\n";; Architecture) printf "amd64\\n";; esac\n',
            "dpkg-query": '#!/bin/sh\nprintf "base-files\\t1.0\\tamd64\\tinstalled\\n"\n',
            "chown": '#!/bin/sh\nprintf "cleanup-owner\\n" >> "$FAKE_LOG"\n',
            "dpkg": '#!/bin/sh\nprintf "%s\\n" "$FAKE_ARCHITECTURE"\n',
            "uname": '#!/bin/sh\nprintf "Linux\\n"\n',
        }
        for name, body in stubs.items():
            stub = fake_bin / name
            stub.write_text(body)
            stub.chmod(0o755)
        env = dict(os.environ, PATH=str(fake_bin) + os.pathsep + os.environ["PATH"],
                   FAKE_LOG=str(log), KLOGG_OUTPUT_OWNER="501:20", FAKE_ARCHITECTURE=architecture,
                   FAKE_INPUTS=str(inputs), FAKE_APT_STATUS=str(apt_status), FAKE_UPDATE_WARNING=update_warning,
                   FAKE_PACKAGE="ca-certificates" if bootstrap else "example",
                   KLOGG_TLS_BOOTSTRAP="snapshot.ubuntu.com" if bootstrap else "",
                   KLOGG_CHECK_VALID_UNTIL="false" if bootstrap else "true")
        script = self.module.RESOLVE_SCRIPT.replace("/inputs", str(inputs))
        result = subprocess.run(["sh", "-c", script, "resolver", env["FAKE_PACKAGE"]],
                                env=env, capture_output=True, text=True, timeout=10)
        return result, log.read_text().splitlines(), inputs

    @unittest.skipUnless(shutil.which("sh"), "requires POSIX shell")
    def test_bootstrap_exception_is_narrow_and_removed_on_success_or_unsigned_failure(self):
        for status in (0, 100):
            with self.subTest(status=status):
                result, calls, inputs = self.run_fake_resolver(bootstrap=True, apt_status=status)
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertFalse((inputs / ".bootstrap-apt.conf").exists())
                self.assertIn('bootstrap-config:Acquire::https::snapshot.ubuntu.com::Verify-Peer "false";', calls)
                self.assertNotIn('bootstrap-config:Acquire::https::Verify-Peer "false";', calls)
                for option in ("Acquire::Check-Valid-Until=false", "APT::Get::AllowUnauthenticated=false",
                               "Acquire::AllowInsecureRepositories=false", "APT::Update::Error-Mode=any"):
                    self.assertIn(option, calls)
                self.assertEqual("install" in calls, status == 0)

    @unittest.skipUnless(shutil.which("sh"), "requires POSIX shell")
    def test_strict_update_failure_clears_stale_lists_and_never_downloads(self):
        result, calls, inputs = self.run_fake_resolver()
        self.assertEqual(result.returncode, 91, result.stderr)
        self.assertIn("APT::Update::Error-Mode=any", calls)
        self.assertNotIn("install", calls)
        self.assertIn("cleanup-owner", calls)
        self.assertEqual(list((inputs / "lists").iterdir()), [])

    @unittest.skipUnless(shutil.which("sh"), "requires POSIX shell")
    def test_zero_exit_partial_index_warnings_never_reach_install(self):
        for warning in ("W: Failed to fetch https://example.invalid/InRelease",
                        "W: Some index files failed to download. They have been ignored, or old ones used instead.",
                        "Err:1 https://example.invalid noble InRelease", "E: Invalid signature"):
            with self.subTest(warning=warning):
                result, calls, inputs = self.run_fake_resolver(apt_status=0, update_warning=warning)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("install", calls)
                self.assertIn(warning, result.stdout + result.stderr)
                self.assertFalse((inputs / "update.log").exists())

    @unittest.skipUnless(shutil.which("sh"), "requires POSIX shell")
    def test_actual_base_architecture_mismatch_never_reaches_apt(self):
        result, calls, _ = self.run_fake_resolver("arm64")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("update", calls)

    def test_validated_bundle_rejects_tamper_extras_missing_and_links(self):
        self.resolve()
        original = self.root / "bundle"
        for mutation in ("tamper", "extra", "missing", "symlink", "directory", "lock", "metadata"):
            with self.subTest(mutation=mutation):
                target = self.root / mutation
                shutil.copytree(original, target)
                deb = target / "debs" / "example_1.2_amd64.deb"
                if mutation == "tamper":
                    deb.write_bytes(b"changed")
                elif mutation == "extra":
                    (target / "debs" / "extra.deb").write_bytes(b"extra")
                elif mutation == "missing":
                    deb.unlink()
                elif mutation == "symlink":
                    deb.unlink()
                    deb.symlink_to(original / "debs" / deb.name)
                elif mutation == "directory":
                    (target / "unexpected").mkdir()
                elif mutation == "lock":
                    (target / "runtime.lock").write_text("schema_version\t2\n")
                else:
                    (target / "lists" / "repo_dists_jammy_InRelease").write_bytes(b"modified")
                with self.assertRaises(self.module.InputError):
                    self.module.validate_materialized_inputs(target)

    def test_manifest_rejects_duplicate_keys_path_traversal_and_identity_substitution(self):
        self.resolve()
        target = self.root / "bundle"
        manifest_file = target / "manifest.json"
        original = manifest_file.read_bytes()
        manifest_file.write_bytes(b'{"schema_version":1,' + original[1:])
        with self.assertRaises(self.module.InputError):
            self.module.validate_materialized_inputs(target)
        manifest_file.write_bytes(original)
        with self.assertRaises(self.module.InputError):
            self.module.validate_materialized_inputs(target, expected_stage={**descriptor(), "stage": "other"})
        document = json.loads(original)
        document["packages"][0]["path"] = "../outside.deb"
        manifest_file.write_text(json.dumps(document))
        with self.assertRaises(self.module.InputError):
            self.module.validate_materialized_inputs(target)

    def installer_stubs(self):
        fake_bin = self.root / "bin"
        fake_bin.mkdir()
        log = self.root / "apt-calls"
        for name, body in {
            "dpkg-query": '#!/bin/sh\nif [ "$#" -eq 2 ]; then printf "base-files\\t%s\\tamd64\\tinstalled\\n" "${FAKE_BASE_VERSION:-1.0}"; else printf "%s\\tamd64\\tinstalled\\n" "${FAKE_INSTALLED_VERSION:-1.2}"; fi\n',
            "dpkg-deb": '#!/bin/sh\ncase "$3" in Package) printf "%s\\n" "${FAKE_PACKAGE:-example}";; Version) printf "1.2\\n";; Architecture) printf "amd64\\n";; esac\n',
            "apt-get": r'''#!/bin/sh
printf '%s\n' "$@" >> "$FAKE_LOG"
if [ -n "${FAKE_SOURCE_LOG:-}" ]; then
    for argument do
        case "$argument" in
            Dir::Etc::sourcelist=*)
                source=${argument#*=}
                while IFS= read -r line; do printf '%s\n' "$line"; done < "$source" > "$FAKE_SOURCE_LOG"
                ;;
        esac
    done
fi
exit "${FAKE_APT_STATUS:-0}"
''',
            "dpkg": '#!/bin/sh\nprintf "%s\\n" "${FAKE_ARCHITECTURE:-amd64}"\n',
            "uname": '#!/bin/sh\nprintf "Linux\\n"\n',
        }.items():
            path = fake_bin / name
            path.write_text(body)
            path.chmod(0o755)
        env = dict(os.environ, PATH=str(fake_bin) + os.pathsep + os.environ["PATH"], FAKE_LOG=str(log))
        return env, log

    @unittest.skipUnless(shutil.which("sh"), "requires POSIX shell")
    def test_offline_scoped_sources_rewrite_only_key_paths_after_byte_verification(self):
        stage, keys = self.keyed_stage()
        # A URL containing the same text must not be changed by path relocation.
        stage["sources"][0] = "deb http://archive.ubuntu.com/ubuntu/inputs/keys/keep focal main"
        manifest = self.module.resolve_stage(stage, self.root / "bundle", runner=self.docker, keyring_files=keys)
        env, log = self.installer_stubs()
        source_log = self.root / "effective-sources"
        env["FAKE_SOURCE_LOG"] = str(source_log)
        command = ["sh", str(INSTALLER), str(self.root / "bundle"), manifest["runtime_lock"]["sha256"]]
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        sources = source_log.read_text().splitlines()
        self.assertEqual(sources[0], stage["sources"][0])
        self.assertIn("[signed-by=", sources[1])
        self.assertNotIn("/inputs/keys/", sources[1])
        for token in ("/keys/current.asc", "/keys/legacy.asc", "A" * 40, "B" * 40):
            self.assertIn(token, sources[1])
        self.assertTrue(sources[1].endswith(stage["sources"][1][4:]))
        self.assertIn("--no-download", log.read_text())
        (self.root / "bundle" / "keys" / "current.asc").write_bytes(b"tampered")
        log.unlink()
        source_log.unlink()
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(log.exists(), "unverified key reached APT")
        self.assertFalse(source_log.exists())

    @unittest.skipUnless(shutil.which("sh"), "requires POSIX shell")
    def test_offline_installer_accepts_authenticated_prerequisite_lock_records(self):
        reference, path = self.prerequisite("first")
        stage = {**descriptor(), "prerequisites": [reference]}
        manifest = self.module.resolve_stage(stage, self.root / "bundle", runner=self.docker,
                                             prerequisite_bundles={"first": path})
        env, log = self.installer_stubs()
        result = subprocess.run(
            ["sh", str(INSTALLER), str(self.root / "bundle"), manifest["runtime_lock"]["sha256"]],
            env=env, capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--no-download", log.read_text())

    @unittest.skipUnless(shutil.which("sh"), "requires POSIX shell")
    def test_offline_installer_verifies_before_apt_and_preserves_failure(self):
        manifest = self.resolve()
        bundle = self.root / "bundle"
        env, log = self.installer_stubs()
        command = ["sh", str(INSTALLER), str(bundle), manifest["runtime_lock"]["sha256"]]
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = log.read_text().splitlines()
        self.assertIn("--no-download", calls)
        self.assertIn("example:amd64=1.2", calls)
        self.assertNotIn("update", calls)
        env["FAKE_APT_STATUS"] = "73"
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 73, result.stderr)
        log.unlink()
        (bundle / "debs" / "example_1.2_amd64.deb").write_bytes(b"tampered")
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(log.exists(), "tampered payload reached APT")

    @unittest.skipUnless(shutil.which("sh"), "requires POSIX shell")
    def test_offline_installer_rejects_substitution_and_checks_installed_state(self):
        manifest = self.resolve()
        env, log = self.installer_stubs()
        for mutation in ("lock", "extra", "missing", "symlink", "hardlink", "package", "base", "architecture", "installed"):
            with self.subTest(mutation=mutation):
                target = self.root / mutation
                shutil.copytree(self.root / "bundle", target)
                local_env = env.copy()
                digest = manifest["runtime_lock"]["sha256"]
                deb = target / "debs" / "example_1.2_amd64.deb"
                if mutation == "lock":
                    digest = "0" * 64
                elif mutation == "extra":
                    (target / "debs" / "extra.deb").write_bytes(b"extra")
                elif mutation == "missing":
                    deb.unlink()
                elif mutation in ("symlink", "hardlink"):
                    deb.unlink()
                    source = self.root / "bundle" / "debs" / deb.name
                    if mutation == "symlink":
                        deb.symlink_to(source)
                    else:
                        os.link(source, deb)
                elif mutation == "package":
                    local_env["FAKE_PACKAGE"] = "substituted"
                elif mutation == "base":
                    local_env["FAKE_BASE_VERSION"] = "9.9"
                elif mutation == "architecture":
                    local_env["FAKE_ARCHITECTURE"] = "arm64"
                else:
                    local_env["FAKE_INSTALLED_VERSION"] = "9.9"
                log.unlink(missing_ok=True)
                result = subprocess.run(
                    ["sh", str(INSTALLER), str(target), digest], env=local_env,
                    capture_output=True, text=True, timeout=10,
                )
                self.assertNotEqual(result.returncode, 0, result.stderr)
                self.assertEqual(log.exists(), mutation == "installed",
                                 "verification failed to precede APT: " + mutation)
                if mutation == "hardlink":
                    deb.unlink()  # Restore the original bundle's link count.


if __name__ == "__main__":
    unittest.main()
