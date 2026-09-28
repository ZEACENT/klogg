"""Binary-core keys must not include application overlays or verifier-only policy."""

import copy
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_dependency_catalog as catalog
import ci_dependency_identity as identity


class DependencyIdentityTest(unittest.TestCase):
    def setUp(self):
        self.adb = json.loads(
            (ROOT / "packaging/adb/adb-helper.lock.json").read_text(encoding="utf-8")
        )
        self.ios = json.loads(
            (ROOT / "3rdparty/libimobiledevice/libimobiledevice.lock.json").read_text(
                encoding="utf-8"
            )
        )

    def test_all_seven_targets_have_distinct_canonical_core_identities(self):
        identities = {
            identity.core_identity("adb-helper", target, self.adb, ROOT)
            for target in self.adb["targets"]
        }
        identities.update(
            identity.core_identity("ios-native", target, self.ios, ROOT)
            for target in ("x86_64", "arm64")
        )
        self.assertEqual(len(identities), 7)
        self.assertTrue(all(len(value) == 64 for value in identities))

    def test_adb_core_excludes_publication_metadata_but_tracks_build_inputs(self):
        base = identity.core_identity("adb-helper", "macos-arm64", self.adb, ROOT)
        overlay = copy.deepcopy(self.adb)
        overlay["release_assets"][4]["file_name"] = "new-source-offer.txt"
        overlay["install_paths"]["dmg"] = "new/package/path"
        overlay["package_targets"]["macos-arm64-dmg"]["qualification"] = {}
        overlay["release_policy"]["require_signing_for_release_qualification"] = False
        self.assertEqual(base, identity.core_identity("adb-helper", "macos-arm64", overlay, ROOT))

        for mutate in (
            lambda lock: lock["sources"][0].update(archive_sha256="0" * 64),
            lambda lock: lock["patches"][0].update(sha256="0" * 64),
            lambda lock: lock["toolchains"]["macos-arm64"].update(xcode="different"),
            lambda lock: lock["release_policy"].update(source_date_epoch=0),
        ):
            changed = copy.deepcopy(self.adb)
            mutate(changed)
            self.assertNotEqual(
                base, identity.core_identity("adb-helper", "macos-arm64", changed, ROOT)
            )

    def test_windows_msys2_tool_change_does_not_invalidate_linux_or_macos_cores(self):
        before = {
            target: identity.core_identity("adb-helper", target, self.adb, ROOT)
            for target in self.adb["targets"]
        }
        changed = copy.deepcopy(self.adb)
        changed["toolchain_packages"][0]["version"] = "2.7.7-1"
        after = {
            target: identity.core_identity("adb-helper", target, changed, ROOT)
            for target in changed["targets"]
        }
        for target in ("linux-x86_64", "linux-arm64", "macos-x86_64", "macos-arm64"):
            self.assertEqual(before[target], after[target], target)
        self.assertNotEqual(before["windows-x86_64"], after["windows-x86_64"])

    def test_ios_core_excludes_receipt_names_but_tracks_build_inputs(self):
        base = identity.core_identity("ios-native", "arm64", self.ios, ROOT)
        overlay = copy.deepcopy(self.ios)
        overlay["receipts"]["legal"] = "renamed-legal-receipt.json"
        overlay["release_policy"]["fail_closed_on_missing_native_stack"] = False
        self.assertEqual(base, identity.core_identity("ios-native", "arm64", overlay, ROOT))
        for mutate in (
            lambda lock: lock["sources"][0].update(archive_sha256="0" * 64),
            lambda lock: lock["patches"][0].update(sha256="0" * 64),
            lambda lock: lock["release_policy"].update(source_date_epoch=0),
        ):
            changed = copy.deepcopy(self.ios)
            mutate(changed)
            self.assertNotEqual(base, identity.core_identity("ios-native", "arm64", changed, ROOT))

    def test_ios_identity_requires_reviewed_producer_toolchain(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for file_name in identity.IOS_BUILD_FILES:
                target = root / file_name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("unchanged builder\n", encoding="utf-8")
            catalog_path = root / "ci/dependencies/catalog.json"
            catalog_path.parent.mkdir(parents=True)
            original = json.loads(
                (ROOT / "ci/dependencies/catalog.json").read_text(encoding="utf-8")
            )
            catalog_path.write_text(json.dumps(original), encoding="utf-8")
            expected = identity.core_identity("ios-native", "arm64", self.ios, root)
            self.assertEqual(len(expected), 64)
            for mutated in (
                lambda d: d["targets"]["ios-arm64"].pop("toolchain"),
                lambda d: d["targets"]["ios-arm64"]["toolchain"].update(
                    sdk_version="26.4"
                ),
            ):
                changed = copy.deepcopy(original)
                mutated(changed)
                catalog_path.write_text(json.dumps(changed), encoding="utf-8")
                with self.assertRaises(identity.DependencyIdentityError):
                    identity.core_identity("ios-native", "arm64", self.ios, root)

    def test_reviewed_ios_runner_change_invalidates_core(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for file_name in identity.IOS_BUILD_FILES:
                path = root / file_name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("builder\n", encoding="utf-8")
            catalog_path = root / "ci/dependencies/catalog.json"
            catalog_path.parent.mkdir(parents=True)
            document = json.loads(
                (ROOT / "ci/dependencies/catalog.json").read_text(encoding="utf-8")
            )
            catalog_path.write_text(json.dumps(document), encoding="utf-8")
            before = identity.core_identity("ios-native", "arm64", self.ios, root)
            document["targets"]["ios-arm64"]["runner"] = "macos-16"
            catalog_path.write_text(json.dumps(document), encoding="utf-8")
            with mock.patch.dict(catalog.TARGETS, {
                "ios-arm64": ("ios-native", "arm64", "macos-16")
            }):
                after = identity.core_identity("ios-native", "arm64", self.ios, root)
            self.assertNotEqual(before, after)

    def test_shared_gate_changes_requalify_both_cores_without_rebuilding(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            files = set(identity.ADB_BUILD_FILES + identity.IOS_BUILD_FILES
                        + identity.ADB_POLICY_FILES + identity.IOS_POLICY_FILES)
            for file_name in files | {"scripts/ci_dependency_gate.py"}:
                path = root / file_name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("original\n", encoding="utf-8")
            catalog_path = root / "ci/dependencies/catalog.json"
            catalog_path.parent.mkdir(parents=True)
            catalog_path.write_bytes((ROOT / "ci/dependencies/catalog.json").read_bytes())
            before = {
                "adb-core": identity.core_identity("adb-helper", "macos-arm64", self.adb, root),
                "adb-policy": identity.policy_identity("adb-helper", root, lock=self.adb),
                "ios-core": identity.core_identity("ios-native", "arm64", self.ios, root),
                "ios-policy": identity.policy_identity("ios-native", root, lock=self.ios),
            }
            (root / "scripts/ci_dependency_gate.py").write_text("changed qualification gate\n", encoding="utf-8")
            self.assertEqual(identity.core_identity("adb-helper", "macos-arm64", self.adb, root), before["adb-core"])
            self.assertEqual(identity.core_identity("ios-native", "arm64", self.ios, root), before["ios-core"])
            self.assertNotEqual(identity.policy_identity("adb-helper", root, lock=self.adb), before["adb-policy"])
            self.assertNotEqual(identity.policy_identity("ios-native", root, lock=self.ios), before["ios-policy"])

    def test_verifier_only_changes_requalify_without_rebuilding(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for file_name in identity.ADB_BUILD_FILES + identity.ADB_POLICY_FILES:
                target = root / file_name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("original\n", encoding="utf-8")
            core = identity.core_identity("adb-helper", "linux-x86_64", self.adb, root)
            policy = identity.policy_identity("adb-helper", root, lock=self.adb)
            verifier = root / identity.ADB_POLICY_FILES[0]
            verifier.write_text("changed verifier\n", encoding="utf-8")
            self.assertEqual(core, identity.core_identity("adb-helper", "linux-x86_64", self.adb, root))
            self.assertNotEqual(policy, identity.policy_identity("adb-helper", root, lock=self.adb))
            builder = root / identity.ADB_BUILD_FILES[0]
            builder.write_text("changed builder\n", encoding="utf-8")
            self.assertNotEqual(core, identity.core_identity("adb-helper", "linux-x86_64", self.adb, root))

    def test_lock_qualification_policy_changes_requalify_without_rebuilding(self):
        before_core = identity.core_identity("adb-helper", "linux-x86_64", self.adb, ROOT)
        before_policy = identity.policy_identity("adb-helper", ROOT, lock=self.adb)
        changed = copy.deepcopy(self.adb)
        changed["release_policy"]["require_signing_for_release_qualification"] = (
            not changed["release_policy"]["require_signing_for_release_qualification"]
        )
        self.assertEqual(before_core,
                         identity.core_identity("adb-helper", "linux-x86_64", changed, ROOT))
        self.assertNotEqual(before_policy,
                            identity.policy_identity("adb-helper", ROOT, lock=changed))
        ios_before = identity.policy_identity("ios-native", ROOT, lock=self.ios)
        changed_ios = copy.deepcopy(self.ios)
        changed_ios["artifact_contract"]["allowed_dylib_rpaths"] = ["@loader_path", "@rpath"]
        self.assertEqual(identity.core_identity("ios-native", "arm64", self.ios, ROOT),
                         identity.core_identity("ios-native", "arm64", changed_ios, ROOT))
        self.assertNotEqual(ios_before,
                            identity.policy_identity("ios-native", ROOT, lock=changed_ios))

    def test_policy_rejects_untracked_rules_on_other_targets(self):
        altered = copy.deepcopy(self.adb)
        altered["targets"]["linux-arm64"]["untracked_qualification_rule"] = True
        with self.assertRaises(identity.DependencyIdentityError):
            identity.policy_identity("adb-helper", ROOT, lock=altered)

    def test_unknown_lock_fields_and_unsupported_targets_fail_closed(self):
        unknown = copy.deepcopy(self.adb)
        unknown["compiler_flags"] = ["-DUNTRACKED"]
        with self.assertRaises(identity.DependencyIdentityError):
            identity.core_identity("adb-helper", "linux-x86_64", unknown, ROOT)
        with self.assertRaises(identity.DependencyIdentityError):
            identity.core_identity("adb-helper", "linux-s390x", self.adb, ROOT)
        with self.assertRaises(identity.DependencyIdentityError):
            identity.core_identity("ios-native", "windows-x86_64", self.ios, ROOT)
        untracked = copy.deepcopy(self.adb)
        untracked["targets"]["linux-x86_64"]["new_compiler_flags"] = ["-fchanged"]
        with self.assertRaises(identity.DependencyIdentityError):
            identity.core_identity("adb-helper", "linux-x86_64", untracked, ROOT)
        untracked_ios = copy.deepcopy(self.ios)
        untracked_ios["artifact_contract"]["new_compile_feature"] = True
        with self.assertRaises(identity.DependencyIdentityError):
            identity.core_identity("ios-native", "arm64", untracked_ios, ROOT)
        for component, lock, target in (
            ("adb-helper", self.adb, "linux-x86_64"),
            ("ios-native", self.ios, "arm64"),
        ):
            added = copy.deepcopy(lock)
            added["release_policy"]["new_build_setting"] = True
            with self.assertRaises(identity.DependencyIdentityError):
                identity.core_identity(component, target, added, ROOT)


if __name__ == "__main__":
    unittest.main()
