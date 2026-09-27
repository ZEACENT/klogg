"""The seven native targets have explicit, nonfloating qualification policy."""

import json
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_dependency_catalog as catalog


class DependencyCatalogTest(unittest.TestCase):
    def setUp(self):
        self.path = ROOT / "ci/dependencies/catalog.json"
        self.document = json.loads(self.path.read_text(encoding="utf-8"))

    def test_real_catalog_covers_exact_seven_targets_and_pinned_runners(self):
        records = catalog.validate_catalog(self.document)
        self.assertEqual(len(records), 7)
        self.assertEqual(
            {row["archive_name"] for row in records.values()},
            {f"{row['component']}-{row['target']}.tar.gz" for row in records.values()},
        )
        self.assertEqual({row["component"] for row in records.values()},
                         {"adb-helper", "ios-native"})
        self.assertEqual({row["runner"] for row in records.values()},
                         {"ubuntu-24.04", "ubuntu-24.04-arm", "windows-2022",
                          "macos-15-intel", "macos-15"})
        for name in ("ios-arm64", "ios-x86_64"):
            self.assertEqual(records[name]["toolchain"], {
                "xcode": ["Xcode 16.4", "Build version 16F6"],
                "sdk_version": "15.5",
                "sdk_path": "/Applications/Xcode_16.4.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk",
                "clang": "Apple clang version 17.0.0 (clang-1700.0.13.5)",
                "cmake": "cmake version 3.31.6",
                "ninja": "1.12.1",
            })

    def test_catalog_rejects_unsupported_targets_and_mutable_runner_versions(self):
        for mutation in (
            lambda d: d["targets"].pop("adb-linux-arm64"),
            lambda d: d["targets"]["adb-linux-arm64"].update(runner="ubuntu-latest"),
            lambda d: d["targets"]["ios-arm64"].update(archive_name="ios-native-arm64:latest"),
            lambda d: d.update(registry="ghcr.io/attacker/deps"),
            lambda d: d["targets"]["ios-arm64"]["toolchain"].update(sdk_version="16.0"),
            lambda d: d["targets"]["ios-arm64"]["toolchain"].update(sdk_path="/tmp/unreviewed.sdk"),
        ):
            document = json.loads(self.path.read_text(encoding="utf-8"))
            mutation(document)
            with self.subTest(document=document), self.assertRaises(catalog.DependencyCatalogError):
                catalog.validate_catalog(document)

    def test_consumer_lock_must_not_be_fabricated_before_publication(self):
        self.assertFalse((ROOT / "ci/dependencies/lock.json").exists())


if __name__ == "__main__":
    unittest.main()
