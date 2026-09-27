"""Native dependency production locks reject missing targets and unsigned placeholders."""

import copy
import hashlib
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import ci_dependency_catalog as catalog
import ci_dependency_lock as dependency_lock


class DependencyLockTest(unittest.TestCase):
    def setUp(self):
        self.catalog = {
            "schema_version": 1,
            "kind": "native-dependency-targets",
            "registry": "ghcr.io/zeacent/klogg-ci-deps",
            "producer_workflow": ".github/workflows/ci-dependencies.yml",
            "targets": {
                name: {
                    "component": component, "target": target, "runner": runner,
                    "archive_name": f"{component}-{target}.tar.gz",
                    **({"toolchain": copy.deepcopy(catalog.IOS_TOOLCHAIN)}
                       if component == "ios-native" else {}),
                }
                for name, (component, target, runner) in catalog.TARGETS.items()
            },
        }
        self.lock = {
            "schema_version": 1, "kind": "production-lock",
            "registry": self.catalog["registry"],
            "targets": {
                name: self.record(name) for name in catalog.TARGETS
            },
        }

    def record(self, name):
        component, target, runner = catalog.TARGETS[name]
        root = f"ci/dependencies/evidence/{name}/"
        return {
            "manifest_digest": "sha256:" + hashlib.sha256(name.encode()).hexdigest(),
            "blob_digest": "sha256:" + hashlib.sha256((name + ":blob").encode()).hexdigest(),
            "blob_size": 1234,
            "archive_name": f"{component}-{target}.tar.gz",
            "core_identity": hashlib.sha256((name + ":core").encode()).hexdigest(),
            "policy_identity": "d" * 64,
            "source": {
                "repository": "ZEACENT/klogg",
                "workflow": self.catalog["producer_workflow"],
                "sha": "e" * 40,
                "ref": "refs/heads/worktree-master-ci-fail",
                "run_id": 123, "run_attempt": 1,
            },
            "qualification": {
                "digest": "sha256:" + hashlib.sha256((name + ":receipt").encode()).hexdigest(),
                "receipt": root + "dependency-verification.json",
                "manifest_bundle": root + "manifest.sigstore.json",
                "receipt_bundle": root + "receipt.sigstore.json",
            },
        }

    def test_all_seven_targets_have_nonplaceholder_signature_bundle_paths(self):
        self.assertEqual(
            dependency_lock.validate_production_lock(self.lock, self.catalog),
            self.lock["targets"],
        )
        self.assertFalse((ROOT / "ci/dependencies/lock.json").exists())

    def test_unknown_or_missing_target_and_mutable_metadata_are_rejected(self):
        for mutation in (
            lambda lock: lock["targets"].pop("ios-arm64"),
            lambda lock: lock["targets"].update(unreviewed=self.record("ios-arm64")),
            lambda lock: lock["targets"]["ios-arm64"].update(
                archive_name="ios-native-x86_64.tar.gz"),
            lambda lock: lock["targets"]["ios-arm64"].update(
                platform="linux/amd64"),
            lambda lock: lock.update(registry="ghcr.io/attacker/other"),
        ):
            document = copy.deepcopy(self.lock)
            mutation(document)
            with self.subTest(document=document), self.assertRaises(
                dependency_lock.DependencyLockError
            ):
                dependency_lock.validate_production_lock(document, self.catalog)

    def test_copying_a_published_identity_to_another_target_is_rejected(self):
        for field in ("manifest_digest", "blob_digest", "core_identity"):
            document = copy.deepcopy(self.lock)
            document["targets"]["adb-linux-arm64"][field] = (
                document["targets"]["adb-linux-x86_64"][field]
            )
            with self.subTest(field=field), self.assertRaises(
                dependency_lock.DependencyLockError
            ):
                dependency_lock.validate_production_lock(document, self.catalog)
        document = copy.deepcopy(self.lock)
        document["targets"]["adb-linux-arm64"]["qualification"]["digest"] = (
            document["targets"]["adb-linux-x86_64"]["qualification"]["digest"]
        )
        with self.assertRaises(dependency_lock.DependencyLockError):
            dependency_lock.validate_production_lock(document, self.catalog)

    def test_invalid_digest_size_source_and_evidence_location_are_rejected(self):
        changes = (
            ("manifest_digest", "latest"),
            ("blob_digest", "sha256:" + "0" * 64),
            ("blob_size", True),
            ("core_identity", "0" * 64),
            ("policy_identity", "sha256:" + "d" * 64),
        )
        for field, value in changes:
            document = copy.deepcopy(self.lock)
            document["targets"]["adb-linux-x86_64"][field] = value
            with self.subTest(field=field), self.assertRaises(
                dependency_lock.DependencyLockError
            ):
                dependency_lock.validate_production_lock(document, self.catalog)
        for field, value in (
            ("workflow", ".github/workflows/ci-environments.yml"),
            ("run_attempt", 0),
            ("ref", "refs/pull/77/merge"),
        ):
            document = copy.deepcopy(self.lock)
            document["targets"]["ios-arm64"]["source"][field] = value
            with self.subTest(source_field=field), self.assertRaises(
                dependency_lock.DependencyLockError
            ):
                dependency_lock.validate_production_lock(document, self.catalog)
        for path in ("../verification.json", "ci/dependencies/evidence/ios-arm64/verification.json",
                     "/tmp/verification.json", "ci/dependencies/evidence/adb-linux-x86_64/../../other"):
            document = copy.deepcopy(self.lock)
            document["targets"]["adb-linux-x86_64"]["qualification"]["receipt"] = path
            with self.subTest(path=path), self.assertRaises(
                dependency_lock.DependencyLockError
            ):
                dependency_lock.validate_production_lock(document, self.catalog)


if __name__ == "__main__":
    unittest.main()
