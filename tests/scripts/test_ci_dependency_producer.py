"""Offline candidates must derive binary-only cores from qualified artifacts."""

import hashlib
import json
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_dependency_catalog as catalog
import ci_dependency_producer as producer


class DependencyProducerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.artifact = self.root / "full-artifact"
        self.artifact.mkdir()
        self.legal = self.root / "legal-assets"
        self.legal.mkdir()
        self.archive = self.root / "adb-helper-linux-x86_64.tar.gz"
        self.candidate = self.root / "candidate.json"
        self.verifier = mock.Mock(side_effect=self._fake_verifier)
        self.write("helpers/adb", b"real adb", 0o755)
        self.write("helpers/libusb-1.0.so.0", b"real libusb")
        self.write("receipt.json", b"qualification 1")
        self.write("smoke.json", b"smoke 1")
        self.write("package-smoke.json", b"package smoke 1")
        self.verification = {
            "schema_version": 1, "receipt_kind": "package-verification",
            "target": "linux-x86_64", "layout": "package",
            "source_set_receipt_sha256": "a" * 64,
            "source_helper_sha256": "b" * 64, "helper_sha256": "c" * 64,
            "package_sha256": None, "packages": [],
            "verified_receipts": ["binary-build", "binary-smoke", "package-verification"],
            "required_receipts": [], "release_qualified": False,
        }
        self.write("package-verification.json",
                   (json.dumps(self.verification, sort_keys=True) + "\n").encode())
        self.write("SHA256SUMS", b"envelope 1")

    def write(self, name, content, mode=0o644):
        path = self.artifact / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(mode)
        return path

    def produce(self, target_id="adb-linux-x86_64"):
        return producer.produce_candidate(
            repo_root=ROOT, target_id=target_id, artifact_root=self.artifact,
            source_assets_root=self.legal, archive=self.archive,
            candidate_receipt=self.candidate, run_verifier=self.verifier,
        )

    def _fake_verifier(self, argv, **kwargs):
        if "--package-verification-receipt" in argv:
            path = pathlib.Path(argv[argv.index("--package-verification-receipt") + 1])
            path.write_bytes((self.artifact / "package-verification.json").read_bytes())
        return subprocess.CompletedProcess(argv, 0)

    def stage_ios(self, toolchain=None):
        for path in self.artifact.rglob("*"):
            if path.is_file():
                path.unlink()
        (self.artifact / "helpers").rmdir()
        self.archive = self.root / "ios-native-arm64.tar.gz"
        self.write("lib/libfoo.1.dylib", b"dylib", 0o755)
        (self.artifact / "lib/libfoo.dylib").symlink_to("libfoo.1.dylib")
        self.write("ios-native-build-receipt.json", json.dumps({
            "toolchain": catalog.IOS_TOOLCHAIN if toolchain is None else toolchain,
        }).encode())
        for name in ("ios-native-source-set-receipt.json", "ios-native-legal-receipt.json",
                     "ios-native-sbom.spdx.json", "NOTICE-ios-native.txt"):
            (self.legal / name).write_text("external qualification material\n")

    def test_adb_candidate_stages_exact_binary_closure_and_requalification_preserves_blob(self):
        first = self.produce()
        document = json.loads(self.candidate.read_text())
        self.assertEqual(document, first)
        self.assertEqual(first["publication_status"], "candidate-only")
        self.assertEqual(first["target"], "linux-x86_64")
        self.assertEqual(first["component"], "adb-helper")
        self.assertEqual(len(first["core_identity"]), 64)
        self.assertEqual(len(first["policy_identity"]), 64)
        self.assertEqual(first["archive"]["sha256"], hashlib.sha256(self.archive.read_bytes()).hexdigest())
        self.assertEqual(first["archive"]["size"], self.archive.stat().st_size)
        self.assertFalse((ROOT / "ci/dependencies/lock.json").exists())
        self.assertNotIn("https://", self.candidate.read_text())
        self.assertNotIn("lock_sha256", self.candidate.read_text())
        with tarfile.open(self.archive, "r:gz") as tar:
            self.assertEqual({entry.name for entry in tar}, {
                ".ci-dependency-core.json", "helpers/adb", "helpers/libusb-1.0.so.0",
            })
        self.write("receipt.json", b"qualification 2")
        self.write("smoke.json", b"smoke 2")
        original = self.archive.read_bytes()
        self.archive.unlink()
        self.candidate.unlink()
        second = self.produce()
        self.assertEqual(second["archive"], first["archive"])
        self.assertEqual(self.archive.read_bytes(), original)
        self.assertEqual(self.verifier.call_count, 6)
        envelope_args = self.verifier.call_args_list[-3][0][0]
        args = self.verifier.call_args_list[-2][0][0]
        final_envelope = self.verifier.call_args_list[-1][0][0]
        self.assertEqual(envelope_args, final_envelope)
        self.assertEqual(pathlib.Path(envelope_args[1]).name, "verify_adb_helper_envelope.py")
        self.assertEqual(pathlib.Path(args[1]).name, "verify_adb_helper_artifact.py")
        self.assertIn("--asset-scope", args)
        self.assertIn("--binary-smoke-receipt", args)
        self.assertEqual(args[args.index("--binary-smoke-receipt") + 1],
                         str(self.artifact / "package-smoke.json"))
        self.assertIn("--package-root", args)
        self.assertIn("--package-verification-receipt", args)
        self.assertIn("--source-assets-root", args)
        self.assertIn("--expected-target", args)
        self.assertIn("--require-lock-binding", args)

    def test_verifier_failure_prevents_any_archive_or_candidate(self):
        self.verifier.side_effect = lambda argv, **kwargs: subprocess.CompletedProcess(argv, 1)
        with self.assertRaises(producer.ProducerError):
            self.produce()
        self.assertFalse(self.archive.exists())
        self.assertFalse(self.candidate.exists())
        self.assertEqual(self.verifier.call_count, 1)

    def test_requalification_must_reproduce_original_package_receipt_without_rewriting_it(self):
        stored = (self.artifact / "package-verification.json").read_bytes()

        def changed_receipt(argv, **kwargs):
            if "--package-verification-receipt" in argv:
                path = pathlib.Path(argv[argv.index("--package-verification-receipt") + 1])
                path.write_text("different qualification evidence")
            return subprocess.CompletedProcess(argv, 0)

        self.verifier.side_effect = changed_receipt
        with self.assertRaises(producer.ProducerError):
            self.produce()
        self.assertEqual((self.artifact / "package-verification.json").read_bytes(), stored)
        self.assertFalse(self.archive.exists())
        self.assertFalse(self.candidate.exists())

    def test_verifier_only_requalification_keeps_original_receipt_and_binary(self):
        stored = (self.artifact / "package-verification.json").read_bytes()
        revised = dict(self.verification, verified_receipts=[
            "binary-build", "binary-smoke", "package-verification", "new-policy-check"
        ])

        def requalified(argv, **kwargs):
            if "--package-verification-receipt" in argv:
                path = pathlib.Path(argv[argv.index("--package-verification-receipt") + 1])
                path.write_text(json.dumps(revised, sort_keys=True) + "\n")
            return subprocess.CompletedProcess(argv, 0)

        self.verifier.side_effect = requalified
        result = self.produce()
        self.assertEqual((self.artifact / "package-verification.json").read_bytes(), stored)
        self.assertNotEqual(result["qualification"]["legacy_package_receipt_sha256"],
                            result["qualification"]["rechecked_package_receipt_sha256"])
        self.assertTrue(self.archive.exists())

    def test_requalification_rejects_changed_binary_or_source_binding(self):
        for field, value in (("helper_sha256", "d" * 64),
                             ("source_set_receipt_sha256", "e" * 64),
                             ("target", "linux-arm64")):
            with self.subTest(field=field):
                altered = dict(self.verification, **{field: value})

                def rechecked(argv, **kwargs):
                    if "--package-verification-receipt" in argv:
                        path = pathlib.Path(argv[argv.index("--package-verification-receipt") + 1])
                        path.write_text(json.dumps(altered, sort_keys=True) + "\n")
                    return subprocess.CompletedProcess(argv, 0)

                self.verifier.side_effect = rechecked
                with self.assertRaises(producer.ProducerError):
                    self.produce()
                self.assertFalse(self.archive.exists())
                self.assertFalse(self.candidate.exists())

    def test_missing_runtime_or_injected_symlink_fails_closed(self):
        (self.artifact / "helpers/libusb-1.0.so.0").unlink()
        with self.assertRaises(producer.ProducerError):
            self.produce()
        self.assertFalse(self.archive.exists())
        (self.artifact / "helpers/libusb-1.0.so.0").symlink_to("../../escape")
        with self.assertRaises(producer.ProducerError):
            self.produce()
        self.assertFalse(self.archive.exists())

    def test_catalog_archive_name_and_existing_output_are_enforced(self):
        self.archive = self.root / "wrong-target.tar.gz"
        with self.assertRaises(producer.ProducerError):
            self.produce()
        self.assertFalse(self.candidate.exists())
        self.archive = self.root / "adb-helper-linux-x86_64.tar.gz"
        self.candidate.write_text("existing receipt")
        with self.assertRaises(producer.ProducerError):
            self.produce()
        self.assertFalse(self.archive.exists())

    def test_candidate_outputs_cannot_change_qualified_inputs(self):
        self.archive = self.artifact / "adb-helper-linux-x86_64.tar.gz"
        with self.assertRaises(producer.ProducerError):
            self.produce()
        self.assertFalse(self.archive.exists())
        self.archive = self.root / "adb-helper-linux-x86_64.tar.gz"
        self.candidate = self.legal / "candidate.json"
        with self.assertRaises(producer.ProducerError):
            self.produce()
        self.assertFalse(self.candidate.exists())
        self.assertEqual(self.verifier.call_count, 0)

    def test_ios_candidate_requires_all_observed_pinned_toolchain_fields(self):
        self.stage_ios()
        first = self.produce(target_id="ios-arm64")
        self.assertEqual(first["target"], "arm64")
        self.assertEqual(first["archive"]["name"], "ios-native-arm64.tar.gz")
        with tarfile.open(self.archive, "r:gz") as tar:
            self.assertEqual({entry.name for entry in tar}, {
                ".ci-dependency-core.json", "lib/libfoo.1.dylib", "lib/libfoo.dylib",
            })
        args = self.verifier.call_args[0][0]
        for option in ("--asset-scope", "--source-receipt", "--legal-receipt", "--sbom"):
            self.assertIn(option, args)
        for changed in (
            {key: value for key, value in catalog.IOS_TOOLCHAIN.items() if key != "ninja"},
            {**catalog.IOS_TOOLCHAIN, "ninja": "1.12.0"},
            {**catalog.IOS_TOOLCHAIN, "sdk_version": "16.0"},
            {**catalog.IOS_TOOLCHAIN, "xcode": ["Xcode 16.5", "Build version unknown"]},
        ):
            self.archive.unlink(missing_ok=True)
            self.candidate.unlink(missing_ok=True)
            self.write("ios-native-build-receipt.json", json.dumps({"toolchain": changed}).encode())
            with self.subTest(toolchain=changed), self.assertRaises(producer.ProducerError):
                self.produce(target_id="ios-arm64")
            self.assertFalse(self.archive.exists())

    def test_ios_qualification_changes_do_not_change_tar_bytes(self):
        self.stage_ios()
        first = self.produce(target_id="ios-arm64")
        raw = self.archive.read_bytes()
        self.archive.unlink()
        self.candidate.unlink()
        (self.legal / "ios-native-legal-receipt.json").write_text("new legal policy\n")
        self.assertEqual(self.produce(target_id="ios-arm64")["archive"], first["archive"])
        self.assertEqual(self.archive.read_bytes(), raw)


if __name__ == "__main__":
    unittest.main()
