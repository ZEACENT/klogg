"""Behavioral tests for offline CI image identity and qualification contracts."""
from __future__ import annotations

import copy
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
import unittest

ROOT = pathlib.Path(__file__).parents[2]
SCRIPT = ROOT / "scripts" / "ci_environment.py"
SPEC = importlib.util.spec_from_file_location("ci_environment", SCRIPT)
ci = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ci)
OCI = "application/vnd.oci.image."
FAMILY = "jammy-qt5"
REGISTRY = "ghcr.io/zeacent/klogg-ci-env"


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def descriptor(data, media_type):
    return {"mediaType": media_type, "digest": digest(data), "size": len(data)}


def source():
    return {
        "repository": "ZEACENT/klogg",
        "sha": "a" * 40,
        "ref": "refs/heads/master",
        "workflow": ".github/workflows/ci-environment-producer.yml",
        "run_id": 123,
        "run_attempt": 2,
    }


def catalog():
    return {
        "schema_version": 1,
        "registry": REGISTRY,
        "families": {
            FAMILY: {
                "platform": "linux/amd64",
                "dockerfile": "docker/Dockerfile",
                "context": "docker",
                "recipe_files": ["docker/Dockerfile", "docker/install.sh"],
                "profiles": ["build", "asan"],
            }
        },
    }


class EnvironmentContractTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        (self.root / "docker").mkdir()
        (self.root / "docker/Dockerfile").write_text("FROM ubuntu:jammy\n", encoding="utf-8")
        (self.root / "docker/install.sh").write_text("#!/bin/sh\ntrue\n", encoding="utf-8")

    def archive(self, mutate=None, extra=None, compression="gzip", mode="w"):
        raw_layer = b"layer filesystem fixture"
        if compression == "gzip":
            layer = gzip.compress(raw_layer, mtime=0)
        elif compression == "zstd":
            # A single-segment Zstandard frame containing one final raw block.
            layer = b"\x28\xb5\x2f\xfd\x20" + bytes([len(raw_layer)])
            layer += ((len(raw_layer) << 3) | 1).to_bytes(3, "little") + raw_layer
        else:
            layer = raw_layer
        layer_type = OCI + "layer.v1.tar" + ("+" + compression if compression != "none" else "")
        config = {"architecture": "amd64", "os": "linux", "rootfs": {"type": "layers", "diff_ids": [digest(raw_layer)]}}
        manifest = {"schemaVersion": 2, "mediaType": OCI + "manifest.v1+json", "config": {}, "layers": [descriptor(layer, layer_type)]}
        index = {"schemaVersion": 2, "mediaType": OCI + "index.v1+json", "manifests": []}
        if mutate:
            mutate(config, manifest, index)
        config_bytes = encoded(config)
        manifest["config"] = descriptor(config_bytes, OCI + "config.v1+json")
        manifest_bytes = encoded(manifest)
        index["manifests"].insert(0, descriptor(manifest_bytes, OCI + "manifest.v1+json"))
        members = [
            ("oci-layout", encoded({"imageLayoutVersion": "1.0.0"})),
            ("index.json", encoded(index)),
            ("blobs/sha256/" + digest(manifest_bytes)[7:], manifest_bytes),
            ("blobs/sha256/" + digest(config_bytes)[7:], config_bytes),
            ("blobs/sha256/" + digest(layer)[7:], layer),
        ]
        if extra:
            extra(members)
        path = self.root / "candidate.tar"
        with tarfile.open(path, mode, format=tarfile.USTAR_FORMAT) as archive:
            for name, data in members:
                if isinstance(name, tarfile.TarInfo):
                    archive.addfile(name, io.BytesIO(data))
                else:
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    archive.addfile(info, io.BytesIO(data))
        return path, {
            "schema_version": 1,
            "platform": "linux/amd64",
            "archive_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "manifest_digest": digest(manifest_bytes),
            "config_digest": digest(config_bytes),
            "diff_ids": [digest(raw_layer)],
            "layer_digests": [digest(layer)],
        }

    def candidate(self):
        _, image = self.archive()
        return {"schema_version": 1, "kind": "candidate", "family": FAMILY,
                "recipe_digest": digest(b"recipe"), "input_digest": digest(b"inputs"),
                "source": source(), "image": image}

    def qualifications(self):
        candidate = self.candidate()
        policy = {"schema_version": 1, "family": FAMILY, "profiles": {
            profile: {"configuration": {"cmake_options": [], "sanitizer": "address" if profile == "asan" else "",
                                         "package": False, "role": "build"},
                      "verification_files": {"scripts/test.py": digest(b"script")}, "role_materials": []}
            for profile in ("build", "asan")}}
        receipts = [{"schema_version": 1, "kind": "qualification-receipt", "profile": profile,
                     "result": "passed", "candidate": copy.deepcopy(candidate), "candidate_artifact_id": 987,
                     "policy_digest": ci.policy_identity(policy)} for profile in ("build", "asan")]
        jobs = {"build": "success", "asan": "success"}
        return candidate, policy, receipts, jobs

    def aggregate(self, candidate, policy, receipts, jobs, artifact_id=987):
        return ci.aggregate_qualification(catalog(), candidate, receipts, jobs,
                                          candidate_artifact_id=artifact_id, policy=policy)

    def lock(self):
        return {"schema_version": 1, "kind": "production-lock", "families": {
            FAMILY: {"image": REGISTRY + "@" + digest(b"manifest"),
                     "platform": "linux/amd64", "config_digest": digest(b"config"),
                     "recipe_digest": digest(b"recipe"), "input_digest": digest(b"inputs"),
                     "qualification": {"artifact_id": 999, "digest": digest(b"evidence"),
                                       "receipt": "ci/environments/evidence/" + FAMILY + "/verification.json",
                                       "image_bundle": "ci/environments/evidence/" + FAMILY + "/image.bundle.json",
                                       "receipt_bundle": "ci/environments/evidence/" + FAMILY + "/receipt.bundle.json"},
                     "source": source()}}}

    def test_canonical_content_identity_is_order_independent_and_domain_separated(self):
        self.assertEqual(ci.canonical_digest({"b": 2, "a": 1}), digest(b'{"a":1,"b":2}'))
        self.assertEqual(ci.canonical_digest({"b": 2, "a": 1}), ci.canonical_digest({"a": 1, "b": 2}))
        manifest = {"schema_version": 1, "base": "ubuntu@" + digest(b"resolved")}
        self.assertNotEqual(ci.input_identity(manifest), ci.policy_identity(manifest))

    def test_json_rejects_duplicate_keys_and_nonfinite_values(self):
        path = self.root / "document.json"
        for text in ('{"schema_version":1,"schema_version":2}', '{"nested":{"a":1,"a":2}}', '{"a":NaN}', '{"a":1e9999}'):
            with self.subTest(text=text):
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ci.ContractError):
                    ci.load_json(path)
        with self.assertRaises(ci.ContractError):
            ci.canonical_digest({"bad": float("nan")})

    def test_recipe_identity_ignores_application_lock_policy_and_source_order(self):
        original = ci.recipe_identity(catalog(), FAMILY, self.root)
        self.assertRegex(original, r"^sha256:[0-9a-f]{64}$")
        (self.root / "application.cpp").write_text("changed app", encoding="utf-8")
        (self.root / "production-lock.json").write_text("changed lock", encoding="utf-8")
        other = catalog()
        other["families"][FAMILY]["profiles"] = ["other-profile"]
        other["families"][FAMILY]["recipe_files"].reverse()
        self.assertEqual(original, ci.recipe_identity(other, FAMILY, self.root))
        (self.root / "docker/install.sh").write_text("changed recipe", encoding="utf-8")
        self.assertNotEqual(original, ci.recipe_identity(catalog(), FAMILY, self.root))

    def test_identity_domains_change_only_with_their_declared_materials(self):
        original = ci.recipe_identity(catalog(), FAMILY, self.root)
        for field, value in (("context", "."), ("build_args", {"TOOLCHAIN": "gcc-13"})):
            other = catalog()
            other["families"][FAMILY][field] = value
            self.assertNotEqual(original, ci.recipe_identity(other, FAMILY, self.root))
        first = {"schema_version": 1, "resolved_base": "ubuntu@" + digest(b"first")}
        second = dict(first, resolved_base="ubuntu@" + digest(b"second"))
        self.assertNotEqual(ci.input_identity(first), ci.input_identity(second))
        for invalid in ({"schema_version": 1}, {"schema_version": True, "inputs": []}, {"schema_version": 2, "inputs": []}):
            with self.subTest(invalid=invalid), self.assertRaises(ci.ContractError):
                ci.input_identity(invalid)

    def test_recipe_rejects_escaping_symlink_and_missing_dockerfile(self):
        for relative in ("../escape", "/absolute", "docker\\bad", "docker/../application.cpp"):
            other = catalog()
            other["families"][FAMILY]["recipe_files"] = ["docker/Dockerfile", relative]
            with self.subTest(relative=relative), self.assertRaises(ci.ContractError):
                ci.recipe_identity(other, FAMILY, self.root)
        (self.root / "docker/install.sh").unlink()
        (self.root / "docker/install.sh").symlink_to(self.root / "docker/Dockerfile")
        with self.assertRaises(ci.ContractError):
            ci.recipe_identity(catalog(), FAMILY, self.root)
        other = catalog()
        other["families"][FAMILY]["recipe_files"] = ["docker/install.sh"]
        with self.assertRaises(ci.ContractError):
            ci.recipe_identity(other, FAMILY, self.root)

    def test_inspection_binds_archive_manifest_config_and_layers(self):
        for compression in ("gzip", "zstd", "none"):
            with self.subTest(compression=compression):
                path, expected = self.archive(compression=compression)
                self.assertEqual(ci.inspect_oci_archive(path), expected)

    def test_archive_authority_changes_when_member_order_changes(self):
        path, _ = self.archive()
        original = ci.inspect_oci_archive(path)
        path, _ = self.archive(extra=lambda members: members.reverse())
        reordered = ci.inspect_oci_archive(path)
        self.assertNotEqual(original.pop("archive_sha256"), reordered.pop("archive_sha256"))
        self.assertEqual(original, reordered)

    def test_inspection_allows_layout_directories_but_not_unreferenced_blobs(self):
        def directories(members):
            for name in ("blobs/", "blobs/sha256/"):
                directory = tarfile.TarInfo(name)
                directory.type = tarfile.DIRTYPE
                members.insert(0, (directory, b""))
        path, expected = self.archive(extra=directories)
        self.assertEqual(ci.inspect_oci_archive(path), expected)
        path, _ = self.archive(extra=lambda members: members.append(("blobs/sha256/" + digest(b"extra")[7:], b"extra")))
        with self.assertRaises(ci.ContractError):
            ci.inspect_oci_archive(path)

    def test_index_descriptor_size_digest_and_platform_are_bound(self):
        changes = [{"size": 0}, {"digest": digest(b"other")}, {"size": True},
                   {"platform": {"os": "linux", "architecture": "arm64"}},
                   {"platform": {"os": "linux", "architecture": "amd64", "variant": "v3"}}]
        for change in changes:
            def change_index(members):
                index = json.loads(members[1][1])
                index["manifests"][0].update(change)
                members[1] = ("index.json", encoded(index))
            path, _ = self.archive(extra=change_index)
            with self.subTest(change=change), self.assertRaises(ci.ContractError):
                ci.inspect_oci_archive(path)

    def test_inspection_rejects_wrong_platform_rootfs_and_multiple_manifests(self):
        mutations = [
            lambda c, m, i: c.update(architecture="arm64"),
            lambda c, m, i: c.update(os="windows"),
            lambda c, m, i: c["rootfs"].update(type="unknown"),
            lambda c, m, i: c["rootfs"].update(diff_ids=[]),
            lambda c, m, i: i["manifests"].append({}),
            lambda c, m, i: m.update(mediaType="application/vnd.docker.distribution.manifest.v2+json"),
            lambda c, m, i: m["layers"][0].update(mediaType="application/octet-stream"),
            lambda c, m, i: m["layers"][0].update(size=100000),
            lambda c, m, i: m["layers"][0].update(digest=digest(b"wrong")),
            lambda c, m, i: m["layers"][0].update(urls=["https://example.invalid/layer"]),
        ]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                path, _ = self.archive(mutate=mutate)
                with self.assertRaises(ci.ContractError):
                    ci.inspect_oci_archive(path)

    def test_inspection_rejects_unsafe_duplicate_and_special_members(self):
        for name in ("../escape", "/absolute", "blobs\\escape", "blobs/../escape", "./index.json", "index.json"):
            with self.subTest(name=name):
                path, _ = self.archive(extra=lambda members: members.append((name, b"{}")))
                with self.assertRaises(ci.ContractError):
                    ci.inspect_oci_archive(path)
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE, tarfile.CHRTYPE, tarfile.GNUTYPE_LONGNAME, tarfile.XHDTYPE):
            with self.subTest(kind=kind):
                info = tarfile.TarInfo("special")
                info.type = kind
                info.linkname = "index.json"
                path, _ = self.archive(extra=lambda members: members.append((info, b"")))
                with self.assertRaises(ci.ContractError):
                    ci.inspect_oci_archive(path)

    def test_inspection_rejects_tamper_duplicate_json_oversize_and_outer_compression(self):
        mutations = [
            lambda members: members.__setitem__(-1, (members[-1][0], b"tampered")),
            lambda members: members.__setitem__(1, ("index.json", b'{"schemaVersion":2,"schemaVersion":2,"manifests":[]}')),
            lambda members: members.__setitem__(1, ("index.json", b" " * (4 * 1024 * 1024 + 1))),
        ]
        for mutate in mutations:
            path, _ = self.archive(extra=mutate)
            with self.assertRaises(ci.ContractError):
                ci.inspect_oci_archive(path)
        path, _ = self.archive(mode="w:gz")
        with self.assertRaises(ci.ContractError):
            ci.inspect_oci_archive(path)

    def test_inspection_rejects_hidden_appended_tar_and_truncated_payload(self):
        path, _ = self.archive()
        original = path.read_bytes()
        path.write_bytes(original + original)
        with self.assertRaises(ci.ContractError):
            ci.inspect_oci_archive(path)
        path.write_bytes(original[:2000])
        with self.assertRaises(ci.ContractError):
            ci.inspect_oci_archive(path)

    def test_malformed_descriptor_types_fail_with_contract_error(self):
        for value in ([], {}, None, 12):
            with self.subTest(value=value):
                path, _ = self.archive(mutate=lambda c, m, i: m["layers"][0].update(mediaType=value))
                with self.assertRaises(ci.ContractError):
                    ci.inspect_oci_archive(path)

    def test_loaded_image_must_match_config_and_all_diff_ids(self):
        _, image = self.archive()
        loaded = {"Id": image["config_digest"], "Architecture": "amd64", "Os": "linux",
                  "RootFS": {"Type": "layers", "Layers": image["diff_ids"]}}
        self.assertIsNone(ci.compare_loaded_image(image, loaded))
        for field, value in (("Id", digest(b"wrong")), ("Architecture", "arm64"), ("RootFS", {"Type": "layers", "Layers": []})):
            other = copy.deepcopy(loaded)
            other[field] = value
            with self.subTest(field=field), self.assertRaises(ci.ContractError):
                ci.compare_loaded_image(image, other)

    def test_qualification_requires_complete_successful_authoritative_results(self):
        candidate, policy, receipts, jobs = self.qualifications()
        result = self.aggregate(candidate, policy, receipts, jobs)
        self.assertEqual(result["kind"], "qualification")
        self.assertEqual(result["profiles"], ["asan", "build"])
        self.assertEqual(result["candidate_artifact_id"], 987)
        self.assertEqual(result["candidate"], candidate)
        self.assertEqual(result, self.aggregate(candidate, policy, list(reversed(receipts)), jobs))
        for bad_jobs in ({"build": "success"}, {"build": "success", "asan": "skipped"},
                         {"build": "failure", "asan": "success"}, dict(jobs, extra="success")):
            with self.subTest(jobs=bad_jobs), self.assertRaises(ci.ContractError):
                self.aggregate(candidate, policy, receipts, bad_jobs)
        for bad_receipts in (receipts[:1], receipts + receipts[:1]):
            with self.assertRaises(ci.ContractError):
                self.aggregate(candidate, policy, bad_receipts, jobs)

    def test_qualification_rejects_cross_run_image_input_recipe_policy_and_artifact_substitution(self):
        candidate, policy, receipts, jobs = self.qualifications()
        mutations = [
            lambda r: r["candidate"]["source"].update(run_id=124),
            lambda r: r["candidate"]["source"].update(run_attempt=1),
            lambda r: r["candidate"]["source"].update(sha="b" * 40),
            lambda r: r["candidate"]["source"].update(ref="refs/heads/other"),
            lambda r: r["candidate"].update(recipe_digest=digest(b"other")),
            lambda r: r["candidate"].update(input_digest=digest(b"other")),
            lambda r: r["candidate"]["image"].update(config_digest=digest(b"other")),
            lambda r: r.update(candidate_artifact_id=988),
            lambda r: r.update(policy_digest=digest(b"other")),
            lambda r: r.update(result="skipped"),
        ]
        for mutate in mutations:
            other = copy.deepcopy(receipts)
            mutate(other[0])
            with self.subTest(mutate=mutate), self.assertRaises(ci.ContractError):
                self.aggregate(candidate, policy, other, jobs)
        with self.assertRaises(ci.ContractError):
            self.aggregate(candidate, policy, receipts, jobs, artifact_id=988)

    def test_policy_must_name_candidate_family_and_exact_configured_profiles(self):
        candidate, policy, receipts, jobs = self.qualifications()
        mutations = [lambda p: p.update(family="noble-qt6"),
                     lambda p: p["profiles"].pop("asan"),
                     lambda p: p["profiles"].update(extra={}),
                     lambda p: p.update(profiles=[])]
        for mutate in mutations:
            other = copy.deepcopy(policy)
            mutate(other)
            for receipt in receipts:
                receipt["policy_digest"] = ci.policy_identity(other)
            with self.subTest(mutate=mutate), self.assertRaises(ci.ContractError):
                self.aggregate(candidate, other, receipts, jobs)

    def test_candidate_has_no_embedded_upload_id_or_placeholder_source(self):
        candidate, policy, receipts, jobs = self.qualifications()
        mutations = [lambda c: c.update(candidate_artifact_id=987),
                     lambda c: c["source"].update(sha="0" * 40),
                     lambda c: c["source"].update(repository="other/repo"),
                     lambda c: c["source"].update(run_attempt=True)]
        for mutate in mutations:
            other = copy.deepcopy(candidate)
            mutate(other)
            other_receipts = copy.deepcopy(receipts)
            for receipt in other_receipts:
                receipt["candidate"] = other
            with self.subTest(mutate=mutate), self.assertRaises(ci.ContractError):
                self.aggregate(other, policy, other_receipts, jobs)

    def test_production_lock_is_complete_and_distinct_from_candidate(self):
        self.assertIsNone(ci.validate_production_lock(self.lock(), catalog()))
        mutations = [
            lambda lock: lock.update(kind="candidate"),
            lambda lock: lock["families"].clear(),
            lambda lock: lock["families"][FAMILY].update(image=REGISTRY + ":latest"),
            lambda lock: lock["families"][FAMILY].update(image=REGISTRY + ":tag@" + digest(b"x")),
            lambda lock: lock["families"][FAMILY].update(image=REGISTRY + "@sha256:" + "0" * 64),
            lambda lock: lock["families"][FAMILY].update(image=REGISTRY + "/" + FAMILY + "@" + digest(b"x")),
            lambda lock: lock["families"][FAMILY].update(config_digest=""),
            lambda lock: lock["families"][FAMILY].update(platform="linux/arm64"),
            lambda lock: lock["families"][FAMILY].update(state="candidate"),
            lambda lock: lock["families"][FAMILY]["qualification"].update(artifact_id=0),
        ]
        for mutate in mutations:
            lock = self.lock()
            mutate(lock)
            with self.subTest(mutate=mutate), self.assertRaises(ci.ContractError):
                ci.validate_production_lock(lock, catalog())

    def test_checked_in_evidence_is_family_local_and_artifact_id_is_optional(self):
        lock = self.lock()
        evidence = lock["families"][FAMILY]["qualification"]
        del evidence["artifact_id"]
        self.assertIsNone(ci.validate_production_lock(lock, catalog()))
        for path in ("../escape", "/absolute", "ci/environments/evidence/other/receipt.json",
                     "ci/environments/evidence/" + FAMILY + "/../receipt.json",
                     "ci/environments/evidence/" + FAMILY + "/bad\\receipt.json",
                     evidence["image_bundle"]):
            other = copy.deepcopy(lock)
            other["families"][FAMILY]["qualification"]["receipt"] = path
            with self.subTest(path=path), self.assertRaises(ci.ContractError):
                ci.validate_production_lock(other, catalog())

    def test_cli_identity_inspection_loaded_comparison_and_qualification(self):
        def document(name, value):
            path = self.root / name
            path.write_bytes(encoded(value))
            return str(path)
        def cli(*args):
            result = subprocess.run([sys.executable, str(SCRIPT)] + list(args), capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        candidate, policy, receipts, jobs = self.qualifications()
        catalog_path = document("catalog.json", catalog())
        policy_path = document("policy.json", policy)
        identities = cli("identity", "--catalog", catalog_path, "--family", FAMILY, "--repo-root", str(self.root),
                         "--inputs", document("inputs.json", {"schema_version": 1, "base": "ubuntu@" + digest(b"base")}),
                         "--policy", policy_path)
        self.assertEqual(identities["recipe_digest"], ci.recipe_identity(catalog(), FAMILY, self.root))
        path, image = self.archive()
        self.assertEqual(cli("inspect-oci", "--archive", str(path)), image)
        loaded = [{"Id": image["config_digest"], "Architecture": "amd64", "Os": "linux",
                   "RootFS": {"Type": "layers", "Layers": image["diff_ids"]}}]
        self.assertTrue(cli("compare-loaded", "--image", document("image.json", image),
                            "--docker-inspect", document("docker-inspect.json", loaded))["loaded_image_matches"])
        result = cli("qualify", "--catalog", catalog_path, "--candidate", document("candidate.json", candidate),
                     "--receipts", document("receipts.json", receipts), "--job-results", document("jobs.json", jobs),
                     "--candidate-artifact-id", "987", "--policy", policy_path)
        self.assertEqual(result, self.aggregate(candidate, policy, receipts, jobs))

    def test_offline_check_cli_labels_schema_not_provenance(self):
        catalog_path = self.root / "catalog.json"
        lock_path = self.root / "lock.json"
        catalog_path.write_bytes(encoded(catalog()))
        lock_path.write_bytes(encoded(self.lock()))
        result = subprocess.run([sys.executable, str(SCRIPT), "offline-check", "--catalog", str(catalog_path), "--lock", str(lock_path)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("not cryptographic provenance", result.stdout)
        lock_path.write_text('{"schema_version":1,"schema_version":1}', encoding="utf-8")
        result = subprocess.run([sys.executable, str(SCRIPT), "offline-check", "--catalog", str(catalog_path), "--lock", str(lock_path)],
                                capture_output=True, text=True, check=False)
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
